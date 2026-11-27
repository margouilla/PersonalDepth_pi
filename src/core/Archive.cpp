// PersonalDepth_pi - extraction des archives téléchargées (.zip, .7z)
// Copyright (C) 2026 Marcaurelio Franzetti
// SPDX-License-Identifier: GPL-2.0-or-later
#include "Archive.h"

#include <cstdio>
#include <cstring>
#include <filesystem>

#include "FileUtil.h"
#include "Lang.h"
#include "miniz.h"

extern "C" {
#include "7z.h"
#include "7zAlloc.h"
#include "7zCrc.h"
}

namespace fs = std::filesystem;

namespace pd {

namespace {

bool ReadAll(const std::string& path, std::vector<uint8_t>& b, std::string& err) {
  FILE* f = OpenFile(path, "rb");
  if (!f) { err = tr("Impossible d'ouvrir ") + path; return false; }
  const long long n = FileSize(path);  // ftell : 32 bits sous Windows
  if (n < 0) { std::fclose(f); err = tr("Impossible de lire ") + path; return false; }
  b.resize((size_t)n);
  const bool ok = n == 0 || std::fread(b.data(), 1, (size_t)n, f) == (size_t)n;
  std::fclose(f);
  if (!ok) err = tr("Impossible de lire ") + path;
  return ok;
}

bool WriteAll(const std::string& path, const void* d, size_t n, std::string& err) {
  std::error_code ec;
  fs::create_directories(U8Path(path).parent_path(), ec);
  FILE* f = OpenFile(path, "wb");
  if (!f) { err = tr("Impossible d'écrire ") + path; return false; }
  const bool ok = n == 0 || std::fwrite(d, 1, n, f) == n;
  if (std::fclose(f) != 0 || !ok) { err = tr("Impossible d'écrire ") + path; return false; }
  return true;
}

uint16_t U16(const uint8_t* p) { return (uint16_t)(p[0] | p[1] << 8); }
uint32_t U32(const uint8_t* p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }

std::string Utf16ToUtf8(const UInt16* s) {
  std::string o;
  for (size_t i = 0; s[i]; ++i) {
    unsigned c = s[i];
    if (c >= 0xD800 && c < 0xDC00 && s[i + 1] >= 0xDC00 && s[i + 1] < 0xE000) {
      c = 0x10000 + ((c - 0xD800) << 10) + (s[i + 1] - 0xDC00);
      ++i;
    }
    if (c < 0x80) o += (char)c;
    else if (c < 0x800) { o += (char)(0xC0 | (c >> 6)); o += (char)(0x80 | (c & 63)); }
    else if (c < 0x10000) { o += (char)(0xE0 | (c >> 12)); o += (char)(0x80 | ((c >> 6) & 63)); o += (char)(0x80 | (c & 63)); }
    else {
      o += (char)(0xF0 | (c >> 18)); o += (char)(0x80 | ((c >> 12) & 63));
      o += (char)(0x80 | ((c >> 6) & 63)); o += (char)(0x80 | (c & 63));
    }
  }
  return o;
}

// ---------------------------------------------------------------- zip
struct ZipItem {
  ArchiveEntry e;
  uint16_t method = 0;
  uint32_t csize = 0;
  uint32_t local = 0;  // position de l'entête locale
};

bool ZipDirectory(const std::vector<uint8_t>& z, std::vector<ZipItem>& out, std::string& err) {
  // fin du répertoire central (cherchée depuis la fin : commentaire < 64 Ko)
  if (z.size() < 22) { err = tr("Archive zip illisible"); return false; }
  size_t eocd = std::string::npos;
  for (size_t i = z.size() - 22 + 1; i-- > 0 && z.size() - i <= 22 + 65535;)
    if (U32(&z[i]) == 0x06054b50) { eocd = i; break; }
  if (eocd == std::string::npos) { err = tr("Archive zip illisible"); return false; }
  const uint16_t n = U16(&z[eocd + 10]);
  size_t p = U32(&z[eocd + 16]);
  if (U32(&z[eocd + 16]) == 0xFFFFFFFFu || n == 0xFFFF) {
    err = tr("Archive zip de plus de 4 Go (zip64) non prise en charge");
    return false;
  }
  for (uint16_t k = 0; k < n; ++k) {
    if (p + 46 > z.size() || U32(&z[p]) != 0x02014b50) { err = tr("Archive zip illisible"); return false; }
    ZipItem it;
    it.method = U16(&z[p + 10]);
    it.csize = U32(&z[p + 20]);
    it.e.size = U32(&z[p + 24]);
    const uint16_t nl = U16(&z[p + 28]), xl = U16(&z[p + 30]), cl = U16(&z[p + 32]);
    it.local = U32(&z[p + 42]);
    if (p + 46 + nl > z.size()) { err = tr("Archive zip illisible"); return false; }
    const std::string raw((const char*)&z[p + 46], nl);
    it.e.dir = !raw.empty() && (raw.back() == '/' || raw.back() == '\\');
    it.e.name = SafeEntryName(raw);
    if (!it.e.name.empty()) out.push_back(it);
    p += 46 + nl + xl + cl;
  }
  return true;
}

bool ZipData(const std::vector<uint8_t>& z, const ZipItem& it, std::vector<uint8_t>& out, std::string& err) {
  const size_t p = it.local;
  if (p + 30 > z.size() || U32(&z[p]) != 0x04034b50) { err = tr("Archive zip illisible"); return false; }
  const size_t d = p + 30 + U16(&z[p + 26]) + U16(&z[p + 28]);
  if (d + it.csize > z.size()) { err = tr("Archive zip tronquée"); return false; }
  if (it.method == 0) {
    out.assign(z.begin() + d, z.begin() + d + it.csize);
  } else if (it.method == 8) {
    out.resize((size_t)it.e.size);
    const size_t got = tinfl_decompress_mem_to_mem(out.data(), out.size(), &z[d], it.csize, 0);
    if (got == TINFL_DECOMPRESS_MEM_TO_MEM_FAILED || got != out.size()) {
      err = tr("Archive zip : données compressées illisibles (") + it.e.name + ")";
      return false;
    }
  } else {
    err = tr("Archive zip : méthode de compression non prise en charge (") + it.e.name + ")";
    return false;
  }
  return true;
}

// ---------------------------------------------------------------- 7z
struct MemSeek {
  ISeekInStream vt;  // premier membre : le pointeur vers vt est celui de l'objet
  const uint8_t* d;
  size_t n, pos;
};

SRes MemRead(ISeekInStreamPtr p, void* buf, size_t* size) {
  MemSeek* m = (MemSeek*)(void*)p;
  const size_t k = std::min(*size, m->n - m->pos);
  std::memcpy(buf, m->d + m->pos, k);
  m->pos += k;
  *size = k;
  return SZ_OK;
}

SRes MemSeekFn(ISeekInStreamPtr p, Int64* pos, ESzSeek origin) {
  MemSeek* m = (MemSeek*)(void*)p;
  Int64 b = origin == SZ_SEEK_SET ? 0 : origin == SZ_SEEK_CUR ? (Int64)m->pos : (Int64)m->n;
  const Int64 np = b + *pos;
  if (np < 0 || np > (Int64)m->n) return SZ_ERROR_PARAM;
  m->pos = (size_t)np;
  *pos = np;
  return SZ_OK;
}

const ISzAlloc kAlloc = {SzAlloc, SzFree};
const ISzAlloc kAllocTemp = {SzAllocTemp, SzFreeTemp};

class SevenZip {
 public:
  explicit SevenZip(const std::vector<uint8_t>& d) {
    static bool crc = (CrcGenerateTable(), true);
    (void)crc;
    mem_.vt.Read = MemRead;
    mem_.vt.Seek = MemSeekFn;
    mem_.d = d.data();
    mem_.n = d.size();
    mem_.pos = 0;
    LookToRead2_CreateVTable(&look_, False);
    buf_.resize(1 << 16);
    look_.buf = buf_.data();
    look_.bufSize = buf_.size();
    look_.realStream = &mem_.vt;
    LookToRead2_INIT(&look_)
    SzArEx_Init(&db_);
  }
  ~SevenZip() {
    if (out_) ISzAlloc_Free(&kAlloc, out_);
    SzArEx_Free(&db_, &kAlloc);
  }
  bool open(std::string& err) {
    const SRes r = SzArEx_Open(&db_, &look_.vt, &kAlloc, &kAllocTemp);
    if (r != SZ_OK) { err = Error(r); return false; }
    return true;
  }
  UInt32 count() const { return db_.NumFiles; }
  ArchiveEntry entry(UInt32 i) const {
    ArchiveEntry e;
    const size_t len = SzArEx_GetFileNameUtf16(&db_, i, nullptr);
    std::vector<UInt16> n(len + 1, 0);
    SzArEx_GetFileNameUtf16(&db_, i, n.data());
    e.name = SafeEntryName(Utf16ToUtf8(n.data()));
    e.dir = SzArEx_IsDir(&db_, i);
    e.size = e.dir ? 0 : SzArEx_GetFileSize(&db_, i);
    return e;
  }
  bool extract(UInt32 i, const uint8_t*& d, size_t& n, std::string& err) {
    size_t off = 0, sz = 0;
    const SRes r = SzArEx_Extract(&db_, &look_.vt, i, &block_, &out_, &outSize_, &off, &sz, &kAlloc, &kAllocTemp);
    if (r != SZ_OK) { err = Error(r); return false; }
    d = out_ + off;
    n = sz;
    return true;
  }
  static std::string Error(SRes r) {
    switch (r) {
      case SZ_ERROR_UNSUPPORTED: return tr("Archive 7z : méthode de compression non prise en charge");
      case SZ_ERROR_MEM: return tr("Archive 7z : mémoire insuffisante");
      case SZ_ERROR_CRC: return tr("Archive 7z : somme de contrôle fausse (fichier abîmé)");
      default: return std::string(tr("Archive 7z illisible")) + " (" + std::to_string(r) + ")";
    }
  }

 private:
  MemSeek mem_;
  CLookToRead2 look_;
  std::vector<Byte> buf_;
  CSzArEx db_;
  UInt32 block_ = 0xFFFFFFFF;
  Byte* out_ = nullptr;
  size_t outSize_ = 0;
};

std::string Target(const std::string& dest, const std::string& name, bool flatten) {
  std::string rel = name;
  if (flatten) {
    const size_t s = rel.find_last_of('/');
    if (s != std::string::npos) rel = rel.substr(s + 1);
  }
  return (U8Path(dest) / U8Path(rel)).u8string();
}

}  // namespace

std::string SafeEntryName(const std::string& name) {
  std::string out, part;
  auto flush = [&]() -> bool {
    if (part.empty() || part == ".") { part.clear(); return true; }
    if (part == ".." || part.find(':') != std::string::npos) return false;
    if (!out.empty()) out += '/';
    out += part;
    part.clear();
    return true;
  };
  for (char c : name) {
    if (c == '/' || c == '\\') { if (!flush()) return std::string(); }
    else if ((unsigned char)c < 32) return std::string();
    else part += c;
  }
  if (!flush()) return std::string();
  return out;
}

ArchiveKind DetectArchive(const std::string& path) {
  FILE* f = OpenFile(path, "rb");
  if (!f) return ArchiveKind::None;
  uint8_t h[6] = {0};
  const size_t n = std::fread(h, 1, 6, f);
  std::fclose(f);
  if (n >= 4 && h[0] == 'P' && h[1] == 'K' && h[2] == 3 && h[3] == 4) return ArchiveKind::Zip;
  static const uint8_t k7z[6] = {'7', 'z', 0xBC, 0xAF, 0x27, 0x1C};
  if (n == 6 && std::memcmp(h, k7z, 6) == 0) return ArchiveKind::SevenZip;
  return ArchiveKind::None;
}

bool ListArchive(const std::string& path, std::vector<ArchiveEntry>& out, std::string& err) {
  out.clear();
  const ArchiveKind k = DetectArchive(path);
  std::vector<uint8_t> d;
  if (k == ArchiveKind::None) { err = tr("Ce fichier n'est pas une archive .zip ou .7z : ") + path; return false; }
  if (!ReadAll(path, d, err)) return false;
  if (k == ArchiveKind::Zip) {
    std::vector<ZipItem> items;
    if (!ZipDirectory(d, items, err)) return false;
    for (const auto& it : items) out.push_back(it.e);
    return true;
  }
  SevenZip z(d);
  if (!z.open(err)) return false;
  for (UInt32 i = 0; i < z.count(); ++i) {
    ArchiveEntry e = z.entry(i);
    if (!e.name.empty()) out.push_back(e);
  }
  return true;
}

bool ExtractArchive(const std::string& path, const std::string& destDir,
                    const std::function<bool(const std::string&)>& want, bool flatten,
                    std::vector<std::string>& written, std::string& err) {
  written.clear();
  const ArchiveKind k = DetectArchive(path);
  if (k == ArchiveKind::None) { err = tr("Ce fichier n'est pas une archive .zip ou .7z : ") + path; return false; }
  std::vector<uint8_t> d;
  if (!ReadAll(path, d, err)) return false;
  if (k == ArchiveKind::Zip) {
    std::vector<ZipItem> items;
    if (!ZipDirectory(d, items, err)) return false;
    std::vector<uint8_t> buf;
    for (const auto& it : items) {
      if (it.e.dir || (want && !want(it.e.name))) continue;
      if (!ZipData(d, it, buf, err)) return false;
      const std::string t = Target(destDir, it.e.name, flatten);
      if (!WriteAll(t, buf.data(), buf.size(), err)) return false;
      written.push_back(t);
    }
    return true;
  }
  SevenZip z(d);
  if (!z.open(err)) return false;
  for (UInt32 i = 0; i < z.count(); ++i) {
    const ArchiveEntry e = z.entry(i);
    if (e.name.empty() || e.dir || (want && !want(e.name))) continue;
    const uint8_t* p = nullptr;
    size_t n = 0;
    if (!z.extract(i, p, n, err)) return false;
    const std::string t = Target(destDir, e.name, flatten);
    if (!WriteAll(t, p, n, err)) return false;
    written.push_back(t);
  }
  return true;
}

}  // namespace pd

// PersonalDepth_pi - personal bathymetry plugin for OpenCPN
// Copyright (C) 2026 Marcaurelio Franzetti <marcaurelio.franzetti@gmail.com>
//
// This program is free software; you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation; either version 2 of the License, or
// (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// SPDX-License-Identifier: GPL-2.0-or-later

#include "GeoTiff.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <sstream>

#include "FileUtil.h"
#include "Inflate.h"
#include "Lang.h"
#include "PointCloud.h"
#include "Projection.h"

namespace pd {

namespace {

void Seek64(FILE* f, long long o) {
#if defined(_WIN32)
  _fseeki64(f, o, SEEK_SET);
#else
  fseeko(f, (off_t)o, SEEK_SET);
#endif
}

bool HostLittle() {
  const uint16_t v = 1;
  uint8_t b;
  std::memcpy(&b, &v, 1);
  return b == 1;
}

// TIFF LZW (codes 9 à 12 bits, poids forts d'abord, changement anticipé)
bool LzwDecode(const uint8_t* src, size_t n, std::vector<uint8_t>& out, size_t expected) {
  out.clear();
  out.reserve(expected);
  struct Entry {
    int prefix;
    uint8_t first, last;
    int len;
  };
  std::vector<Entry> tab(4096);
  for (int k = 0; k < 256; ++k) tab[k] = {-1, (uint8_t)k, (uint8_t)k, 1};
  int next = 258, width = 9, prev = -1;
  uint32_t acc = 0;
  int nbits = 0;
  size_t pos = 0;
  std::vector<uint8_t> tmp;
  auto emit = [&](int code) {
    const int len = tab[code].len;
    tmp.resize(len);
    for (int c = code, k = len - 1; k >= 0; --k) {
      tmp[k] = tab[c].last;
      c = tab[c].prefix;
      if (k > 0 && c < 0) return false;
    }
    out.insert(out.end(), tmp.begin(), tmp.end());
    return true;
  };
  for (;;) {
    while (nbits < width) {
      if (pos >= n) return !out.empty();
      acc = (acc << 8) | src[pos++];
      nbits += 8;
    }
    const int code = (int)((acc >> (nbits - width)) & ((1u << width) - 1));
    nbits -= width;
    if (code == 257) break;  // fin
    if (code == 256) {       // remise à zéro
      next = 258;
      width = 9;
      prev = -1;
      continue;
    }
    if (prev < 0) {
      if (code > 255) return false;
      out.push_back((uint8_t)code);
      prev = code;
      continue;
    }
    if (code < next) {
      if (!emit(code)) return false;
      if (next < 4096) {
        tab[next] = {prev, tab[prev].first, tab[code].first, tab[prev].len + 1};
        ++next;
      }
    } else if (code == next && next < 4096) {
      tab[next] = {prev, tab[prev].first, tab[prev].first, tab[prev].len + 1};
      ++next;
      if (!emit(code)) return false;
    } else {
      return false;
    }
    prev = code;
    if (next >= 511 && width == 9) width = 10;
    if (next >= 1023 && width == 10) width = 11;
    if (next >= 2047 && width == 11) width = 12;
    if (expected && out.size() >= expected) break;
  }
  return true;
}

bool PackBitsDecode(const uint8_t* src, size_t n, std::vector<uint8_t>& out, size_t expected) {
  out.clear();
  out.reserve(expected);
  size_t i = 0;
  while (i < n && (!expected || out.size() < expected)) {
    const int c = (int8_t)src[i++];
    if (c >= 0) {
      const size_t k = (size_t)c + 1;
      if (i + k > n) return false;
      out.insert(out.end(), src + i, src + i + k);
      i += k;
    } else if (c != -128) {
      if (i >= n) return false;
      out.insert(out.end(), (size_t)(1 - c), src[i++]);
    }
  }
  return true;
}

}  // namespace

struct GeoTiff::Impl {
  FILE* f = nullptr;
  bool be = false, big = false;
  std::string path;
  std::vector<uint64_t> off, cnt;  // blocs (bandes ou tuiles)
  int bytes = 0;                   // octets par échantillon
  int blocksAcross = 1;
  // cache : une rangée de blocs décodée (première bande, réels)
  int cachedBand = -1;
  std::vector<float> rows;  // largeur × blockH
  std::vector<uint8_t> comp, raw;

  ~Impl() {
    if (f) std::fclose(f);
  }
  uint16_t u16(const uint8_t* p) const { return be ? (uint16_t)(p[0] << 8 | p[1]) : (uint16_t)(p[1] << 8 | p[0]); }
  uint32_t u32(const uint8_t* p) const {
    return be ? ((uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3])
              : ((uint32_t)p[3] << 24 | (uint32_t)p[2] << 16 | (uint32_t)p[1] << 8 | p[0]);
  }
  uint64_t u64(const uint8_t* p) const {
    uint64_t a = u32(p), b = u32(p + 4);
    return be ? (a << 32 | b) : (b << 32 | a);
  }
  bool readAt(uint64_t o, void* dst, size_t n) {
    Seek64(f, (long long)o);
    return std::fread(dst, 1, n, f) == n;
  }
};

GeoTiff::GeoTiff() : impl_(new Impl) {}
GeoTiff::~GeoTiff() = default;

bool IsGeoTiffFile(const std::string& path) {
  const size_t d = path.find_last_of('.');
  if (d == std::string::npos) return false;
  std::string e = path.substr(d + 1);
  std::transform(e.begin(), e.end(), e.begin(), ::tolower);
  return e == "tif" || e == "tiff";
}

namespace {

// Projection définie dans les GeoKeys (ProjectedCSType = 32767) -> chaîne +proj
std::string UserProjection(const std::map<int, double>& k, std::string& label) {
  auto has = [&](int id) { return k.count(id) != 0; };
  auto v = [&](int id, double d = 0) { return has(id) ? k.at(id) : d; };
  std::ostringstream s;
  s.precision(12);
  const int ct = (int)v(3075);
  if (ct == 7) {  // Mercator (1SP ou 2SP)
    double latts = has(3078) ? v(3078) : v(3081);
    s << "+proj=merc +lon_0=" << (has(3080) ? v(3080) : v(3088));
    if (latts != 0) s << " +lat_ts=" << latts;
    else s << " +k_0=" << v(3092, 1);
    s << " +x_0=" << v(3082) << " +y_0=" << v(3083);
    char b[64];
    std::snprintf(b, sizeof b, "Mercator %g°", latts);
    label = b;
  } else if (ct == 1) {  // Mercator transverse
    s << "+proj=tmerc +lat_0=" << v(3081) << " +lon_0=" << v(3080) << " +k_0=" << v(3092, 1)
      << " +x_0=" << v(3082) << " +y_0=" << v(3083);
    label = "Transverse Mercator";
  } else if (ct == 8) {  // Lambert conique conforme, 2 parallèles
    s << "+proj=lcc +lat_1=" << v(3078) << " +lat_2=" << v(3079) << " +lat_0=" << v(3085, v(3081))
      << " +lon_0=" << v(3084, v(3080)) << " +x_0=" << v(3086, v(3082)) << " +y_0=" << v(3087, v(3083));
    label = "Lambert 2SP";
  } else if (ct == 9) {  // Lambert conique conforme, 1 parallèle
    s << "+proj=lcc +lat_1=" << v(3081) << " +lat_0=" << v(3081) << " +lon_0=" << v(3080) << " +k_0="
      << v(3092, 1) << " +x_0=" << v(3082) << " +y_0=" << v(3083);
    label = "Lambert 1SP";
  } else {
    return std::string();
  }
  // ellipsoïde : demi-grand axe et aplatissement inverse, sinon WGS84 / GRS80
  if (has(2057) && (has(2059) || has(2058))) {
    s << " +a=" << v(2057);
    if (has(2059)) s << " +rf=" << v(2059);
    else s << " +b=" << v(2058);
  } else {
    s << ((int)v(2056) == 7019 ? " +ellps=GRS80" : " +ellps=WGS84");
  }
  const int u = (int)v(3076, 9001);
  if (u == 9002) s << " +units=ft";
  else if (u == 9003) s << " +units=us-ft";
  return s.str();
}

}  // namespace

bool GeoTiff::open(const std::string& path, std::string& err) {
  Impl& I = *impl_;
  GeoTiffInfo& G = info_;
  I.path = path;
  I.f = OpenFile(path, "rb");
  if (!I.f) { err = pd::tr("Impossible d'ouvrir ") + path; return false; }
  uint8_t h[16];
  if (std::fread(h, 1, 8, I.f) != 8) { err = pd::tr("Fichier TIFF illisible"); return false; }
  if (h[0] == 'I' && h[1] == 'I') I.be = false;
  else if (h[0] == 'M' && h[1] == 'M') I.be = true;
  else { err = pd::tr("Fichier TIFF illisible"); return false; }
  const int magic = I.u16(h + 2);
  uint64_t ifd;
  if (magic == 42) {
    ifd = I.u32(h + 4);
  } else if (magic == 43) {
    I.big = true;
    if (std::fread(h + 8, 1, 8, I.f) != 8) { err = pd::tr("Fichier TIFF illisible"); return false; }
    ifd = I.u64(h + 8);
  } else {
    err = pd::tr("Fichier TIFF illisible");
    return false;
  }
  // première image (IFD) : balises
  uint8_t nb[8];
  if (!I.readAt(ifd, nb, I.big ? 8 : 2)) { err = pd::tr("Fichier TIFF illisible"); return false; }
  const uint64_t n = I.big ? I.u64(nb) : I.u16(nb);
  if (n == 0 || n > 4096) { err = pd::tr("Fichier TIFF illisible"); return false; }
  const size_t esz = I.big ? 20 : 12;
  std::vector<uint8_t> ents((size_t)n * esz);
  if (!I.readAt(ifd + (I.big ? 8 : 2), ents.data(), ents.size())) { err = pd::tr("Fichier TIFF illisible"); return false; }
  std::map<int, std::vector<double>> num;  // valeurs numériques
  std::map<int, std::vector<uint64_t>> arr;  // tableaux d'entiers (positions des blocs)
  std::map<int, std::string> txt;
  static const int tsize[] = {0, 1, 1, 2, 4, 8, 1, 1, 2, 4, 8, 4, 8, 4, 0, 0, 8, 8, 8};
  for (uint64_t e = 0; e < n; ++e) {
    const uint8_t* p = ents.data() + e * esz;
    const int tag = I.u16(p), typ = I.u16(p + 2);
    const uint64_t count = I.big ? I.u64(p + 4) : I.u32(p + 8 - 4);
    const uint8_t* val = p + (I.big ? 12 : 8);
    if (typ <= 0 || typ > 18 || !tsize[typ] || count > ((uint64_t)1 << 28)) continue;
    const uint64_t len = count * (uint64_t)tsize[typ];
    std::vector<uint8_t> data;
    if (len <= (uint64_t)(I.big ? 8 : 4)) {
      data.assign(val, val + len);
    } else {
      static const int wanted[] = {258, 273, 279, 320, 324, 325, 339, 33550, 33922, 34264, 34735, 34736, 34737, 42113};
      if (std::find(std::begin(wanted), std::end(wanted), tag) == std::end(wanted)) continue;
      data.resize((size_t)len);
      if (!I.readAt(I.big ? I.u64(val) : I.u32(val), data.data(), data.size())) continue;
    }
    if (typ == 2) {
      txt[tag].assign(data.begin(), data.end());
      continue;
    }
    std::vector<double>& out = num[tag];
    std::vector<uint64_t>& ai = arr[tag];
    for (uint64_t k = 0; k < count; ++k) {
      const uint8_t* q = data.data() + k * tsize[typ];
      double d = 0;
      uint64_t u = 0;
      switch (typ) {
        case 1: case 7: u = q[0]; d = (double)u; break;
        case 6: d = (int8_t)q[0]; break;
        case 3: u = I.u16(q); d = (double)u; break;
        case 8: d = (int16_t)I.u16(q); break;
        case 4: case 13: u = I.u32(q); d = (double)u; break;
        case 9: d = (int32_t)I.u32(q); break;
        case 16: case 18: u = I.u64(q); d = (double)u; break;
        case 17: d = (double)(int64_t)I.u64(q); break;
        case 5: d = I.u32(q + 4) ? (double)I.u32(q) / I.u32(q + 4) : 0; break;
        case 10: d = I.u32(q + 4) ? (double)(int32_t)I.u32(q) / (int32_t)I.u32(q + 4) : 0; break;
        case 11: { uint32_t b = I.u32(q); float fl; std::memcpy(&fl, &b, 4); d = fl; break; }
        case 12: { uint64_t b = I.u64(q); std::memcpy(&d, &b, 8); break; }
      }
      out.push_back(d);
      ai.push_back(u);
    }
  }
  auto first = [&](int tag, double d) { return num.count(tag) && !num[tag].empty() ? num[tag][0] : d; };
  G.width = (int)first(256, 0);
  G.height = (int)first(257, 0);
  G.bits = (int)first(258, 1);
  G.samples = (int)first(277, 1);
  G.sampleFormat = (int)first(339, 1);
  G.compression = (int)first(259, 1);
  G.predictor = (int)first(317, 1);
  G.planar = (int)first(284, 1);
  G.photometric = (int)first(262, 1);
  if (G.width <= 0 || G.height <= 0) { err = pd::tr("Fichier TIFF illisible"); return false; }
  if (G.bits != 8 && G.bits != 16 && G.bits != 32 && G.bits != 64) {
    char b[96];
    std::snprintf(b, sizeof b, pd::tr("TIFF : %d bits par valeur non pris en charge"), G.bits);
    err = b;
    return false;
  }
  if (G.compression != 1 && G.compression != 5 && G.compression != 8 && G.compression != 32946 &&
      G.compression != 32773) {
    char b[128];
    std::snprintf(b, sizeof b, pd::tr("TIFF : compression %d non prise en charge (sans, LZW, Deflate, PackBits)"),
                  G.compression);
    err = b;
    return false;
  }
  I.bytes = G.bits / 8;
  G.tiled = num.count(322) && num.count(324);
  if (G.tiled) {
    G.blockW = (int)first(322, 0);
    G.blockH = (int)first(323, 0);
    I.off = arr[324];
    I.cnt = arr[325];
  } else {
    G.blockW = G.width;
    G.blockH = (int)std::min<double>(first(278, G.height), G.height);
    I.off = arr[273];
    I.cnt = arr[279];
  }
  if (G.blockW <= 0 || G.blockH <= 0 || I.off.empty() || I.off.size() != I.cnt.size()) {
    err = pd::tr("Fichier TIFF illisible");
    return false;
  }
  I.blocksAcross = (G.width + G.blockW - 1) / G.blockW;
  const size_t need = (size_t)I.blocksAcross * ((G.height + G.blockH - 1) / G.blockH);
  if (I.off.size() < need) { err = pd::tr("Fichier TIFF illisible"); return false; }
  if (!G.tiled && G.compression == 1 && G.blockH > 1) {  // sans compression : lecture ligne par ligne
    const uint64_t rowBytes = (uint64_t)G.width * I.bytes * (G.planar == 2 ? 1 : G.samples);
    std::vector<uint64_t> off((size_t)G.height), cnt((size_t)G.height, rowBytes);
    for (int r = 0; r < G.height; ++r) off[r] = I.off[r / G.blockH] + (uint64_t)(r % G.blockH) * rowBytes;
    I.off.swap(off);
    I.cnt.swap(cnt);
    G.blockH = 1;
  }
  if (G.photometric == 3 && num.count(320)) {
    for (double v : num[320]) G.palette.push_back((uint16_t)v);
  }
  if (txt.count(42113)) {  // GDAL_NODATA
    const std::string& s = txt[42113];
    char* end = nullptr;
    const double v = std::strtod(s.c_str(), &end);
    if (end != s.c_str()) { G.hasNoData = true; G.noData = v; }
  }
  // Géoréférencement
  if (num.count(33550) && num[33550].size() >= 2 && num.count(33922) && num[33922].size() >= 6) {
    const auto& sc = num[33550];
    const auto& tp = num[33922];
    G.dx = sc[0];
    G.dy = sc[1];
    G.x0 = tp[3] - tp[0] * G.dx;
    G.y0 = tp[4] + tp[1] * G.dy;
    G.georef = G.dx > 0 && G.dy > 0;
  } else if (num.count(34264) && num[34264].size() >= 16) {
    const auto& m = num[34264];
    if (m[1] == 0 && m[4] == 0 && m[0] > 0 && m[5] < 0) {
      G.dx = m[0];
      G.dy = -m[5];
      G.x0 = m[3];
      G.y0 = m[7];
      G.georef = true;
    }
  }
  if (!G.georef) {  // fichier de calage voisin (.tfw) : A D B E C F, centre du pixel (0, 0)
    for (const char* ext : {".tfw", ".tifw", ".wld"}) {
      std::string w = path.substr(0, path.find_last_of('.')) + ext;
      FILE* tf = OpenFile(w, "r");
      if (!tf) continue;
      double a[6];
      int got = 0;
      while (got < 6 && std::fscanf(tf, "%lf", &a[got]) == 1) ++got;
      std::fclose(tf);
      if (got == 6 && a[1] == 0 && a[2] == 0 && a[0] > 0 && a[3] < 0) {
        G.dx = a[0];
        G.dy = -a[3];
        G.x0 = a[4] - a[0] / 2;
        G.y0 = a[5] - a[3] / 2;
        G.georef = true;
      }
      break;
    }
  }
  // GeoKeys
  std::map<int, double> keys;
  if (num.count(34735) && num[34735].size() >= 4) {
    const auto& gk = num[34735];
    const auto& dbl = num[34736];
    const size_t nk = (size_t)gk[3];
    for (size_t i = 0; i < nk && 4 + 4 * i + 3 < gk.size(); ++i) {
      const int id = (int)gk[4 + 4 * i], loc = (int)gk[5 + 4 * i];
      const size_t vo = (size_t)gk[7 + 4 * i];
      if (loc == 0) keys[id] = (double)vo;
      else if (loc == 34736 && vo < dbl.size()) keys[id] = dbl[vo];
    }
  }
  if (keys.count(1025) && (int)keys[1025] == 2 && G.georef) {  // PixelIsPoint : point d'attache au centre
    G.x0 -= G.dx / 2;
    G.y0 += G.dy / 2;
  }
  G.verticalDepth = keys.count(4096) && (int)keys[4096] == 5861;  // « LAT depth »
  const int model = keys.count(1024) ? (int)keys[1024] : 0;
  if (model == 1) {
    G.geographic = false;
    const int pcs = keys.count(3072) ? (int)keys[3072] : 0;
    if (pcs > 0 && pcs != 32767) {
      G.crsDef = "EPSG:" + std::to_string(pcs);
      G.crsLabel = G.crsDef;
    } else {
      std::string label;
      G.crsDef = UserProjection(keys, label);
      G.crsLabel = label;
      if (G.crsDef.empty()) G.crsError = pd::tr("TIFF : projection définie par l'utilisateur non prise en charge");
    }
    if (!G.crsDef.empty()) {
      Crs c;
      std::string ce;
      if (!Crs::Parse(G.crsDef, c, ce)) {
        G.crsError = pd::tr("TIFF : système ") + G.crsLabel + pd::tr(" non reconnu");
        G.crsDef.clear();
      }
    }
  } else if (model == 2) {
    G.geographic = true;
    const int gcs = keys.count(2048) ? (int)keys[2048] : 4326;
    // WGS84 et repères équivalents au mètre près (ETRS89, NAD83, RGF93) : lon/lat directs
    if (gcs != 4326 && gcs != 4258 && gcs != 4269 && gcs != 4171 && gcs != 32767) {
      Crs c;
      std::string ce;
      if (Crs::Parse("EPSG:" + std::to_string(gcs), c, ce)) {
        G.crsDef = "EPSG:" + std::to_string(gcs);
        G.crsLabel = G.crsDef;
      }
    }
  }
  I.rows.clear();
  return true;
}

bool GeoTiff::readRowRaw(int r, float* out, std::string& err) {
  Impl& I = *impl_;
  const GeoTiffInfo& G = info_;
  if (r < 0 || r >= G.height) { err = "TIFF : ligne hors image"; return false; }
  const int band = r / G.blockH;
  const int spp = G.planar == 2 ? 1 : G.samples;
  const size_t pix = (size_t)I.bytes * spp;  // octets par pixel (bande 1 seule si plan séparé)
  const bool swap = I.bytes > 1 && (I.be == HostLittle()) && G.predictor != 3;
  if (band != I.cachedBand) {
    const int rowsIn = std::min(G.blockH, G.height - band * G.blockH);
    I.rows.assign((size_t)G.width * rowsIn, NAN);
    for (int bx = 0; bx < I.blocksAcross; ++bx) {
      const size_t id = (size_t)band * I.blocksAcross + bx;  // plan séparé : bande 1 en premier
      const int bRows = G.tiled ? G.blockH : rowsIn;
      const size_t rowBytes = (size_t)G.blockW * pix;
      const size_t expect = rowBytes * bRows;
      if (I.cnt[id] == 0) continue;  // tuile absente : vide
      I.comp.resize((size_t)I.cnt[id]);
      if (!I.readAt(I.off[id], I.comp.data(), I.comp.size())) { err = pd::tr("TIFF : lecture impossible"); return false; }
      bool ok = true;
      switch (G.compression) {
        case 1: I.raw.swap(I.comp); break;
        case 5: ok = LzwDecode(I.comp.data(), I.comp.size(), I.raw, expect); break;
        case 8: case 32946: ok = ZlibInflate(I.comp.data(), I.comp.size(), I.raw, expect); break;
        case 32773: ok = PackBitsDecode(I.comp.data(), I.comp.size(), I.raw, expect); break;
      }
      if (!ok) { err = pd::tr("TIFF : bloc compressé illisible"); return false; }
      if (I.raw.size() < expect) I.raw.resize(expect, 0);  // bloc tronqué (dernière bande) : complété
      uint8_t* d = I.raw.data();
      if (swap) {  // ordre des octets du fichier -> machine
        for (size_t k = 0; k + I.bytes <= expect; k += I.bytes) std::reverse(d + k, d + k + I.bytes);
      }
      if (G.predictor == 2) {  // différences horizontales (entiers)
        for (int y = 0; y < bRows; ++y) {
          uint8_t* row = d + (size_t)y * rowBytes;
          const size_t nv = (size_t)G.blockW * spp;
          for (size_t k = spp; k < nv; ++k) {
            uint8_t* a = row + k * I.bytes;
            const uint8_t* b = row + (k - spp) * I.bytes;
            if (I.bytes == 1) { a[0] = (uint8_t)(a[0] + b[0]); }
            else if (I.bytes == 2) { uint16_t x, y2; std::memcpy(&x, a, 2); std::memcpy(&y2, b, 2); x = (uint16_t)(x + y2); std::memcpy(a, &x, 2); }
            else if (I.bytes == 4) { uint32_t x, y2; std::memcpy(&x, a, 4); std::memcpy(&y2, b, 4); x += y2; std::memcpy(a, &x, 4); }
            else { uint64_t x, y2; std::memcpy(&x, a, 8); std::memcpy(&y2, b, 8); x += y2; std::memcpy(a, &x, 8); }
          }
        }
      } else if (G.predictor == 3) {  // prédicteur des réels : octets regroupés puis différenciés
        std::vector<uint8_t> tmp(rowBytes);
        const size_t wc = (size_t)G.blockW * spp;
        const bool little = HostLittle();
        for (int y = 0; y < bRows; ++y) {
          uint8_t* row = d + (size_t)y * rowBytes;
          for (size_t k = spp; k < rowBytes; ++k) row[k] = (uint8_t)(row[k] + row[k - spp]);
          std::memcpy(tmp.data(), row, rowBytes);
          for (size_t k = 0; k < wc; ++k)
            for (int b = 0; b < I.bytes; ++b)  // octets de poids fort d'abord
              row[k * I.bytes + (little ? I.bytes - 1 - b : b)] = tmp[(size_t)b * wc + k];
        }
      }
      // première bande -> réels
      const int x0 = bx * G.blockW;
      const int wv = std::min(G.blockW, G.width - x0);
      for (int y = 0; y < rowsIn; ++y) {
        const uint8_t* row = d + (size_t)y * rowBytes;
        float* o = I.rows.data() + (size_t)y * G.width + x0;
        for (int x = 0; x < wv; ++x) {
          const uint8_t* q = row + (size_t)x * pix;
          double v;
          if (G.sampleFormat == 3) {
            if (I.bytes == 4) { float fl; std::memcpy(&fl, q, 4); v = fl; }
            else if (I.bytes == 8) { std::memcpy(&v, q, 8); }
            else { v = NAN; }
          } else if (G.sampleFormat == 2) {
            if (I.bytes == 1) v = (int8_t)q[0];
            else if (I.bytes == 2) { int16_t t; std::memcpy(&t, q, 2); v = t; }
            else if (I.bytes == 4) { int32_t t; std::memcpy(&t, q, 4); v = t; }
            else { int64_t t; std::memcpy(&t, q, 8); v = (double)t; }
          } else {
            if (I.bytes == 1) v = q[0];
            else if (I.bytes == 2) { uint16_t t; std::memcpy(&t, q, 2); v = t; }
            else if (I.bytes == 4) { uint32_t t; std::memcpy(&t, q, 4); v = t; }
            else { uint64_t t; std::memcpy(&t, q, 8); v = (double)t; }
          }
          o[x] = (float)v;
        }
      }
      if (G.compression == 1) I.comp.swap(I.raw);
    }
    I.cachedBand = band;
  }
  std::memcpy(out, I.rows.data() + (size_t)(r - band * G.blockH) * G.width, sizeof(float) * G.width);
  return true;
}

bool GeoTiff::readRow(int r, float* out, std::string& err) {
  if (!readRowRaw(r, out, err)) return false;
  const GeoTiffInfo& G = info_;
  for (int x = 0; x < G.width; ++x) {
    const float v = out[x];
    if (!std::isfinite(v) ||
        (G.hasNoData && std::fabs(v - G.noData) <= 1e-6 * std::max(1.0, std::fabs(G.noData))))
      out[x] = NAN;
  }
  return true;
}

bool GeoTiffCrs(const GeoTiffInfo& G, const std::shared_ptr<const Crs>& user,
                std::shared_ptr<const Crs>& crs, std::string& used, std::string& err) {
  crs = user;
  used.clear();
  if (crs) {
    used = crs->definition();
    return true;
  }
  if (!G.crsError.empty()) {
    err = G.crsError + pd::tr(" : choisir le système à l'import");
    return false;
  }
  if (!G.crsDef.empty()) {
    auto c = std::make_shared<Crs>();
    std::string ce;
    if (!Crs::Parse(G.crsDef, *c, ce)) { err = ce; return false; }
    if (!c->isWGS84LonLat()) crs = c;
    used = G.crsLabel;
  }
  return true;
}

bool ForEachGeoTiffRow(GeoTiff& t, const std::shared_ptr<const Crs>& crs, const GeoTiffRowFn& fn,
                       std::string& err, const std::atomic<bool>* cancel, bool raw) {
  const GeoTiffInfo& G = t.info();
  if (!G.georef) {
    err = pd::tr("TIFF sans géoréférencement (ni GeoTIFF ni fichier .tfw)");
    return false;
  }
  std::vector<float> row(G.width);
  std::vector<double> lon(G.width), lat(G.width);
  // grille projetée : conversion exacte d'un nœud sur kStep le long de la
  // ligne, interpolation linéaire entre les deux (comme les grilles ASC)
  const int kStep = 32;
  const long nn = (G.width - 1) / kStep + 2;
  std::vector<double> nlon(nn), nlat(nn);
  std::vector<uint8_t> nok(nn);
  for (int r = 0; r < G.height; ++r) {
    if (cancel && *cancel && (r & 63) == 0) { err = "Annulé"; return false; }
    if (!(raw ? t.readRowRaw(r, row.data(), err) : t.readRow(r, row.data(), err))) return false;
    const double y = G.y0 - (r + 0.5) * G.dy;
    if (!crs) {
      for (int c = 0; c < G.width; ++c) { lon[c] = G.x0 + (c + 0.5) * G.dx; lat[c] = y; }
    } else {
      for (long k = 0; k < nn; ++k) {
        const long c = std::min<long>(k * kStep, G.width - 1);
        nok[k] = crs->toWGS84(G.x0 + (c + 0.5) * G.dx, y, nlon[k], nlat[k]) ? 1 : 0;
      }
      for (int c = 0; c < G.width; ++c) {
        const long k = c / kStep;
        const long c0 = k * kStep, c1 = std::min<long>((k + 1) * kStep, G.width - 1);
        if (c == c0 && nok[k]) {
          lon[c] = nlon[k]; lat[c] = nlat[k];
        } else if (nok[k] && nok[k + 1] && c1 > c0) {
          const double tt = double(c - c0) / double(c1 - c0);
          lon[c] = nlon[k] + tt * (nlon[k + 1] - nlon[k]);
          lat[c] = nlat[k] + tt * (nlat[k + 1] - nlat[k]);
        } else if (!crs->toWGS84(G.x0 + (c + 0.5) * G.dx, y, lon[c], lat[c])) {
          lon[c] = lat[c] = NAN;  // hors du domaine de la projection
        }
      }
    }
    if (!fn(r, row.data(), lon.data(), lat.data())) return true;
  }
  return true;
}

bool LoadGeoTiff(const std::string& path, const LoadOptions& opt, PointCloud& pc, double& cellLon,
                 double& cellLat, std::string& crsUsed, std::string& err, const std::atomic<bool>* cancel) {
  GeoTiff t;
  if (!t.open(path, err)) return false;
  const GeoTiffInfo& G = t.info();
  std::shared_ptr<const Crs> crs;
  if (!GeoTiffCrs(G, opt.crs, crs, crsUsed, err)) return false;
  pc.clear();
  pc.reserve(std::min<size_t>((size_t)G.width * G.height, (size_t)1 << 26));
  if (!ForEachGeoTiffRow(t, crs, [&](int, const float* v, const double* lon, const double* lat) {
        for (int c = 0; c < G.width; ++c)
          if (!std::isnan(v[c]) && !std::isnan(lon[c])) pc.add(lon[c], lat[c], v[c]);
        return true;
      }, err, cancel))
    return false;
  if (pc.empty()) { err = pd::tr("Grille vide"); return false; }
  cellLon = G.dx;
  cellLat = G.dy;
  if (crs && !crs->isGeographic()) {
    pc.srcStepX = G.dx * crs->unitToMeter();
    pc.srcStepY = G.dy * crs->unitToMeter();
  }
  pc.applySign(opt);
  return true;
}

}  // namespace pd

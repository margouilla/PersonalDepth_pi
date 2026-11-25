// PersonalDepth_pi - lecture des fichiers netCDF-3 et netCDF-4/HDF5 (voir NetCdf.h)
//
// Références : « HDF5 File Format Specification Version 3.0 » (The HDF Group)
// et « NetCDF Classic and 64-bit Offset File Formats » (Unidata).
//
// Copyright (C) 2026 Marcaurelio Franzetti
// SPDX-License-Identifier: GPL-2.0-or-later
#include "NetCdf.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <list>
#include <map>
#include <stdexcept>
#include <unordered_map>

#include "FileUtil.h"
#include "Lang.h"
#include "miniz/miniz.h"

namespace pd {

// ---------------------------------------------------------------- outils

namespace {

constexpr uint64_t kUndef = ~0ull;

struct Fail : std::runtime_error {
  using std::runtime_error::runtime_error;
};

[[noreturn]] void Unsupported(const std::string& what) {
  throw Fail(std::string(tr("netCDF/HDF5 : structure non prise en charge")) + " (" + what + ")");
}
[[noreturn]] void Corrupt(const std::string& what) {
  throw Fail(std::string(tr("netCDF/HDF5 : fichier tronqué ou endommagé")) + " (" + what + ")");
}

class Src {
 public:
  ~Src() { close(); }
  bool open(const std::string& path) {
    close();
    f_ = OpenFile(path, "rb");
    if (!f_) return false;
    seek(0, SEEK_END);
#ifdef _WIN32
    size_ = (uint64_t)_ftelli64(f_);
#else
    size_ = (uint64_t)ftello(f_);
#endif
    return true;
  }
  void close() {
    if (f_) std::fclose(f_);
    f_ = nullptr;
  }
  uint64_t size() const { return size_; }
  void read(uint64_t pos, void* buf, size_t n) {
    if (n == 0) return;
    if (pos > size_ || n > size_ - pos) Corrupt("lecture au-delà de la fin du fichier");
    if (!seek(pos, SEEK_SET) || std::fread(buf, 1, n, f_) != n) Corrupt("lecture impossible");
  }
  std::vector<uint8_t> read(uint64_t pos, size_t n) {
    if (pos > size_ || n > size_ - pos) Corrupt("lecture au-delà de la fin du fichier");
    std::vector<uint8_t> v(n);
    read(pos, v.data(), n);
    return v;
  }
  // Lit au plus n octets (moins près de la fin du fichier)
  std::vector<uint8_t> readUpTo(uint64_t pos, size_t n) {
    if (pos >= size_) Corrupt("adresse hors du fichier");
    return read(pos, (size_t)std::min<uint64_t>(n, size_ - pos));
  }

 private:
  bool seek(uint64_t pos, int whence) {
#ifdef _WIN32
    return _fseeki64(f_, (long long)pos, whence) == 0;
#else
    return fseeko(f_, (off_t)pos, whence) == 0;
#endif
  }
  FILE* f_ = nullptr;
  uint64_t size_ = 0;
};

// Lecteur petit-boutiste d'un tampon
struct Rd {
  const uint8_t* p;
  size_t n, i = 0;
  Rd(const uint8_t* p_, size_t n_) : p(p_), n(n_) {}
  explicit Rd(const std::vector<uint8_t>& v) : p(v.data()), n(v.size()) {}
  void need(size_t k) const {
    if (i + k > n) Corrupt("structure incomplète");
  }
  uint64_t u(int bytes) {
    if (bytes < 0 || bytes > 8) Corrupt("champ de taille invalide");
    need(bytes);
    uint64_t v = 0;
    for (int k = 0; k < bytes; ++k) v |= (uint64_t)p[i + k] << (8 * k);
    i += bytes;
    return v;
  }
  // Adresse ou longueur de `bytes` octets ; tous les bits à 1 = indéfinie
  uint64_t addr(int bytes) {
    need(bytes);
    bool all = true;
    for (int k = 0; k < bytes; ++k) all = all && p[i + k] == 0xFF;
    uint64_t v = u(bytes);
    return all ? kUndef : v;
  }
  uint8_t u8() { return (uint8_t)u(1); }
  uint16_t u16() { return (uint16_t)u(2); }
  uint32_t u32() { return (uint32_t)u(4); }
  void skip(size_t k) {
    need(k);
    i += k;
  }
  bool sig(const char* s) {
    need(4);
    bool ok = std::memcmp(p + i, s, 4) == 0;
    i += 4;
    return ok;
  }
  std::string cstr(size_t k) {  // k octets, texte jusqu'au premier nul
    need(k);
    size_t e = 0;
    while (e < k && p[i + e]) ++e;
    std::string s((const char*)p + i, e);
    i += k;
    return s;
  }
  const uint8_t* here() const { return p + i; }
  size_t left() const { return n - i; }
};

int BytesFor(uint64_t n) {  // octets nécessaires pour coder n
  int b = 1;
  while (b < 8 && (n >> (8 * b)) != 0) ++b;
  return b;
}

uint64_t Product(const std::vector<uint64_t>& v) {
  uint64_t p = 1;
  for (uint64_t x : v) p *= x;
  return p;
}

// Types de données
struct DType {
  int cls = -1;  // 0 entier, 1 flottant, 3 chaîne, 9 longueur variable
  uint32_t size = 0;
  bool be = false, sign = false;
  bool vlenString = false;
  bool numeric() const {
    return (cls == 0 && (size == 1 || size == 2 || size == 4 || size == 8)) || (cls == 1 && (size == 4 || size == 8));
  }
};

double ToDouble(const uint8_t* p, const DType& t) {
  uint8_t b[8];
  for (uint32_t k = 0; k < t.size; ++k) b[k] = t.be ? p[t.size - 1 - k] : p[k];
  if (t.cls == 1) {
    if (t.size == 4) {
      float f;
      std::memcpy(&f, b, 4);
      return f;
    }
    double d;
    std::memcpy(&d, b, 8);
    return d;
  }
  uint64_t u = 0;
  for (uint32_t k = 0; k < t.size; ++k) u |= (uint64_t)b[k] << (8 * k);
  if (!t.sign) return (double)u;
  if (t.size < 8) {
    const uint64_t m = 1ull << (8 * t.size - 1);
    return (double)((int64_t)((u ^ m) - m));
  }
  return (double)(int64_t)u;
}

DType ParseDType(Rd& r) {
  DType t;
  const uint8_t b0 = r.u8();
  t.cls = b0 & 0x0F;
  const uint32_t bits = (uint32_t)r.u(3);
  t.size = r.u32();
  if (t.cls == 0) {
    t.be = bits & 1;
    t.sign = (bits >> 3) & 1;
    r.skip(std::min<size_t>(4, r.left()));  // décalage, précision
  } else if (t.cls == 1) {
    t.be = bits & 1;
    if (bits & 0x40) t.cls = -1;  // ordre VAX
    r.skip(std::min<size_t>(12, r.left()));
  } else if (t.cls == 9) {
    t.vlenString = (bits & 0x0F) == 1;
    if (r.left()) ParseDType(r);  // type de base
  }
  return t;
}

// ---------------------------------------------------------------- HDF5

struct Msg {
  uint16_t type;
  uint8_t flags;
  std::vector<uint8_t> data;
};

struct Filter {
  uint16_t id = 0, flags = 0;
  std::vector<uint32_t> cd;
};

struct ChunkRef {
  uint64_t addr = kUndef, size = 0;
  uint32_t mask = 0;
};

struct H5Var {
  DType dt;
  std::vector<uint64_t> dims;
  int layout = -1;  // 0 compact, 1 contigu, 2 par blocs
  std::vector<uint8_t> compact;
  uint64_t addr = kUndef, size = 0;
  std::vector<uint64_t> chunk;  // taille d'un bloc (rang)
  int index = 0;                // 0 B-tree v1, 1 bloc unique, 2 implicite, 3 tableau fixe
  bool edgeUnfiltered = false;
  ChunkRef single;
  std::vector<Filter> filters;
  std::vector<uint8_t> fill;
  bool chunksLoaded = false;
  std::unordered_map<uint64_t, ChunkRef> chunks;  // indice linéaire du bloc
  std::string unsupported;                         // lecture impossible : pourquoi
};

struct Link {
  std::string name;
  uint64_t addr;
};

class Hdf5 {
 public:
  Src& src;
  int O = 8, L = 8;
  uint64_t base = 0;

  explicit Hdf5(Src& s) : src(s) {}
  long budget = 4000000;  // nœuds lus au plus (fichier endommagé : pas de boucle sans fin)
  void spend() {
    if (--budget < 0) Corrupt("structure en boucle");
  }

  uint64_t A(uint64_t a) const { return a == kUndef ? a : base + a; }

  uint64_t superblock() {
    static const uint8_t sig[8] = {0x89, 'H', 'D', 'F', '\r', '\n', 0x1A, '\n'};
    uint64_t at = 0;
    for (;;) {
      if (at + 8 > src.size()) throw Fail(tr("netCDF : format non reconnu (ni netCDF-3, ni netCDF-4/HDF5)"));
      uint8_t b[8];
      src.read(at, b, 8);
      if (std::memcmp(b, sig, 8) == 0) break;
      at = at ? at * 2 : 512;
    }
    std::vector<uint8_t> v = src.readUpTo(at, 256);
    Rd r(v);
    r.skip(8);
    const int ver = r.u8();
    uint64_t root = kUndef;
    if (ver == 0 || ver == 1) {
      r.skip(4);  // versions espace libre, table racine, réservé, en-tête partagé
      O = r.u8();
      L = r.u8();
      r.skip(1 + 2 + 2 + 4);
      if (ver == 1) r.skip(4);
      base = r.addr(O);
      r.addr(O);  // espace libre
      r.addr(O);  // fin de fichier
      r.addr(O);  // pilote
      r.addr(O);  // nom du lien
      root = r.addr(O);
    } else if (ver == 2 || ver == 3) {
      O = r.u8();
      L = r.u8();
      r.skip(1);
      base = r.addr(O);
      r.addr(O);  // extension
      r.addr(O);  // fin de fichier
      root = r.addr(O);
    } else {
      Unsupported("superbloc v" + std::to_string(ver));
    }
    if (O < 2 || O > 8 || L < 2 || L > 8) Unsupported("taille d'adresse");
    if (base == kUndef) base = 0;
    if (root == kUndef) Corrupt("groupe racine");
    return root;
  }

  // Messages d'un en-tête d'objet (blocs de continuation compris)
  std::vector<Msg> header(uint64_t addr) {
    std::vector<Msg> out;
    addr = A(addr);
    std::vector<uint8_t> h = src.readUpTo(addr, 64);
    struct Block {
      uint64_t a, n;
    };
    std::vector<Block> blocks;
    bool v2 = false, order = false;
    if (h.size() >= 4 && std::memcmp(h.data(), "OHDR", 4) == 0) {
      v2 = true;
      Rd r(h);
      r.skip(4);
      if (r.u8() != 2) Unsupported("en-tête d'objet");
      const uint8_t fl = r.u8();
      order = fl & 0x04;
      if (fl & 0x20) r.skip(16);
      if (fl & 0x10) r.skip(4);
      const uint64_t n0 = r.u(1 << (fl & 3));
      blocks.push_back({addr + r.i, n0});
    } else {
      Rd r(h);
      if (r.u8() != 1) Unsupported("en-tête d'objet");
      r.skip(1);
      r.u16();  // nombre de messages
      r.u32();  // compteur de références
      const uint32_t n0 = r.u32();
      blocks.push_back({addr + 16, n0});
    }
    for (size_t b = 0; b < blocks.size(); ++b) {
      spend();
      if (blocks.size() > 4096) Corrupt("en-tête d'objet");
      std::vector<uint8_t> d = src.read(blocks[b].a, (size_t)blocks[b].n);
      Rd r(d);
      const size_t mh = v2 ? (order ? 6 : 4) : 8;
      while (r.left() >= mh) {
        Msg m;
        if (v2) {
          m.type = r.u8();
          const uint16_t sz = r.u16();
          m.flags = r.u8();
          if (order) r.skip(2);
          if (sz > r.left()) Corrupt("message d'en-tête");
          m.data.assign(r.here(), r.here() + sz);
          r.skip(sz);
        } else {
          m.type = r.u16();
          const uint16_t sz = r.u16();
          m.flags = r.u8();
          r.skip(3);
          if (sz > r.left()) Corrupt("message d'en-tête");
          m.data.assign(r.here(), r.here() + sz);
          r.skip(sz);
        }
        if (m.type == 0x10) {  // continuation
          Rd c(m.data);
          const uint64_t a = A(c.addr(O)), n = c.addr(L);
          if (v2) {
            if (n < 8) Corrupt("continuation");
            std::vector<uint8_t> s = src.read(a, 4);
            if (std::memcmp(s.data(), "OCHK", 4) != 0) Corrupt("continuation");
            blocks.push_back({a + 4, n - 8});
          } else {
            blocks.push_back({a, n});
          }
          continue;
        }
        if (m.type != 0) out.push_back(std::move(m));
      }
    }
    return out;
  }

  // ---- tas fractal (liens et attributs denses)
  struct Heap {
    int idLen = 0, offBytes = 0, lenBytes = 0;
    uint16_t width = 0, curRows = 0;
    uint64_t start = 0, maxDirect = 0, root = kUndef;
    int maxDirectRows = 0;
    bool filtered = false;
  };

  Heap heap(uint64_t addr) {
    std::vector<uint8_t> v = src.readUpTo(A(addr), 256);
    Rd r(v);
    if (!r.sig("FRHP")) Corrupt("tas fractal");
    Heap h;
    r.skip(1);
    h.idLen = r.u16();
    const uint16_t ioLen = r.u16();
    h.filtered = ioLen > 0;
    r.skip(1);
    const uint32_t maxManaged = r.u32();
    r.addr(L);  // prochain objet énorme
    r.addr(O);  // B-tree des objets énormes
    r.addr(L);  // espace libre
    r.addr(O);  // gestionnaire d'espace libre
    for (int k = 0; k < 8; ++k) r.addr(L);
    h.width = r.u16();
    h.start = r.addr(L);
    h.maxDirect = r.addr(L);
    const uint16_t maxHeapBits = r.u16();
    r.u16();  // lignes de départ
    h.root = r.addr(O);
    h.curRows = r.u16();
    if (h.filtered) Unsupported("tas fractal filtré");
    if (h.width == 0 || h.start == 0 || h.maxDirect < h.start) Corrupt("tas fractal");
    h.offBytes = (maxHeapBits + 7) / 8;
    h.lenBytes = BytesFor(std::min<uint64_t>(h.maxDirect, maxManaged));
    int l2s = 0, l2d = 0;
    while ((1ull << l2s) < h.start) ++l2s;
    while ((1ull << l2d) < h.maxDirect) ++l2d;
    h.maxDirectRows = l2d - l2s + 2;
    return h;
  }

  uint64_t rowSize(const Heap& h, int r) const { return r <= 1 ? h.start : h.start << (r - 1); }

  // Adresse dans le fichier de l'octet `off` du tas, cherché dans le bloc
  // indirect `blk` (nrows lignes) qui commence à l'offset `boff` du tas
  uint64_t locate(const Heap& h, uint64_t blk, int nrows, uint64_t boff, uint64_t off, int depth) {
    spend();
    if (depth > 8) Corrupt("tas fractal");
    const size_t hdr = 4 + 1 + O + h.offBytes;
    std::vector<uint8_t> v = src.read(A(blk) + hdr, (size_t)nrows * h.width * O);
    Rd r(v);
    uint64_t pos = boff;
    for (int row = 0; row < nrows; ++row) {
      const uint64_t sz = rowSize(h, row);
      for (int c = 0; c < h.width; ++c) {
        const uint64_t a = r.addr(O);
        if (off >= pos && off < pos + sz) {
          if (a == kUndef) Corrupt("bloc du tas absent");
          if (row < h.maxDirectRows) return A(a) + (off - pos);
          int child = 0;
          uint64_t span = h.start * h.width;
          while (span < sz) {
            span <<= 1;
            ++child;
          }
          return locate(h, a, child + 1, pos, off, depth + 1);
        }
        pos += sz;
      }
    }
    Corrupt("objet hors du tas");
  }

  std::vector<uint8_t> heapObject(const Heap& h, const uint8_t* id) {
    const int type = (id[0] >> 4) & 3;
    if (type == 2) {  // objet minuscule : dans l'identifiant
      const int n = (id[0] & 0x0F) + 1;
      if (n + 1 > h.idLen) Corrupt("objet minuscule");
      return std::vector<uint8_t>(id + 1, id + 1 + n);
    }
    if (type != 0) Unsupported("objet énorme du tas fractal");
    Rd r(id + 1, (size_t)h.idLen - 1);
    const uint64_t off = r.u(h.offBytes), len = r.u(h.lenBytes);
    uint64_t at;
    if (h.curRows == 0) at = A(h.root) + off;  // racine = bloc direct
    else at = locate(h, h.root, h.curRows, 0, off, 0);
    return src.read(at, (size_t)len);
  }

  // ---- B-tree v2 : enregistrements de tous les nœuds
  void btree2(uint64_t addr, std::vector<std::vector<uint8_t>>& recs, int& type) {
    std::vector<uint8_t> v = src.readUpTo(A(addr), 64);
    Rd r(v);
    if (!r.sig("BTHD")) Corrupt("B-tree v2");
    r.skip(1);
    type = r.u8();
    const uint32_t nodeSize = r.u32();
    const uint16_t recSize = r.u16();
    const uint16_t depth = r.u16();
    r.skip(2);
    const uint64_t root = r.addr(O);
    const uint16_t rootN = r.u16();
    if (root == kUndef || rootN == 0) return;
    if (recSize == 0 || nodeSize < 16) Corrupt("B-tree v2");
    // tailles des champs « nombre d'enregistrements » des pointeurs
    std::vector<uint64_t> maxN(depth + 1), cumN(depth + 1);
    std::vector<int> ptr(depth + 1, 0), nBytes(depth + 1, 0), cBytes(depth + 1, 0);
    maxN[0] = (nodeSize - 10) / recSize;
    cumN[0] = maxN[0];
    for (int d = 1; d <= depth; ++d) {
      nBytes[d] = BytesFor(maxN[d - 1]);
      cBytes[d] = d > 1 ? BytesFor(cumN[d - 1]) : 0;
      ptr[d] = O + nBytes[d] + cBytes[d];
      maxN[d] = (nodeSize - 10 - ptr[d]) / (recSize + ptr[d]);
      cumN[d] = (maxN[d] + 1) * cumN[d - 1] + maxN[d];
    }
    walk2(root, rootN, depth, nodeSize, recSize, nBytes, cBytes, recs, 0);
  }

  void walk2(uint64_t addr, uint64_t n, int d, uint32_t nodeSize, uint16_t recSize, const std::vector<int>& nBytes,
             const std::vector<int>& cBytes, std::vector<std::vector<uint8_t>>& recs, int guard) {
    spend();
    if (guard > 16) Corrupt("B-tree v2");
    std::vector<uint8_t> v = src.readUpTo(A(addr), nodeSize);
    Rd r(v);
    if (!r.sig(d == 0 ? "BTLF" : "BTIN")) Corrupt("nœud de B-tree v2");
    r.skip(2);
    for (uint64_t k = 0; k < n; ++k) {
      r.need(recSize);
      recs.emplace_back(r.here(), r.here() + recSize);
      r.skip(recSize);
    }
    if (d == 0) return;
    for (uint64_t k = 0; k <= n; ++k) {
      const uint64_t a = r.addr(O);
      const uint64_t cn = r.u(nBytes[d]);
      if (cBytes[d]) r.u(cBytes[d]);
      if (a != kUndef && cn) walk2(a, cn, d - 1, nodeSize, recSize, nBytes, cBytes, recs, guard + 1);
    }
  }

  // ---- liens d'un groupe
  static void parseLink(Rd& r, int O, std::vector<Link>& out) {
    if (r.u8() != 1) Unsupported("message de lien");
    const uint8_t fl = r.u8();
    int type = 0;
    if (fl & 0x08) type = r.u8();
    if (fl & 0x04) r.skip(8);
    if (fl & 0x10) r.skip(1);
    const uint64_t nl = r.u(1 << (fl & 3));
    const std::string name = r.cstr((size_t)nl);
    if (type == 0) out.push_back({name, r.addr(O)});
  }

  std::vector<Link> links(const std::vector<Msg>& msgs) {
    std::vector<Link> out;
    for (const Msg& m : msgs) {
      if (m.type == 0x06) {
        Rd r(m.data);
        parseLink(r, O, out);
      } else if (m.type == 0x02) {  // informations de liens : stockage dense ?
        Rd r(m.data);
        r.skip(1);
        const uint8_t fl = r.u8();
        if (fl & 1) r.skip(8);
        const uint64_t fh = r.addr(O), bt = r.addr(O);
        if (fh == kUndef || bt == kUndef) continue;
        Heap h = heap(fh);
        std::vector<std::vector<uint8_t>> recs;
        int type = 0;
        btree2(bt, recs, type);
        for (const auto& rec : recs) {
          if (rec.size() < 4 + (size_t)h.idLen) Corrupt("index des liens");
          std::vector<uint8_t> obj = heapObject(h, rec.data() + 4);
          Rd lr(obj);
          parseLink(lr, O, out);
        }
      } else if (m.type == 0x11) {  // table des symboles (ancien format)
        Rd r(m.data);
        const uint64_t bt = r.addr(O), lh = r.addr(O);
        std::vector<uint8_t> hv = src.readUpTo(A(lh), 64);
        Rd hr(hv);
        if (!hr.sig("HEAP")) Corrupt("tas local");
        hr.skip(4);
        const uint64_t dsize = hr.addr(L);
        hr.addr(L);
        const uint64_t da = hr.addr(O);
        std::vector<uint8_t> names = src.read(A(da), (size_t)dsize);
        symbols(bt, names, out, -1, 0);
      }
    }
    return out;
  }

  void symbols(uint64_t addr, const std::vector<uint8_t>& names, std::vector<Link>& out, int want, int guard) {
    spend();
    if (guard > 32) Corrupt("B-tree de groupe");
    std::vector<uint8_t> h = src.read(A(addr), 8 + 2 * O);
    Rd r(h);
    if (!r.sig("TREE")) Corrupt("B-tree de groupe");
    if (r.u8() != 0) Corrupt("B-tree de groupe");
    const int level = r.u8();
    if (want >= 0 && level != want) Corrupt("B-tree de groupe");
    const int used = r.u16();
    std::vector<uint8_t> body = src.read(A(addr) + 8 + 2 * O, (size_t)used * (L + O) + L);
    Rd b(body);
    for (int k = 0; k < used; ++k) {
      b.u(L);
      const uint64_t child = b.addr(O);
      if (level > 0) {
        symbols(child, names, out, level - 1, guard + 1);
        continue;
      }
      std::vector<uint8_t> sv = src.read(A(child), 8);
      Rd s(sv);
      if (!s.sig("SNOD")) Corrupt("nœud de symboles");
      s.skip(2);
      const int n = s.u16();
      const size_t es = 2 * O + 24;
      std::vector<uint8_t> ev = src.read(A(child) + 8, n * es);
      Rd e(ev);
      for (int j = 0; j < n; ++j) {
        const uint64_t no = e.u(O), oh = e.addr(O);
        e.skip(24);
        if (no >= names.size()) Corrupt("nom de symbole");
        size_t z = (size_t)no;
        while (z < names.size() && names[z]) ++z;
        out.push_back({std::string((const char*)names.data() + no, z - (size_t)no), oh});
      }
    }
  }

  // ---- attributs
  std::vector<uint8_t> globalHeapObject(uint64_t coll, uint32_t index) {
    std::vector<uint8_t> h = src.read(A(coll), 8 + L);
    Rd r(h);
    if (!r.sig("GCOL")) Corrupt("tas global");
    r.skip(4);
    const uint64_t size = r.u(L);
    std::vector<uint8_t> v = src.readUpTo(A(coll), (size_t)size);
    Rd c(v);
    c.skip(8 + L);
    while (c.left() >= 8 + (size_t)L) {
      const uint16_t idx = c.u16();
      c.skip(6);
      const uint64_t sz = c.u(L);
      if (idx == 0) break;
      if (sz > c.left()) Corrupt("tas global");
      if (idx == index) return std::vector<uint8_t>(c.here(), c.here() + sz);
      c.skip((size_t)((sz + 7) & ~7ull));
    }
    Corrupt("objet du tas global");
  }

  bool parseAttr(const std::vector<uint8_t>& data, NcAtt& a) {
    Rd r(data);
    const int ver = r.u8();
    r.skip(1);
    const uint16_t nameSz = r.u16(), dtSz = r.u16(), dsSz = r.u16();
    if (ver == 3) r.skip(1);
    auto pad = [&](size_t n) { return ver == 1 ? (n + 7) & ~(size_t)7 : n; };
    if (ver < 1 || ver > 3) return false;
    a.name = r.cstr(pad(nameSz));
    r.need(pad(dtSz));
    Rd tr_(r.here(), dtSz);
    DType t = ParseDType(tr_);
    r.skip(pad(dtSz));
    r.need(pad(dsSz));
    Rd sr(r.here(), dsSz);
    std::vector<uint64_t> dims = dataspaceL(sr);
    r.skip(pad(dsSz));
    const uint64_t n = Product(dims);
    if (t.cls == 3) {
      a.text = true;
      const size_t k = (size_t)std::min<uint64_t>(n * t.size, r.left());
      a.str = r.cstr(k);
      return true;
    }
    if (t.cls == 9 && t.vlenString) {
      a.text = true;
      for (uint64_t k = 0; k < n; ++k) {
        const uint32_t len = r.u32();
        const uint64_t coll = r.addr(O);
        const uint32_t idx = r.u32();
        if (len == 0 || coll == kUndef) continue;
        std::vector<uint8_t> s = globalHeapObject(coll, idx);
        if (!a.str.empty()) a.str += '\n';
        a.str.append((const char*)s.data(), std::min<size_t>(len, s.size()));
      }
      return true;
    }
    if (!t.numeric() || n > 1000000) return false;
    r.need((size_t)(n * t.size));
    for (uint64_t k = 0; k < n; ++k) a.num.push_back(ToDouble(r.here() + k * t.size, t));
    return true;
  }

  std::vector<NcAtt> attributes(const std::vector<Msg>& msgs) {
    std::vector<NcAtt> out;
    for (const Msg& m : msgs) {
      if (m.type == 0x0C) {
        if (m.flags & 0x02) continue;  // attribut partagé
        NcAtt a;
        try {
          if (parseAttr(m.data, a)) out.push_back(std::move(a));
        } catch (const Fail&) {
        }
      } else if (m.type == 0x15) {  // informations d'attributs : stockage dense
        Rd r(m.data);
        r.skip(1);
        const uint8_t fl = r.u8();
        if (fl & 1) r.skip(2);
        const uint64_t fh = r.addr(O), bt = r.addr(O);
        if (fh == kUndef || bt == kUndef) continue;
        Heap h = heap(fh);
        std::vector<std::vector<uint8_t>> recs;
        int type = 0;
        btree2(bt, recs, type);
        for (const auto& rec : recs) {
          if (rec.size() < 8) continue;
          NcAtt a;
          try {
            if (parseAttr(heapObject(h, rec.data()), a)) out.push_back(std::move(a));
          } catch (const Fail&) {
          }
        }
      }
    }
    return out;
  }

  // ---- jeux de données
  std::vector<uint64_t> dataspaceL(Rd& r) {
    const int ver = r.u8();
    const int rank = r.u8();
    r.u8();
    int type = 1;
    if (ver == 1) r.skip(5);
    else if (ver == 2) type = r.u8();
    else Unsupported("espace de données v" + std::to_string(ver));
    std::vector<uint64_t> dims;
    if (type == 2) return std::vector<uint64_t>{0};
    for (int k = 0; k < rank; ++k) dims.push_back(r.u(L));
    return dims;
  }

  bool dataset(const std::vector<Msg>& msgs, H5Var& v) {
    bool hasSpace = false, hasType = false, hasLayout = false;
    for (const Msg& m : msgs) {
      Rd r(m.data);
      switch (m.type) {
        case 0x01:
          v.dims = dataspaceL(r);
          hasSpace = true;
          break;
        case 0x03:
          hasType = true;
          if (m.flags & 0x02) {
            v.unsupported = std::string(tr("netCDF/HDF5 : structure non prise en charge")) + " (type partagé)";
            break;
          }
          v.dt = ParseDType(r);
          break;
        case 0x05: {  // valeur de remplissage
          const int ver = r.u8();
          bool def = true;
          if (ver == 1 || ver == 2) {
            r.skip(2);
            def = r.u8() != 0;
            if (ver == 2 && !def) break;
          } else if (ver == 3) {
            def = (r.u8() & 0x20) != 0;
            if (!def) break;
          } else {
            break;
          }
          const uint32_t n = r.u32();
          if (n && n <= r.left()) v.fill.assign(r.here(), r.here() + n);
          break;
        }
        case 0x04:
          if (v.fill.empty()) {
            const uint32_t n = r.u32();
            if (n && n <= r.left()) v.fill.assign(r.here(), r.here() + n);
          }
          break;
        case 0x08:
          hasLayout = true;
          try {
            layout(r, v);
          } catch (const Fail& e) {
            v.unsupported = e.what();
          }
          break;
        case 0x0B:
          try {
            pipeline(r, v);
          } catch (const Fail& e) {
            v.unsupported = e.what();
          }
          break;
        default:
          break;
      }
    }
    return hasSpace && hasType && hasLayout;
  }

  void layout(Rd& r, H5Var& v) {
    const int ver = r.u8();
    if (ver == 1 || ver == 2) {
      const int nd = r.u8();
      v.layout = r.u8();
      r.skip(5);
      if (v.layout != 0) v.addr = r.addr(O);
      std::vector<uint64_t> d;
      for (int k = 0; k < nd; ++k) d.push_back(r.u32());
      if (v.layout == 2) {
        v.chunk.assign(d.begin(), d.end() - 1);
      } else if (v.layout == 0) {
        const uint32_t n = r.u32();
        r.need(n);
        v.compact.assign(r.here(), r.here() + n);
      }
      return;
    }
    if (ver != 3 && ver != 4) Unsupported("disposition v" + std::to_string(ver));
    v.layout = r.u8();
    if (v.layout == 0) {
      const uint16_t n = r.u16();
      r.need(n);
      v.compact.assign(r.here(), r.here() + n);
    } else if (v.layout == 1) {
      v.addr = r.addr(O);
      v.size = r.u(L);
    } else if (v.layout == 2) {
      if (ver == 3) {
        const int nd = r.u8();
        v.addr = r.addr(O);
        for (int k = 0; k < nd; ++k) v.chunk.push_back(r.u32());
        v.chunk.pop_back();  // taille de l'élément
        v.index = 0;
      } else {
        const uint8_t fl = r.u8();
        v.edgeUnfiltered = fl & 1;
        const int nd = r.u8();
        const int enc = r.u8();
        for (int k = 0; k < nd; ++k) v.chunk.push_back(r.u(enc));
        v.chunk.pop_back();
        const int it = r.u8();
        if (it == 1) {
          v.index = 1;
          if (fl & 2) {
            v.single.size = r.u(L);
            v.single.mask = r.u32();
          }
        } else if (it == 2) {
          v.index = 2;
        } else if (it == 3) {
          v.index = 3;
          r.skip(1);
        } else {
          Unsupported(it == 4 ? "index de blocs « tableau extensible »" : "index de blocs B-tree v2");
        }
        v.addr = r.addr(O);
        if (v.index == 1) {
          v.single.addr = v.addr;
          if (!(fl & 2)) v.single.size = 0;
        }
      }
    } else {
      Unsupported("disposition virtuelle");
    }
  }

  void pipeline(Rd& r, H5Var& v) {
    const int ver = r.u8();
    const int n = r.u8();
    if (ver == 1) r.skip(6);
    for (int k = 0; k < n; ++k) {
      Filter f;
      f.id = r.u16();
      uint16_t nameLen = 0;
      if (ver == 1 || f.id >= 256) nameLen = r.u16();
      f.flags = r.u16();
      const uint16_t nv = r.u16();
      r.skip(nameLen);
      for (int j = 0; j < nv; ++j) f.cd.push_back(r.u32());
      if (ver == 1 && (nv & 1)) r.skip(4);
      v.filters.push_back(f);
    }
  }

  // ---- index des blocs
  uint64_t chunkGrid(const H5Var& v, std::vector<uint64_t>& n) const {
    n.resize(v.dims.size());
    uint64_t t = 1;
    for (size_t k = 0; k < v.dims.size(); ++k) {
      n[k] = (v.dims[k] + v.chunk[k] - 1) / v.chunk[k];
      t *= n[k];
    }
    return t;
  }

  void loadChunks(H5Var& v) {
    if (v.chunksLoaded) return;
    v.chunksLoaded = true;
    if (v.addr == kUndef) return;  // rien d'écrit : remplissage
    std::vector<uint64_t> grid;
    const uint64_t total = chunkGrid(v, grid);
    const uint64_t raw = Product(v.chunk) * v.dt.size;
    if (v.index == 1) {
      ChunkRef c = v.single;
      if (c.size == 0) c.size = raw;
      v.chunks[0] = c;
    } else if (v.index == 2) {
      for (uint64_t k = 0; k < total; ++k) v.chunks[k] = ChunkRef{v.addr + k * raw, raw, 0};
    } else if (v.index == 3) {
      std::vector<uint8_t> hv = src.readUpTo(A(v.addr), 64);
      Rd h(hv);
      if (!h.sig("FAHD")) Corrupt("tableau fixe");
      h.skip(1);
      const int client = h.u8();
      const int es = h.u8();
      const int pageBits = h.u8();
      const uint64_t nent = h.u(L);
      const uint64_t db = h.addr(O);
      if (pageBits < 64 && nent > (1ull << pageBits)) Unsupported("tableau fixe paginé");
      if (db == kUndef) return;
      const size_t hdr = 4 + 1 + 1 + O;
      std::vector<uint8_t> dv = src.read(A(db) + hdr, (size_t)(nent * es));
      Rd d(dv);
      for (uint64_t k = 0; k < nent && k < total; ++k) {
        ChunkRef c;
        c.addr = d.addr(O);
        if (client == 1) {
          c.size = d.u(es - O - 4);
          c.mask = d.u32();
        } else {
          c.size = raw;
          d.skip(es - O);
        }
        if (c.addr != kUndef) v.chunks[k] = c;
      }
    } else {
      chunkTree(v, v.addr, grid, -1, 0);
    }
  }

  void chunkTree(H5Var& v, uint64_t addr, const std::vector<uint64_t>& grid, int want, int guard) {
    spend();
    if (guard > 32) Corrupt("B-tree des blocs");
    std::vector<uint8_t> h = src.read(A(addr), 8 + 2 * O);
    Rd r(h);
    if (!r.sig("TREE")) Corrupt("B-tree des blocs");
    if (r.u8() != 1) Corrupt("B-tree des blocs");
    const int level = r.u8();
    if (want >= 0 && level != want) Corrupt("B-tree des blocs");
    const int used = r.u16();
    const size_t nd = v.dims.size() + 1;
    const size_t key = 8 + 8 * nd;
    std::vector<uint8_t> body = src.read(A(addr) + 8 + 2 * O, used * (key + O) + key);
    Rd b(body);
    for (int k = 0; k < used; ++k) {
      ChunkRef c;
      c.size = b.u32();
      c.mask = b.u32();
      uint64_t lin = 0;
      for (size_t d = 0; d < nd; ++d) {
        const uint64_t off = b.u(8);
        if (d < v.dims.size()) lin = lin * grid[d] + off / v.chunk[d];
      }
      c.addr = b.addr(O);
      if (level > 0) chunkTree(v, c.addr, grid, level - 1, guard + 1);
      else if (c.addr != kUndef) v.chunks[lin] = c;
    }
  }
};

void Unshuffle(std::vector<uint8_t>& buf, size_t es) {
  if (es <= 1 || buf.size() < es) return;
  const size_t n = buf.size() / es;
  std::vector<uint8_t> o(buf.size());
  for (size_t b = 0; b < es; ++b) {
    const uint8_t* s = buf.data() + b * n;
    for (size_t i = 0; i < n; ++i) o[i * es + b] = s[i];
  }
  std::memcpy(o.data() + n * es, buf.data() + n * es, buf.size() - n * es);
  buf.swap(o);
}

// ---------------------------------------------------------------- netCDF-3

struct Nc3Var {
  int type = 0;
  uint64_t begin = 0, vsize = 0;
  bool record = false;
  DType dt;
};

}  // namespace

// ---------------------------------------------------------------- NcVar

const NcAtt* NcVar::att(const std::string& n) const {
  for (const auto& a : atts)
    if (a.name == n) return &a;
  return nullptr;
}

double NcVar::num(const std::string& n, double def) const {
  const NcAtt* a = att(n);
  return a && !a->text && !a->num.empty() ? a->num[0] : def;
}

std::string NcVar::str(const std::string& n) const {
  const NcAtt* a = att(n);
  return a && a->text ? a->str : std::string();
}

uint64_t NcVar::size() const { return Product(shape); }

// ---------------------------------------------------------------- NcFile

struct NcFile::Impl {
  Src src;
  std::unique_ptr<Hdf5> h5;
  std::vector<H5Var> h5vars;
  std::vector<Nc3Var> nc3vars;
  uint64_t recSize = 0, numRecs = 0;
  // cache des blocs décompressés
  struct Key {
    size_t var;
    uint64_t chunk;
    bool operator<(const Key& o) const { return var != o.var ? var < o.var : chunk < o.chunk; }
  };
  std::map<Key, std::pair<std::vector<uint8_t>, std::list<Key>::iterator>> cache;
  std::list<Key> lru;
  size_t cacheBytes = 0, cacheMax = 128u << 20;

  void cacheTrim(size_t keep) {
    while (cacheBytes > cacheMax && lru.size() > keep) {
      auto it = cache.find(lru.back());
      cacheBytes -= it->second.first.size();
      cache.erase(it);
      lru.pop_back();
    }
  }

  const std::vector<uint8_t>& chunkData(size_t vi, uint64_t lin, const std::vector<uint64_t>& ci) {
    Key k{vi, lin};
    auto it = cache.find(k);
    if (it != cache.end()) {
      lru.splice(lru.begin(), lru, it->second.second);
      return it->second.first;
    }
    H5Var& v = h5vars[vi];
    const size_t es = v.dt.size;
    const uint64_t raw = Product(v.chunk) * es;
    if (raw > (uint64_t)1 << 31) Unsupported("bloc trop grand");
    std::vector<uint8_t> out;
    auto c = v.chunks.find(lin);
    if (c == v.chunks.end() || c->second.addr == kUndef) {
      out.resize((size_t)raw);
      if (v.fill.size() == es)
        for (size_t i = 0; i < out.size(); i += es) std::memcpy(out.data() + i, v.fill.data(), es);
      else
        std::fill(out.begin(), out.end(), 0);
    } else {
      out = src.read(h5->A(c->second.addr), (size_t)c->second.size);
      bool edge = false;
      for (size_t d = 0; d < v.dims.size(); ++d) edge = edge || (ci[d] + 1) * v.chunk[d] > v.dims[d];
      const bool skipAll = edge && v.edgeUnfiltered;
      for (size_t f = v.filters.size(); f-- > 0;) {
        if (skipAll || (c->second.mask >> f) & 1) continue;
        const Filter& fl = v.filters[f];
        if (fl.id == 1) {
          // taille de sortie : celle du bloc, plus d'éventuels octets ajoutés
          // par les filtres précédents (fletcher32)
          size_t cap = (size_t)raw + 64;
          for (;;) {
            std::vector<uint8_t> o(cap);
            mz_ulong n = (mz_ulong)o.size();
            const int rc = mz_uncompress(o.data(), &n, out.data(), (mz_ulong)out.size());
            if (rc == MZ_OK) {
              o.resize(n);
              out.swap(o);
              break;
            }
            if (rc != MZ_BUF_ERROR || cap > ((size_t)raw << 4) + (1u << 20)) Corrupt("bloc deflate");
            cap *= 2;
          }
        } else if (fl.id == 2) {
          Unshuffle(out, fl.cd.empty() ? es : fl.cd[0]);
        } else if (fl.id == 3) {
          if (out.size() < 4) Corrupt("fletcher32");
          out.resize(out.size() - 4);
        } else if (!(fl.flags & 1)) {  // filtre obligatoire inconnu
          throw Fail(std::string(tr("netCDF : filtre de compression non pris en charge")) + " (" +
                     std::to_string(fl.id) + ")");
        } else {
          throw Fail(std::string(tr("netCDF : filtre de compression non pris en charge")) + " (" +
                     std::to_string(fl.id) + ")");
        }
      }
      if (out.size() != raw) Corrupt("taille du bloc décompressé");
    }
    lru.push_front(k);
    cacheBytes += out.size();
    auto& slot = cache[k];
    slot.first.swap(out);
    slot.second = lru.begin();
    cacheTrim(64);
    return cache[k].first;
  }
};

NcFile::NcFile() : d_(new Impl) {}
NcFile::~NcFile() = default;

void NcFile::close() {
  d_.reset(new Impl);
  vars_.clear();
  globals_.clear();
  format_ = 0;
}

void NcFile::setCacheBytes(size_t n) { d_->cacheMax = n; }

const NcVar* NcFile::var(const std::string& name) const {
  for (const auto& v : vars_)
    if (v.name == name) return &v;
  return nullptr;
}

const NcVar* NcFile::coord(const NcVar& v, size_t axis, char want) const {
  if (axis >= v.shape.size()) return nullptr;
  const uint64_t n = v.shape[axis];
  if (axis < v.dims.size()) {
    const NcVar* c = var(v.dims[axis]);
    if (c && c->shape.size() == 1 && c->shape[0] == n) return c;
  }
  static const char* ys[] = {"lat", "latitude", "y", "nav_lat", "Latitude", "LAT"};
  static const char* xs[] = {"lon", "longitude", "x", "nav_lon", "Longitude", "LON"};
  const char** names = want == 'y' ? ys : xs;
  for (int k = 0; k < 6; ++k) {
    const NcVar* c = var(names[k]);
    if (c && c->shape.size() == 1 && c->shape[0] == n) return c;
  }
  const std::string unit = want == 'y' ? "degrees_north" : "degrees_east";
  const std::string sn = want == 'y' ? "latitude" : "longitude";
  for (const auto& c : vars_)
    if (c.shape.size() == 1 && c.shape[0] == n && (c.str("units") == unit || c.str("standard_name") == sn))
      return &c;
  return nullptr;
}

namespace {

// netCDF-3 : en-tête (gros-boutiste)
struct Be {
  const std::vector<uint8_t>& v;
  size_t i = 0;
  explicit Be(const std::vector<uint8_t>& b) : v(b) {}
  uint64_t u(int n) {
    if (i + n > v.size()) Corrupt("en-tête netCDF-3");
    uint64_t x = 0;
    for (int k = 0; k < n; ++k) x = (x << 8) | v[i + k];
    i += n;
    return x;
  }
  std::string name() {
    const uint64_t n = u(4);
    if (i + n > v.size()) Corrupt("nom netCDF-3");
    std::string s((const char*)v.data() + i, (size_t)n);
    i += (size_t)((n + 3) & ~3ull);
    return s;
  }
};

DType Nc3Type(int t) {
  DType d;
  d.be = true;
  switch (t) {
    case 1: d.cls = 0; d.size = 1; d.sign = true; break;
    case 2: d.cls = 3; d.size = 1; break;
    case 3: d.cls = 0; d.size = 2; d.sign = true; break;
    case 4: d.cls = 0; d.size = 4; d.sign = true; break;
    case 5: d.cls = 1; d.size = 4; break;
    case 6: d.cls = 1; d.size = 8; break;
    case 7: d.cls = 0; d.size = 1; break;
    case 8: d.cls = 0; d.size = 2; break;
    case 9: d.cls = 0; d.size = 4; break;
    case 10: d.cls = 0; d.size = 8; d.sign = true; break;
    case 11: d.cls = 0; d.size = 8; break;
    default: Unsupported("type netCDF-3 " + std::to_string(t));
  }
  return d;
}

void Nc3Atts(Be& b, bool cdf5, std::vector<NcAtt>& out) {
  const uint64_t tag = b.u(4), n = b.u(cdf5 ? 8 : 4);
  if (tag == 0 && n == 0) return;
  if (tag != 0x0C) Corrupt("attributs netCDF-3");
  for (uint64_t k = 0; k < n; ++k) {
    NcAtt a;
    a.name = b.name();
    const int t = (int)b.u(4);
    const uint64_t ne = b.u(cdf5 ? 8 : 4);
    DType d = Nc3Type(t);
    const uint64_t bytes = ne * d.size;
    if (b.i + bytes > b.v.size()) Corrupt("attribut netCDF-3");
    if (d.cls == 3) {
      a.text = true;
      size_t e = 0;
      while (e < bytes && b.v[b.i + e]) ++e;
      a.str.assign((const char*)b.v.data() + b.i, e);
    } else {
      for (uint64_t j = 0; j < ne; ++j) a.num.push_back(ToDouble(b.v.data() + b.i + j * d.size, d));
    }
    b.i += (size_t)((bytes + 3) & ~3ull);
    out.push_back(std::move(a));
  }
}

}  // namespace

bool NcFile::open(const std::string& path, std::string& err) {
  close();
  Impl& d = *d_;
  if (!d.src.open(path)) {
    err = tr("netCDF : fichier introuvable ou illisible");
    return false;
  }
  try {
    uint8_t magic[4] = {0, 0, 0, 0};
    if (d.src.size() >= 4) d.src.read(0, magic, 4);
    if (magic[0] == 'C' && magic[1] == 'D' && magic[2] == 'F' && (magic[3] == 1 || magic[3] == 2 || magic[3] == 5)) {
      // ---- netCDF-3
      const bool cdf5 = magic[3] == 5;
      const int offBytes = magic[3] == 1 ? 4 : 8;
      std::vector<uint8_t> hv = d.src.readUpTo(0, 1 << 20);
      for (;;) {
        try {
          Be b(hv);
          b.i = 4;
          d.numRecs = b.u(cdf5 ? 8 : 4);
          std::vector<std::pair<std::string, uint64_t>> dims;
          uint64_t tag = b.u(4), n = b.u(cdf5 ? 8 : 4);
          if (tag == 0x0A)
            for (uint64_t k = 0; k < n; ++k) {
              std::string nm = b.name();
              dims.emplace_back(nm, b.u(cdf5 ? 8 : 4));
            }
          else if (tag != 0 || n != 0)
            Corrupt("dimensions netCDF-3");
          Nc3Atts(b, cdf5, globals_);
          tag = b.u(4);
          n = b.u(cdf5 ? 8 : 4);
          if (tag != 0x0B && (tag != 0 || n != 0)) Corrupt("variables netCDF-3");
          std::vector<size_t> recVars;
          for (uint64_t k = 0; k < (tag == 0x0B ? n : 0); ++k) {
            NcVar v;
            Nc3Var x;
            v.name = b.name();
            const uint64_t rank = b.u(cdf5 ? 8 : 4);
            for (uint64_t j = 0; j < rank; ++j) {
              const uint64_t id = b.u(cdf5 ? 8 : 4);
              if (id >= dims.size()) Corrupt("dimension netCDF-3");
              v.dims.push_back(dims[id].first);
              uint64_t len = dims[id].second;
              if (len == 0) {
                if (j != 0) Corrupt("dimension illimitée");
                x.record = true;
                len = d.numRecs;
              }
              v.shape.push_back(len);
            }
            Nc3Atts(b, cdf5, v.atts);
            x.type = (int)b.u(4);
            x.vsize = b.u(cdf5 ? 8 : 4);
            x.begin = b.u(offBytes);
            x.dt = Nc3Type(x.type);
            v.numeric = x.dt.numeric();
            v.floating = x.dt.cls == 1;
            v.bytes = (int)x.dt.size;
            if (x.record) recVars.push_back(vars_.size());
            vars_.push_back(std::move(v));
            d.nc3vars.push_back(x);
          }
          if (recVars.size() == 1) {
            const NcVar& v = vars_[recVars[0]];
            uint64_t s = d.nc3vars[recVars[0]].dt.size;
            for (size_t j = 1; j < v.shape.size(); ++j) s *= v.shape[j];
            d.recSize = s;
          } else {  // tailles calculées (vsize n'est pas toujours arrondi à 4)
            for (size_t r : recVars) {
              uint64_t sz = d.nc3vars[r].dt.size;
              for (size_t j = 1; j < vars_[r].shape.size(); ++j) sz *= vars_[r].shape[j];
              d.recSize += (sz + 3) & ~3ull;
            }
          }
          break;
        } catch (const Fail&) {
          if (hv.size() < (1u << 20) || hv.size() >= d.src.size()) throw;
          hv = d.src.readUpTo(0, std::min<uint64_t>(d.src.size(), (uint64_t)hv.size() * 4));
          vars_.clear();
          globals_.clear();
          d.nc3vars.clear();
          d.recSize = 0;
        }
      }
      format_ = 3;
      return true;
    }
    // ---- netCDF-4 / HDF5
    d.h5.reset(new Hdf5(d.src));
    Hdf5& h = *d.h5;
    const uint64_t root = h.superblock();
    std::vector<Msg> rm = h.header(root);
    globals_ = h.attributes(rm);
    struct G {
      std::string prefix;
      std::vector<Msg> msgs;
      int depth;
    };
    std::vector<G> todo{{std::string(), std::move(rm), 0}};
    std::vector<uint64_t> seen{root};
    while (!todo.empty()) {
      G g = std::move(todo.back());
      todo.pop_back();
      for (const Link& l : h.links(g.msgs)) {
        if (l.addr == kUndef || std::find(seen.begin(), seen.end(), l.addr) != seen.end()) continue;
        seen.push_back(l.addr);
        std::vector<Msg> m = h.header(l.addr);
        H5Var hv;
        bool isData = false;
        try {
          isData = h.dataset(m, hv);
        } catch (const Fail&) {
          isData = false;
        }
        if (isData) {
          NcVar v;
          v.name = g.prefix + l.name;
          v.shape = hv.dims;
          v.atts = h.attributes(m);
          v.numeric = hv.dt.numeric();
          v.floating = hv.dt.cls == 1;
          v.bytes = (int)hv.dt.size;
          // dimension sans variable (netCDF-4) : ignorée
          if (v.str("NAME").compare(0, 34, "This is a netCDF dimension but not") == 0) continue;
          if (hv.unsupported.empty() && hv.layout == 2) {
            bool ok = hv.chunk.size() == hv.dims.size();
            for (uint64_t c : hv.chunk) ok = ok && c > 0;
            if (!ok) hv.unsupported = std::string(tr("netCDF/HDF5 : fichier tronqué ou endommagé")) + " (blocs)";
          }
          if (!hv.unsupported.empty()) v.numeric = false;
          vars_.push_back(std::move(v));
          d.h5vars.push_back(std::move(hv));
        } else if (g.depth < 4) {
          bool group = false;
          for (const Msg& x : m) group = group || x.type == 0x02 || x.type == 0x06 || x.type == 0x11;
          if (group) todo.push_back({g.prefix + l.name + "/", std::move(m), g.depth + 1});
        }
      }
    }
    format_ = 4;
    return true;
  } catch (const Fail& e) {
    err = e.what();
  } catch (const std::exception&) {
    err = tr("netCDF/HDF5 : fichier tronqué ou endommagé");
  }
  close();
  return false;
}

bool NcFile::readAll(const NcVar& v, std::vector<double>& out, std::string& err) {
  return read(v, std::vector<uint64_t>(v.shape.size(), 0), v.shape, out, err);
}

bool NcFile::read(const NcVar& v, const std::vector<uint64_t>& start, const std::vector<uint64_t>& count,
                  std::vector<double>& out, std::string& err) {
  const size_t vi = (size_t)(&v - vars_.data());
  if (vi >= vars_.size() || !format_) {
    err = tr("netCDF : fichier introuvable ou illisible");
    return false;
  }
  const size_t rank = v.shape.size();
  if (start.size() != rank || count.size() != rank) {
    err = tr("netCDF : lecture hors des dimensions de la variable");
    return false;
  }
  for (size_t k = 0; k < rank; ++k)
    if (start[k] > v.shape[k] || count[k] > v.shape[k] - start[k]) {
      err = tr("netCDF : lecture hors des dimensions de la variable");
      return false;
    }
  if (format_ == 4 && !d_->h5vars[vi].unsupported.empty()) {
    err = d_->h5vars[vi].unsupported;
    return false;
  }
  if (!v.numeric) {
    err = tr("netCDF : type de données non pris en charge");
    return false;
  }
  Impl& d = *d_;
  const uint64_t total = Product(count);
  if (total > (uint64_t)(SIZE_MAX / 16)) {  // OpenCPN 32 bits : sélection trop grande
    err = tr("netCDF : mémoire insuffisante pour cette lecture");
    return false;
  }
  try {
    out.assign((size_t)total, 0.0);
    if (total == 0) return true;
    const DType dt = format_ == 3 ? d.nc3vars[vi].dt : d.h5vars[vi].dt;
    const size_t es = dt.size;
    std::vector<uint8_t> run;
    // indices courants (toutes dimensions sauf la dernière), parcours en ordre C
    const size_t last = rank ? rank - 1 : 0;
    const uint64_t runLen = rank ? count[last] : 1;
    std::vector<uint64_t> idx(rank, 0);
    auto convert = [&](const uint8_t* p, uint64_t n, double* o) {
      for (uint64_t k = 0; k < n; ++k) o[k] = ToDouble(p + k * es, dt);
    };
    // indices de la série numéro r (toutes dimensions sauf la dernière)
    const uint64_t runs = total / runLen;
    auto setIdx = [&](uint64_t r) {
      for (size_t k = last; k-- > 0;) {
        idx[k] = r % count[k];
        r /= count[k];
      }
    };
    if (format_ == 3) {
      const Nc3Var& x = d.nc3vars[vi];
      if (x.record && rank == 1) {  // un élément par enregistrement
        run.resize(es);
        for (uint64_t r = 0; r < count[0]; ++r) {
          d.src.read(x.begin + (start[0] + r) * d.recSize, run.data(), es);
          convert(run.data(), 1, out.data() + r);
        }
        return true;
      }
      run.resize((size_t)(runLen * es));
      for (uint64_t r = 0; r < runs; ++r) {
        setIdx(r);
        uint64_t lin = 0, rec = 0;
        for (size_t k = 0; k < rank; ++k) {
          const uint64_t i = start[k] + (k == last ? 0 : idx[k]);
          if (k == 0 && x.record) rec = i;
          else lin = lin * v.shape[k] + i;
        }
        const uint64_t pos = x.begin + (x.record ? rec * d.recSize : 0) + lin * es;
        d.src.read(pos, run.data(), run.size());
        convert(run.data(), runLen, out.data() + r * runLen);
      }
      return true;
    }
    H5Var& h = d.h5vars[vi];
    if (h.layout == 0 || h.layout == 1) {
      const size_t n = (size_t)(runLen * es);
      for (uint64_t r = 0; r < runs; ++r) {
        setIdx(r);
        uint64_t lin = 0;
        for (size_t k = 0; k < rank; ++k) lin = lin * v.shape[k] + start[k] + (k == last ? 0 : idx[k]);
        double* o = out.data() + r * runLen;
        if (h.layout == 0) {
          if ((lin + runLen) * es > h.compact.size()) Corrupt("données compactes");
          convert(h.compact.data() + lin * es, runLen, o);
        } else if (h.addr == kUndef) {
          std::vector<uint8_t> f(n, 0);
          if (h.fill.size() == es)
            for (size_t i = 0; i < n; i += es) std::memcpy(f.data() + i, h.fill.data(), es);
          convert(f.data(), runLen, o);
        } else {
          run.resize(n);
          d.src.read(d.h5->A(h.addr) + lin * es, run.data(), n);
          convert(run.data(), runLen, o);
        }
      }
      return true;
    }
    if (rank == 0) Unsupported("scalaire par blocs");
    // par blocs
    d.h5->loadChunks(h);
    std::vector<uint64_t> grid;
    d.h5->chunkGrid(h, grid);
    std::vector<uint64_t> c0(rank), c1(rank), ci(rank);
    for (size_t k = 0; k < rank; ++k) {
      c0[k] = start[k] / h.chunk[k];
      c1[k] = (start[k] + count[k] - 1) / h.chunk[k];
      ci[k] = c0[k];
    }
    // strides de la sortie
    std::vector<uint64_t> ostr(rank, 1), cstr(rank, 1);
    for (size_t k = rank; k-- > 1;) {
      ostr[k - 1] = ostr[k] * count[k];
      cstr[k - 1] = cstr[k] * h.chunk[k];
    }
    for (;;) {
      uint64_t lin = 0;
      for (size_t k = 0; k < rank; ++k) lin = lin * grid[k] + ci[k];
      const std::vector<uint8_t>& cd = d.chunkData(vi, lin, ci);
      // intersection du bloc et de la sélection
      std::vector<uint64_t> lo(rank), hi(rank), j(rank);
      for (size_t k = 0; k < rank; ++k) {
        const uint64_t b0 = ci[k] * h.chunk[k], b1 = b0 + h.chunk[k];
        lo[k] = std::max(b0, start[k]);
        hi[k] = std::min(b1, start[k] + count[k]);
        j[k] = lo[k];
      }
      const uint64_t n = hi[last] - lo[last];
      for (;;) {
        uint64_t so = 0, oo = 0;
        for (size_t k = 0; k < rank; ++k) {
          so += (j[k] - ci[k] * h.chunk[k]) * cstr[k];
          oo += (j[k] - start[k]) * ostr[k];
        }
        convert(cd.data() + so * es, n, out.data() + oo);
        size_t k = last;
        bool done = true;
        while (k-- > 0) {
          if (++j[k] < hi[k]) {
            done = false;
            break;
          }
          j[k] = lo[k];
        }
        if (done) break;
      }
      size_t k = rank;
      bool done = true;
      while (k-- > 0) {
        if (++ci[k] <= c1[k]) {
          done = false;
          break;
        }
        ci[k] = c0[k];
      }
      if (done) break;
    }
    return true;
  } catch (const Fail& e) {
    err = e.what();
  } catch (const std::bad_alloc&) {
    err = tr("netCDF : mémoire insuffisante pour cette lecture");
  } catch (const std::exception&) {
    err = tr("netCDF/HDF5 : fichier tronqué ou endommagé");
  }
  return false;
}

}  // namespace pd

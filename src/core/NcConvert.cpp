// PersonalDepth_pi - conversion netCDF -> .pdtide / .pdgrid (voir NcConvert.h)
//
// Les calculs reprennent pas à pas ceux des scripts Python (arrondi au pair,
// ordre des opérations, pas de contraction en FMA : voir CMakeLists.txt) pour
// donner les mêmes valeurs entières.
//
// Copyright (C) 2026 Marcaurelio Franzetti
// SPDX-License-Identifier: GPL-2.0-or-later
#include "NcConvert.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <limits>
#include <memory>
#include <vector>

#include "FileUtil.h"
#include "GeoTiff.h"
#include "Lang.h"
#include "NetCdf.h"
#include "VerticalRef.h"
#include "miniz/miniz.h"

namespace fs = std::filesystem;

namespace pd {
namespace {

constexpr int T = 32;
constexpr int kBand = 16;  // rangées de tuiles par partie (.pdgrid)
constexpr double kNaN = std::numeric_limits<double>::quiet_NaN();
constexpr double kPi = 3.141592653589793238462643383279502884;

const char* const kNames[] = {"2N2", "Eps2", "J1",   "K1",  "K2",  "L2",  "Lambda2", "M2", "M3",
                              "M4",  "M6",   "M8",   "MKS2", "MN4", "MS4", "MSf",    "Mf", "Mm",
                              "Msqm", "Mtm", "Mu2",  "N2",  "N4",  "Nu2", "O1",     "P1", "Q1",
                              "R2",  "S1",   "S2",   "S4",  "Sa",  "Ssa", "T2"};
constexpr int kWaves = sizeof kNames / sizeof kNames[0];

void Put(std::vector<uint8_t>& b, const void* p, size_t n) {
  const uint8_t* c = (const uint8_t*)p;
  b.insert(b.end(), c, c + n);
}
template <class V> void PutV(std::vector<uint8_t>& b, V v) { Put(b, &v, sizeof v); }  // x86 : petit-boutiste
void PutText(std::vector<uint8_t>& b, const std::string& s, size_t n) {
  std::string t = s.substr(0, n - 1);
  t.resize(n, '\0');
  Put(b, t.data(), n);
}
void PutIndex(std::vector<uint8_t>& b, uint64_t off, uint32_t size, uint8_t kind) {
  PutV(b, off);
  PutV(b, size);
  PutV(b, kind);
  const uint8_t z[3] = {0, 0, 0};
  Put(b, z, 3);
}

bool Compress(const std::vector<uint8_t>& raw, std::vector<uint8_t>& out) {
  mz_ulong n = mz_compressBound((mz_ulong)raw.size());
  out.resize(n);
  if (mz_compress2(out.data(), &n, raw.data(), (mz_ulong)raw.size(), 6) != MZ_OK) return false;
  out.resize(n);
  return true;
}

// Octets séparés : tous les poids faibles, puis tous les forts
void PutSplit16(std::vector<uint8_t>& b, const std::vector<int16_t>& d) {
  const size_t at = b.size();
  b.resize(at + 2 * d.size());
  for (size_t k = 0; k < d.size(); ++k) {
    const uint16_t u = (uint16_t)d[k];
    b[at + k] = (uint8_t)(u & 0xFF);
    b[at + d.size() + k] = (uint8_t)(u >> 8);
  }
}

void PutMask(std::vector<uint8_t>& b, const std::vector<uint8_t>& m) {  // bit k%8 de l'octet k/8
  std::vector<uint8_t> bits(m.size() / 8, 0);
  for (size_t k = 0; k < m.size(); ++k)
    if (m[k]) bits[k / 8] |= (uint8_t)(1u << (k % 8));
  Put(b, bits.data(), bits.size());
}

bool WriteFile(const std::string& path, const std::vector<uint8_t>& a, const std::vector<uint8_t>* b = nullptr) {
  FILE* f = OpenFile(path, "wb");
  if (!f) return false;
  bool ok = a.empty() || std::fwrite(a.data(), 1, a.size(), f) == a.size();
  if (ok && b && !b->empty()) ok = std::fwrite(b->data(), 1, b->size(), f) == b->size();
  ok = std::fclose(f) == 0 && ok;
  return ok;
}

bool ReadFile(const std::string& path, std::vector<uint8_t>& v) {
  FILE* f = OpenFile(path, "rb");
  if (!f) return false;
  v.clear();
  uint8_t buf[1 << 16];
  size_t n;
  while ((n = std::fread(buf, 1, sizeof buf, f)) > 0) v.insert(v.end(), buf, buf + n);
  std::fclose(f);
  return true;
}

bool Replace(const std::string& from, const std::string& to) {
  std::error_code ec;
  fs::rename(U8Path(from), U8Path(to), ec);
  if (!ec) return true;
  fs::remove(U8Path(to), ec);
  fs::rename(U8Path(from), U8Path(to), ec);
  return !ec;
}

bool Exists(const std::string& p) {
  std::error_code ec;
  return fs::exists(U8Path(p), ec);
}

uint64_t FileSize(const std::string& p) {
  std::error_code ec;
  const uint64_t n = fs::file_size(U8Path(p), ec);
  return ec ? 0 : n;
}

// Copie de `in` à la suite de `out` (à partir de l'octet skip)
bool AppendFile(FILE* out, const std::string& in, uint64_t skip) {
  FILE* f = OpenFile(in, "rb");
  if (!f) return false;
  std::vector<uint8_t> buf(1 << 22);
  uint64_t pos = 0;
  bool ok = true;
  size_t n;
  while ((n = std::fread(buf.data(), 1, buf.size(), f)) > 0) {
    size_t s = 0;
    if (pos < skip) s = (size_t)std::min<uint64_t>(n, skip - pos);
    pos += n;
    if (n > s && std::fwrite(buf.data() + s, 1, n - s, out) != n - s) {
      ok = false;
      break;
    }
  }
  std::fclose(f);
  return ok;
}

void RemoveParts(const std::string& dir) {
  std::error_code ec;
  fs::remove_all(U8Path(dir), ec);
}

std::string Join(const std::string& a, const std::string& b) { return (U8Path(a) / U8Path(b)).u8string(); }

std::string Lower(std::string s) {
  for (char& c : s) c = (char)std::tolower((unsigned char)c);
  return s;
}

// Valeurs masquées comme netCDF4-python (set_auto_mask/scale) : _FillValue,
// missing_value, valid_min / valid_max / valid_range, NaN ; puis
// scale_factor et add_offset
struct Mask {
  bool hasFill = false, hasMiss = false;
  double fill = 0, miss = 0, lo = -INFINITY, hi = INFINITY, scale = 1, offset = 0;
  explicit Mask(const NcVar& v) {
    if (const NcAtt* a = v.att("_FillValue"))
      if (!a->text && !a->num.empty()) hasFill = true, fill = a->num[0];
    if (!hasFill && v.floating) {  // remplissage par défaut de netCDF
      hasFill = true;
      fill = v.bytes == 4 ? (double)9.9692099683868690e+36f : 9.9692099683868690e+36;
    }
    if (const NcAtt* a = v.att("missing_value"))
      if (!a->text && !a->num.empty()) hasMiss = true, miss = a->num[0];
    if (const NcAtt* a = v.att("valid_range"))
      if (!a->text && a->num.size() >= 2) lo = a->num[0], hi = a->num[1];
    lo = v.num("valid_min", lo);
    hi = v.num("valid_max", hi);
    scale = v.num("scale_factor", 1.0);
    offset = v.num("add_offset", 0.0);
  }
  double operator()(double x) const {
    if (std::isnan(x) || (hasFill && x == fill) || (hasMiss && x == miss) || x < lo || x > hi) return kNaN;
    if (scale != 1.0) x = x * scale;
    if (offset != 0.0) x = x + offset;
    return x;
  }
};

// ---------------------------------------------------------------- .pdgrid

double TpToWgs84(double latDeg) {
  const double aTp = 6378136.3, fTp = 1.0 / 298.257;
  const double aW = 6378137.0, fW = 1.0 / 298.257223563;
  const double s = std::sin(latDeg * (kPi / 180.0));
  const double s2 = s * s;
  const double rTp = aTp * (1.0 - fTp * s2);
  const double rW = aW * (1.0 - fW * s2);
  return rTp - rW;
}

// Grille source lue par lignes, du sud au nord (nœud (i, j) en lon0 + i·dlon,
// lat0 + j·dlat)
struct GridSrc {
  virtual ~GridSrc() = default;
  bool periodic = false;
  uint64_t ny = 0, nx = 0;
  double dlat = 0, dlon = 0, lat0 = 0, lon0 = 0;
  std::string defName, defPeriod;  // nom et période lus dans le fichier
  // Lignes j0..j1-1 comptées depuis le sud, en m (NaN = absent)
  virtual bool rows(long long j0, long long j1, std::vector<double>& out, std::string& err) = 0;
  void finish() { periodic = std::fabs((double)nx * dlon - 360.0) < 1e-6; }
};

// netCDF : variable 2-D (lat, lon)
struct NcSrc : GridSrc {
  NcFile nc;
  const NcVar* v = nullptr;
  std::vector<double> lat, lon;
  bool flip = false, hasFill = false;
  double fill = 0, scale = 1, offset = 0;

  bool open(const std::string& path, std::string var, std::string& err) {
    if (!nc.open(path, err)) return false;
    if (var.empty()) {  // première variable 2-D avec ses coordonnées (Band1 de GDAL…)
      for (const auto& x : nc.vars())
        if (x.numeric && x.shape.size() == 2 && x.shape[0] >= 2 && x.shape[1] >= 2 && nc.coord(x, 0, 'y') &&
            nc.coord(x, 1, 'x')) {
          var = x.name;
          break;
        }
      if (var.empty()) {
        err = tr("Aucune grille lat / lon dans le fichier");
        return false;
      }
    }
    v = nc.var(var);
    if (!v) {
      err = std::string(tr("Variable absente du fichier : ")) + var;
      return false;
    }
    if (v->shape.size() != 2 || v->shape[0] < 2 || v->shape[1] < 2) {
      err = std::string(tr("Variable : 2 dimensions attendues (lat, lon)")) + " (" + var + ")";
      return false;
    }
    const NcVar* la = nc.coord(*v, 0, 'y');
    const NcVar* lo = nc.coord(*v, 1, 'x');
    if (!la || !lo) {
      err = tr("Coordonnées lat / lon introuvables dans le fichier");
      return false;
    }
    if (!nc.readAll(*la, lat, err) || !nc.readAll(*lo, lon, err)) return false;
    flip = lat[1] < lat[0];
    ny = lat.size();
    nx = lon.size();
    dlat = std::fabs(lat[1] - lat[0]);
    dlon = lon[1] - lon[0];
    lat0 = std::min(lat[0], lat.back());
    lon0 = lon[0];
    finish();
    if (const NcAtt* a = v->att("_FillValue"))
      if (!a->text && !a->num.empty() && !std::isnan(a->num[0])) hasFill = true, fill = a->num[0];
    scale = v->num("scale_factor", 1.0);
    offset = v->num("add_offset", 0.0);
    for (const auto& a : nc.globals())
      if (a.text && a.name == "OriginalName") defName = a.str;
    if (defName.empty())
      for (const auto& a : nc.globals())
        if (a.text && a.name == "title") defName = a.str;
    const std::string ln = v->str("long_name");
    const size_t p = ln.find('['), q = ln.find(']');
    if (p != std::string::npos && q != std::string::npos && q > p)
      for (char c : ln.substr(p + 1, q - p - 1))
        if (c != ' ') defPeriod += c == ',' ? '-' : c;
    return true;
  }

  bool rows(long long j0, long long j1, std::vector<double>& out, std::string& err) override {
    out.assign((size_t)((j1 - j0) * (long long)nx), kNaN);
    const long long c0 = std::max<long long>(0, j0), c1 = std::min<long long>((long long)ny, j1);
    if (c1 <= c0) return true;
    std::vector<double> a;
    const uint64_t s = flip ? ny - (uint64_t)c1 : (uint64_t)c0;
    if (!nc.read(*v, {s, 0}, {(uint64_t)(c1 - c0), nx}, a, err)) return false;
    for (long long j = c0; j < c1; ++j) {
      const long long src = flip ? (c1 - 1 - j) : (j - c0);
      const double* p = a.data() + src * (long long)nx;
      double* o = out.data() + (j - j0) * (long long)nx;
      for (uint64_t i = 0; i < nx; ++i) {
        double x = p[i];
        if (hasFill && x == fill) x = kNaN;
        x = x * scale + offset;
        o[i] = x;
      }
    }
    return true;
  }
};

// GeoTIFF en longitude / latitude (lecteur intégré), première bande
struct TiffSrc : GridSrc {
  GeoTiff t;
  std::vector<float> row;
  bool open(const std::string& path, std::string& err) {
    if (!t.open(path, err)) return false;
    const GeoTiffInfo& g = t.info();
    if (!g.georef || !(g.dx > 0) || !(g.dy > 0) || g.width < 2 || g.height < 2) {
      err = tr("GeoTIFF sans géoréférencement");
      return false;
    }
    if (!g.geographic || !g.crsDef.empty()) {
      err = std::string(tr("GeoTIFF en projection : une grille en longitude / latitude est attendue")) +
            (g.crsLabel.empty() ? std::string() : " (" + g.crsLabel + ")");
      return false;
    }
    nx = (uint64_t)g.width;
    ny = (uint64_t)g.height;
    dlon = g.dx;
    dlat = g.dy;
    lon0 = g.x0 + 0.5 * g.dx;                        // centre des pixels
    lat0 = g.y0 - ((double)g.height - 0.5) * g.dy;  // ligne du bas
    finish();
    row.resize(nx);
    return true;
  }
  bool rows(long long j0, long long j1, std::vector<double>& out, std::string& err) override {
    out.assign((size_t)((j1 - j0) * (long long)nx), kNaN);
    for (long long j = std::max<long long>(0, j0); j < std::min<long long>((long long)ny, j1); ++j) {
      if (!t.readRow((int)(ny - 1 - (uint64_t)j), row.data(), err)) return false;
      double* o = out.data() + (j - j0) * (long long)nx;
      for (uint64_t i = 0; i < nx; ++i) o[i] = row[i];  // NoData -> NaN
    }
    return true;
  }
};

// Grilles .gtx (NOAA / PROJ), .asc (ESRI), .tac / .mnt (IGN), lues en entier
struct MemSrc : GridSrc {
  Grid2D g;
  bool open(const std::string& path, std::string& err) {
    if (!LoadAnyGrid(path, g, err)) return false;
    nx = (uint64_t)g.nx;
    ny = (uint64_t)g.ny;
    dlon = g.dlon;
    dlat = g.dlat;
    lon0 = g.lonW;
    lat0 = g.latN - (double)(g.ny - 1) * g.dlat;
    finish();
    return true;
  }
  bool rows(long long j0, long long j1, std::vector<double>& out, std::string&) override {
    out.assign((size_t)((j1 - j0) * (long long)nx), kNaN);
    for (long long j = std::max<long long>(0, j0); j < std::min<long long>((long long)ny, j1); ++j) {
      double* o = out.data() + (j - j0) * (long long)nx;
      for (uint64_t i = 0; i < nx; ++i) o[i] = g.at((int)i, (int)(ny - 1 - (uint64_t)j));
    }
    return true;
  }
};

std::string ExtLower(const std::string& path) {
  std::string e = U8Path(path).extension().u8string();
  for (char& c : e) c = (char)std::tolower((unsigned char)c);
  return e;
}

std::unique_ptr<GridSrc> OpenGrid(const std::string& path, const std::string& var, std::string& err) {
  const std::string e = ExtLower(path);
  if (e == ".tif" || e == ".tiff") {
    std::unique_ptr<TiffSrc> s(new TiffSrc);
    if (!s->open(path, err)) return nullptr;
    return std::move(s);
  }
  if (e == ".gtx" || e == ".asc" || e == ".tac" || e == ".mnt") {
    std::unique_ptr<MemSrc> s(new MemSrc);
    if (!s->open(path, err)) return nullptr;
    return std::move(s);
  }
  std::unique_ptr<NcSrc> s(new NcSrc);
  if (!s->open(path, var, err)) return nullptr;
  return std::move(s);
}

// Partie b (rangées de tuiles b*kBand…) : index relatif puis tuiles
bool EncodeBand(GridSrc& src, int b, int ntx, int nty, bool tp, bool negate, const std::string& partDir,
                const std::function<bool(int)>& rowDone, std::string& err, bool& stopped) {
  const int tj0 = b * kBand, tj1 = std::min(nty, (b + 1) * kBand);
  std::vector<uint8_t> index, data, raw, blob;
  const size_t W = (size_t)ntx * T;
  std::vector<double> a;
  std::vector<int64_t> mm(W * T);
  std::vector<uint8_t> valid(W * T);
  for (int tj = tj0; tj < tj1; ++tj) {
    if (!src.rows((long long)tj * T, (long long)(tj + 1) * T, a, err)) return false;
    for (int r = 0; r < T; ++r) {
      const double add = tp ? TpToWgs84(src.lat0 + src.dlat * ((double)(tj * T + r) + 0.0)) : 0.0;
      for (size_t i = 0; i < W; ++i) {
        double x = i < src.nx ? a[(size_t)r * src.nx + i] : kNaN;
        if (negate) x = -x;
        if (tp) x = x + add;
        const bool ok = !std::isnan(x);
        valid[(size_t)r * W + i] = ok;
        mm[(size_t)r * W + i] = ok ? (int64_t)std::nearbyint(x * 1000.0) : 0;
      }
    }
    for (int ti = 0; ti < ntx; ++ti) {
      std::vector<int64_t> v(T * T);
      std::vector<uint8_t> m(T * T);
      int nv = 0;
      for (int r = 0; r < T; ++r)
        for (int c = 0; c < T; ++c) {
          const size_t s = (size_t)r * W + (size_t)ti * T + c;
          v[r * T + c] = mm[s];
          m[r * T + c] = valid[s];
          nv += valid[s];
        }
      if (nv == 0) {
        PutIndex(index, 0, 0, 0);
        continue;
      }
      if (nv < T * T) {  // absents : dernière valeur présente (première pour le début)
        int first = 0;
        while (!m[first]) ++first;
        int lastValid = first;
        for (int k = 0; k < T * T; ++k) {
          if (m[k]) lastValid = k;
          v[k] = v[k < first ? first : lastValid];
        }
      }
      raw.clear();
      if (nv < T * T) PutMask(raw, m);
      std::vector<int64_t> d(T * T);
      int64_t mx = 0;
      for (int r = 0; r < T; ++r)
        for (int c = 0; c < T; ++c) {
          int64_t x;
          if (c > 0) x = v[r * T + c] - v[r * T + c - 1];
          else if (r > 0) x = v[r * T] - v[(r - 1) * T];
          else x = 0;
          d[r * T + c] = x;
          mx = std::max<int64_t>(mx, x < 0 ? -x : x);
        }
      uint8_t kind;
      if (mx <= 32767) {
        kind = nv == T * T ? 1 : 2;
        PutV(raw, (int32_t)v[0]);
        std::vector<int16_t> dd(T * T);
        for (int k = 0; k < T * T; ++k) dd[k] = (int16_t)d[k];
        PutSplit16(raw, dd);
      } else {
        kind = nv == T * T ? 3 : 4;
        for (int k = 0; k < T * T; ++k) PutV(raw, (int32_t)v[k]);
      }
      if (!Compress(raw, blob)) {
        err = tr("Compression impossible");
        return false;
      }
      PutIndex(index, data.size(), (uint32_t)blob.size(), kind);
      Put(data, blob.data(), blob.size());
    }
    if (!rowDone(tj)) {
      stopped = true;
      return true;
    }
  }
  char nm[16];
  std::snprintf(nm, sizeof nm, "%03d", b);
  const std::string tmp = Join(partDir, std::string(nm) + ".tmp");
  std::vector<uint8_t> head;
  PutV(head, (uint32_t)index.size());
  Put(head, index.data(), index.size());
  if (!WriteFile(tmp, head, &data) || !Replace(tmp, Join(partDir, std::string(nm) + ".part"))) {
    err = std::string(tr("Écriture impossible : ")) + tmp;
    return false;
  }
  return true;
}

}  // namespace

int ConvertToPdgrid(const std::string& in, const std::string& out, const PdgridOptions& opt,
                    const ConvertProgress& progress, std::string& err) {
  std::unique_ptr<GridSrc> sp = OpenGrid(in, opt.var, err);
  if (!sp) return -1;
  GridSrc& src = *sp;
  if (src.nx > 400000 || src.ny > 400000) {
    err = tr("Grille trop grande");
    return -1;
  }
  std::string name = opt.name.empty() ? src.defName : opt.name;
  if (name.empty()) name = U8Path(in).stem().u8string();
  const std::string period = opt.period.empty() ? src.defPeriod : opt.period;
  const int nty = (int)((src.ny + T - 1) / T), ntx = (int)((src.nx + T - 1) / T);
  const int nbands = (nty + kBand - 1) / kBand;
  const std::string partDir = out + ".parts";
  std::error_code ec;
  fs::create_directories(U8Path(partDir), ec);
  if (ec) {
    err = std::string(tr("Écriture impossible : ")) + partDir;
    return -1;
  }
  auto partPath = [&](int b) {
    char nm[16];
    std::snprintf(nm, sizeof nm, "%03d.part", b);
    return Join(partDir, nm);
  };
  for (int b = 0; b < nbands; ++b) {
    if (Exists(partPath(b))) continue;
    bool stopped = false;
    auto rowDone = [&](int tj) { return progress((double)(tj + 1) / nty, name); };
    if (!progress((double)(b * kBand) / nty, name)) return 0;
    if (!EncodeBand(src, b, ntx, nty, opt.tp2wgs84, opt.negate, partDir, rowDone, err, stopped)) return -1;
    if (stopped) return 0;
  }
  // assemblage
  std::vector<uint8_t> head;
  Put(head, "PDGRID01", 8);
  PutV(head, (uint32_t)T);
  PutV(head, (int32_t)src.nx);
  PutV(head, (int32_t)src.ny);
  PutV(head, (int32_t)ntx);
  PutV(head, (int32_t)nty);
  PutV(head, (uint32_t)(src.periodic ? 1 : 0));
  PutV(head, src.lon0);
  PutV(head, src.lat0);
  PutV(head, src.dlon);
  PutV(head, src.dlat);
  PutV(head, 0.001);
  PutText(head, opt.quantity, 32);
  PutText(head, name, 128);
  PutText(head, period, 32);
  PutText(head, "", 8);
  const uint64_t ntile = (uint64_t)ntx * nty;
  uint64_t base = head.size() + ntile * 16;
  std::vector<uint8_t> idx;
  std::vector<uint64_t> skip(nbands);
  for (int b = 0; b < nbands; ++b) {
    std::vector<uint8_t> p;
    FILE* f = OpenFile(partPath(b), "rb");
    uint32_t n = 0;
    if (!f || std::fread(&n, 4, 1, f) != 1) {
      if (f) std::fclose(f);
      err = std::string(tr("Partie illisible : ")) + partPath(b);
      return -1;
    }
    p.resize(n);
    const bool ok = std::fread(p.data(), 1, n, f) == n;
    std::fclose(f);
    if (!ok || n % 16) {
      err = std::string(tr("Partie illisible : ")) + partPath(b);
      return -1;
    }
    const uint64_t dsize = FileSize(partPath(b)) - 4 - n;
    for (uint32_t k = 0; k < n; k += 16) {
      uint64_t o;
      uint32_t s;
      std::memcpy(&o, p.data() + k, 8);
      std::memcpy(&s, p.data() + k + 8, 4);
      const uint8_t t = p[k + 12];
      PutIndex(idx, t ? base + o : 0, s, t);
    }
    base += dsize;
    skip[b] = 4 + (uint64_t)n;
  }
  if (idx.size() != ntile * 16) {
    err = std::string(tr("Partie illisible : ")) + partDir;
    return -1;
  }
  const std::string tmp = out + ".tmp";
  FILE* f = OpenFile(tmp, "wb");
  bool ok = f && std::fwrite(head.data(), 1, head.size(), f) == head.size() &&
            std::fwrite(idx.data(), 1, idx.size(), f) == idx.size();
  for (int b = 0; ok && b < nbands; ++b) ok = AppendFile(f, partPath(b), skip[b]);
  if (f) ok = std::fclose(f) == 0 && ok;
  if (!ok || !Replace(tmp, out)) {
    err = std::string(tr("Écriture impossible : ")) + out;
    return -1;
  }
  RemoveParts(partDir);
  progress(1.0, name);
  return 1;
}

// ---------------------------------------------------------------- .pdtide

std::string FindFesFolder(const std::string& chosen) {
  std::error_code ec;
  fs::path p = U8Path(chosen);
  if (fs::is_regular_file(p, ec)) p = p.parent_path();
  const fs::path cand[] = {p, p / "ocean_tide_extrapolated", p / "fes2022b" / "ocean_tide_extrapolated",
                           p.parent_path() / "ocean_tide_extrapolated"};
  // chemin normalisé : même texte quel que soit le choix de départ (Windows :
  // barres obliques et contre-obliques mélangées)
  for (const auto& c : cand)
    if (fs::is_regular_file(c / "m2_fes2022.nc", ec)) return fs::path(c).make_preferred().lexically_normal().u8string();
  return std::string();
}

namespace {

struct FesGrid {
  uint64_t nx = 0, ny = 0;
  double lon0 = 0, lat0 = 0, dlon = 0, dlat = 0;
};

// Valeur d'un élément telle que numpy la garde (float32 : calcul en float32)
double Step(const std::vector<double>& c, int bytes) {
  if (bytes == 4) return (double)((float)c[1] - (float)c[0]);
  return c[1] - c[0];
}

bool GridInfo(const std::string& path, FesGrid& g, std::string& err) {
  NcFile nc;
  if (!nc.open(path, err)) return false;
  const NcVar* lo = nc.var("lon");
  const NcVar* la = nc.var("lat");
  if (!lo || !la || lo->shape.size() != 1 || la->shape.size() != 1 || lo->shape[0] < 2 || la->shape[0] < 2) {
    err = std::string(tr("Coordonnées lat / lon introuvables dans le fichier")) + " (" + path + ")";
    return false;
  }
  std::vector<double> lon, lat;
  if (!nc.readAll(*lo, lon, err) || !nc.readAll(*la, lat, err)) return false;
  g.nx = lon.size();
  g.ny = lat.size();
  g.lon0 = lon[0];
  g.lat0 = lat[0];
  g.dlon = Step(lon, lo->bytes);
  g.dlat = Step(lat, la->bytes);
  return true;
}

// Une onde : <part>/<NOM>.bin et <NOM>.idx
bool ConvertWave(const std::string& dir, int w, uint64_t nx0, const std::string& partDir,
                 const std::function<bool(double)>& step, std::string& err, bool& stopped) {
  const std::string name = kNames[w];
  std::string path = Join(dir, Lower(name) + "_fes2022.nc");
  if (!Exists(path)) {  // ex. Lambda2 : absente des grilles extrapolées
    fs::path alt = fs::absolute(U8Path(dir)).parent_path() / "ocean_tide" / U8Path(Lower(name) + "_fes2022.nc");
    path = alt.u8string();
  }
  NcFile nc;
  if (!nc.open(path, err)) {
    err += " (" + path + ")";
    return false;
  }
  nc.setCacheBytes(64u << 20);
  const NcVar* va = nc.var("amplitude");
  const NcVar* vg = nc.var("phase");
  if (!va || !vg || va->shape.size() != 2 || vg->shape != va->shape) {
    err = std::string(tr("Variables amplitude / phase absentes : ")) + path;
    return false;
  }
  const uint64_t ny = va->shape[0], nx = std::min<uint64_t>(nx0, va->shape[1]);
  const int nty = (int)((ny + T - 1) / T), ntx = (int)((nx + T - 1) / T);
  const Mask ma(*va), mg(*vg);
  std::vector<uint8_t> index;
  const std::string tmp = Join(partDir, name + ".bin.tmp");
  FILE* f = OpenFile(tmp, "wb");
  if (!f) {
    err = std::string(tr("Écriture impossible : ")) + tmp;
    return false;
  }
  uint64_t off = 0;
  std::vector<double> A, G;
  const size_t W = (size_t)ntx * T;
  std::vector<int32_t> R(W * T), I(W * T);
  std::vector<uint8_t> V(W * T), raw, blob;
  for (int tj = 0; tj < nty; ++tj) {
    const uint64_t j0 = (uint64_t)tj * T, n = std::min<uint64_t>(T, ny - j0);
    if (!nc.read(*va, {j0, 0}, {n, nx}, A, err) || !nc.read(*vg, {j0, 0}, {n, nx}, G, err)) {
      std::fclose(f);
      err += " (" + path + ")";
      return false;
    }
    std::fill(R.begin(), R.end(), 0);
    std::fill(I.begin(), I.end(), 0);
    std::fill(V.begin(), V.end(), 0);
    for (uint64_t r = 0; r < n; ++r)
      for (uint64_t i = 0; i < nx; ++i) {
        const double a = ma(A[r * nx + i]);
        const double g = mg(G[r * nx + i]) * (kPi / 180.0);
        const double re = a * std::cos(g) * 10.0;  // mm
        const double im = a * std::sin(g) * 10.0;
        if (std::isnan(re)) continue;
        if (std::fabs(re) > 16000 || std::fabs(im) > 16000) {
          std::fclose(f);
          char b[160];
          std::snprintf(b, sizeof b, "%s : amplitude %.0f mm > 16000", name.c_str(),
                        std::max(std::fabs(re), std::fabs(im)));
          err = b;
          return false;
        }
        R[r * W + i] = (int32_t)std::nearbyint(re);
        I[r * W + i] = (int32_t)std::nearbyint(im);
        V[r * W + i] = 1;
      }
    for (int ti = 0; ti < ntx; ++ti) {
      std::vector<uint8_t> m(T * T);
      int nv = 0;
      for (int r = 0; r < T; ++r)
        for (int c = 0; c < T; ++c) nv += (m[r * T + c] = V[(size_t)r * W + ti * T + c]);
      if (nv == 0) {
        PutIndex(index, 0, 0, 0);
        continue;
      }
      raw.clear();
      uint8_t kind = 1;
      if (nv < T * T) {
        kind = 2;
        PutMask(raw, m);
      }
      for (const std::vector<int32_t>* C : {&R, &I}) {
        std::vector<int16_t> dd(T * T);
        for (int r = 0; r < T; ++r) {
          int32_t prev = 0;
          for (int c = 0; c < T; ++c) {
            const int32_t x = (*C)[(size_t)r * W + ti * T + c];
            dd[r * T + c] = (int16_t)(x - prev);
            prev = x;
          }
        }
        PutSplit16(raw, dd);
      }
      if (!Compress(raw, blob) || std::fwrite(blob.data(), 1, blob.size(), f) != blob.size()) {
        std::fclose(f);
        err = std::string(tr("Écriture impossible : ")) + tmp;
        return false;
      }
      PutIndex(index, off, (uint32_t)blob.size(), kind);
      off += blob.size();
    }
    if (!step((double)(tj + 1) / nty)) {
      std::fclose(f);
      RemoveFile(tmp);
      stopped = true;
      return true;
    }
  }
  if (std::fclose(f) != 0 || !WriteFile(Join(partDir, name + ".idx"), index) ||
      !Replace(tmp, Join(partDir, name + ".bin"))) {
    err = std::string(tr("Écriture impossible : ")) + tmp;
    return false;
  }
  return true;
}

}  // namespace

int ConvertFesToPdtide(const std::string& dir, const std::string& out, const ConvertProgress& progress,
                       std::string& err) {
  FesGrid g;
  if (!GridInfo(Join(dir, "m2_fes2022.nc"), g, err)) return -1;
  const std::string partDir = out + ".parts";
  std::error_code ec;
  fs::create_directories(U8Path(partDir), ec);
  if (ec) {
    err = std::string(tr("Écriture impossible : ")) + partDir;
    return -1;
  }
  for (int w = 0; w < kWaves; ++w) {
    if (Exists(Join(partDir, std::string(kNames[w]) + ".bin"))) continue;
    if (!progress((double)w / kWaves, kNames[w])) return 0;
    bool stopped = false;
    auto step = [&](double f) { return progress((w + f) / kWaves, kNames[w]); };
    if (!ConvertWave(dir, w, g.nx, partDir, step, err, stopped)) return -1;
    if (stopped) return 0;
  }
  const uint64_t nty = (g.ny + T - 1) / T, ntx = (g.nx + T - 1) / T, ntile = nty * ntx;
  std::vector<uint8_t> head;
  Put(head, "PDTIDE01", 8);
  PutV(head, (uint32_t)kWaves);
  PutV(head, (uint32_t)T);
  PutV(head, (int32_t)g.nx);
  PutV(head, (int32_t)g.ny);
  PutV(head, (int32_t)ntx);
  PutV(head, (int32_t)nty);
  PutV(head, g.lon0);
  PutV(head, g.lat0);
  PutV(head, g.dlon);
  PutV(head, g.dlat);
  PutV(head, 0.001);
  PutV(head, (uint32_t)(std::fabs((double)g.nx * g.dlon - 360.0) < 1e-6 ? 1 : 0));
  PutV(head, (uint32_t)0);
  for (int w = 0; w < kWaves; ++w) PutText(head, kNames[w], 16);
  std::vector<uint8_t> idx;
  uint64_t base = head.size() + (uint64_t)kWaves * ntile * 16;
  for (int w = 0; w < kWaves; ++w) {
    std::vector<uint8_t> raw;
    const std::string ip = Join(partDir, std::string(kNames[w]) + ".idx");
    if (!ReadFile(ip, raw) || raw.size() != ntile * 16) {
      err = std::string(tr("Partie illisible : ")) + ip;
      return -1;
    }
    for (size_t k = 0; k < raw.size(); k += 16) {
      uint64_t o;
      uint32_t s;
      std::memcpy(&o, raw.data() + k, 8);
      std::memcpy(&s, raw.data() + k + 8, 4);
      const uint8_t t = raw[k + 12];
      PutIndex(idx, t ? base + o : 0, s, t);
    }
    base += FileSize(Join(partDir, std::string(kNames[w]) + ".bin"));
  }
  const std::string tmp = out + ".tmp";
  FILE* f = OpenFile(tmp, "wb");
  bool ok = f && std::fwrite(head.data(), 1, head.size(), f) == head.size() &&
            std::fwrite(idx.data(), 1, idx.size(), f) == idx.size();
  for (int w = 0; ok && w < kWaves; ++w) ok = AppendFile(f, Join(partDir, std::string(kNames[w]) + ".bin"), 0);
  if (f) ok = std::fclose(f) == 0 && ok;
  if (!ok || !Replace(tmp, out)) {
    err = std::string(tr("Écriture impossible : ")) + out;
    return -1;
  }
  RemoveParts(partDir);
  progress(1.0, "");
  return 1;
}

}  // namespace pd

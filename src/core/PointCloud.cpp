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

#include "PointCloud.h"
#include "Lang.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <limits>
#include <sstream>
#include <unordered_map>

#include "Projection.h"

#ifdef PD_HAVE_GDAL
#include <gdal_priv.h>
#endif

namespace pd {

// ---------------------------------------------------------------- PointCloud
void PointCloud::reserve(size_t n) {
  qlon.reserve(n);
  qlat.reserve(n);
  z.reserve(n);
}

void PointCloud::add(double lon, double lat, float depth) {
  qlon.push_back((int32_t)std::llround(lon / kQ));
  qlat.push_back((int32_t)std::llround(lat / kQ));
  z.push_back(depth);
}

void PointCloud::clear() {
  qlon.clear(); qlat.clear(); z.clear();
  qlon.shrink_to_fit(); qlat.shrink_to_fit(); z.shrink_to_fit();
}

void PointCloud::computeBounds() {
  if (empty()) return;
  int32_t a = INT32_MAX, b = INT32_MIN, c = INT32_MAX, d = INT32_MIN;
  float zm = std::numeric_limits<float>::max(), zM = -zm;
  for (size_t i = 0; i < size(); ++i) {
    a = std::min(a, qlon[i]); b = std::max(b, qlon[i]);
    c = std::min(c, qlat[i]); d = std::max(d, qlat[i]);
    zm = std::min(zm, z[i]); zM = std::max(zM, z[i]);
  }
  lonMin = a * kQ; lonMax = b * kQ; latMin = c * kQ; latMax = d * kQ;
  zMin = zm; zMax = zM;
}

static bool IsUserNoData(double v, const std::vector<double>& nd) {
  for (double x : nd)
    if (std::fabs(v - x) <= 1e-6 * std::max(1.0, std::fabs(x))) return true;
  return false;
}

void PointCloud::applySign(const LoadOptions& o, std::vector<uint32_t>* tris) {
  // valeurs absentes : non finies, remplissages énormes, NODATA de l'utilisateur
  filter([&](float v) {
    return std::isfinite(v) && std::fabs(v) < 1e20f && !IsUserNoData(v, o.nodata);
  }, tris);
  bool negate = false;
  if (o.zsign == LoadOptions::ZSign::Elevation) negate = true;
  else if (o.zsign == LoadOptions::ZSign::Auto && !empty()) {
    size_t neg = 0;
    for (float v : z) neg += (v < 0.f);
    negate = neg > (size() * 6) / 10;
  }
  const double k = (negate ? -1.0 : 1.0) * o.zToMeters;  // unité du fichier -> m
  if (k != 1.0 || o.zOffset != 0.f)
    for (float& v : z) v = (float)(v * k) + o.zOffset;
  computeBounds();
}

static const char kMagic1[8] = {'P', 'D', 'P', 'C', '0', '0', '0', '1'};
static const char kMagic[8] = {'P', 'D', 'P', 'C', '0', '0', '0', '2'};

bool PointCloud::saveCache(const std::string& path) const {
  FILE* f = std::fopen(path.c_str(), "wb");
  if (!f) return false;
  uint64_t n = size();
  const double steps[2] = {srcStepX, srcStepY};
  bool ok = std::fwrite(kMagic, 1, 8, f) == 8 && std::fwrite(&n, 8, 1, f) == 1 &&
            std::fwrite(steps, 8, 2, f) == 2 &&
            std::fwrite(qlon.data(), 4, n, f) == n &&
            std::fwrite(qlat.data(), 4, n, f) == n &&
            std::fwrite(z.data(), 4, n, f) == n;
  std::fclose(f);
  if (!ok) std::remove(path.c_str());
  return ok;
}

bool PointCloud::loadCache(const std::string& path) {
  FILE* f = std::fopen(path.c_str(), "rb");
  if (!f) return false;
  char m[8]; uint64_t n = 0;
  bool ok = std::fread(m, 1, 8, f) == 8 &&
            (!std::memcmp(m, kMagic, 8) || !std::memcmp(m, kMagic1, 8)) &&
            std::fread(&n, 8, 1, f) == 1;
  srcStepX = srcStepY = 0;
  if (ok && !std::memcmp(m, kMagic, 8)) {
    double steps[2];
    ok = std::fread(steps, 8, 2, f) == 2;
    srcStepX = steps[0]; srcStepY = steps[1];
  }
  if (ok) {
    qlon.resize(n); qlat.resize(n); z.resize(n);
    ok = std::fread(qlon.data(), 4, n, f) == n &&
         std::fread(qlat.data(), 4, n, f) == n &&
         std::fread(z.data(), 4, n, f) == n;
  }
  std::fclose(f);
  if (!ok) { clear(); return false; }
  computeBounds();
  return true;
}

// ---------------------------------------------------------- parseur rapide
// Lit un nombre décimal ; renvoie false si aucun chiffre. Précision : < 1 ulp
// pour ≤ 15 chiffres significatifs (largement suffisant pour coords et sondes).
static inline bool isSep(char c) {
  return c == ' ' || c == '\t' || c == ',' || c == ';' || c == '\r';
}

static bool parseNum(const char*& p, const char* e, double& out) {
  while (p < e && isSep(*p)) ++p;
  if (p >= e || *p == '\n') return false;
  bool neg = false;
  if (*p == '-' || *p == '+') { neg = (*p == '-'); ++p; }
  uint64_t mant = 0; int exp10 = 0, nd = 0; bool any = false;
  while (p < e && *p >= '0' && *p <= '9') {
    if (nd < 18) { mant = mant * 10 + (*p - '0'); ++nd; } else ++exp10;
    ++p; any = true;
  }
  if (p < e && *p == '.') {
    ++p;
    while (p < e && *p >= '0' && *p <= '9') {
      if (nd < 18) { mant = mant * 10 + (*p - '0'); ++nd; --exp10; }
      ++p; any = true;
    }
  }
  if (!any) return false;
  if (p < e && (*p == 'e' || *p == 'E')) {
    ++p; bool en = false; int ev = 0;
    if (p < e && (*p == '-' || *p == '+')) { en = (*p == '-'); ++p; }
    while (p < e && *p >= '0' && *p <= '9') { ev = ev * 10 + (*p - '0'); ++p; }
    exp10 += en ? -ev : ev;
  }
  static const double p10[] = {1e0, 1e1, 1e2, 1e3, 1e4, 1e5, 1e6, 1e7, 1e8,
                               1e9, 1e10, 1e11, 1e12, 1e13, 1e14, 1e15,
                               1e16, 1e17, 1e18, 1e19, 1e20, 1e21, 1e22};
  double v = (double)mant;
  if (exp10 < 0) v = (exp10 >= -22) ? v / p10[-exp10] : v * std::pow(10.0, exp10);
  else if (exp10 > 0) v = (exp10 <= 22) ? v * p10[exp10] : v * std::pow(10.0, exp10);
  out = neg ? -v : v;
  // un nombre doit être suivi d'un séparateur ou d'une fin de ligne
  return p >= e || isSep(*p) || *p == '\n';
}

double LatticeStepUnits(const std::vector<int64_t>& q) {
  if (q.size() < 16) return 0;
  std::vector<int64_t> u(q);
  std::sort(u.begin(), u.end());
  u.erase(std::unique(u.begin(), u.end()), u.end());
  if (u.size() < 4) return 0;
  std::vector<int64_t> d(u.size() - 1);
  for (size_t k = 0; k + 1 < u.size(); ++k) d[k] = u[k + 1] - u[k];
  std::vector<int64_t> sd(d);
  std::sort(sd.begin(), sd.end());
  int64_t best = 0; size_t bestN = 0;
  for (size_t a = 0, b = 0; a < sd.size(); a = b) {
    b = a;
    while (b < sd.size() && sd[b] - sd[a] <= 4) ++b;
    if (b - a > bestN) { bestN = b - a; best = sd[(a + b) / 2]; }
  }
  if (best < 5) return 0;
  double sum = 0; size_t n = 0, ok = 0;
  for (int64_t x : d) {
    if (std::llabs(x - best) <= 2) { sum += (double)x; ++n; }
    double k = std::round((double)x / best);
    if (k >= 1 && std::fabs(x - k * best) <= 2 + k) ++ok;
  }
  if (ok < d.size() * 95 / 100 || n == 0) return 0;
  return sum / n;
}

namespace {
struct ParseState {
  const LoadOptions& o;
  PointCloud& pc;
  size_t outOfRange = 0, failed = 0;
  double sampleX = 0, sampleY = 0;
  bool haveSample = false;
  std::vector<int64_t> sx, sy;  // échantillon (mm) pour détecter un pas de grille
  ParseState(const LoadOptions& oo, PointCloud& p) : o(oo), pc(p) {}

  void add(double a, double b, double z) {
    if (o.order == LoadOptions::Order::LatLonZ) std::swap(a, b);
    if (o.crs) {
      if (!haveSample) { sampleX = a; sampleY = b; haveSample = true; }
      if (sx.size() < 300000) {
        sx.push_back(std::llround(a * 1000.0));
        sy.push_back(std::llround(b * 1000.0));
      }
      double lon, lat;
      if (!o.crs->toWGS84(a, b, lon, lat)) { ++failed; return; }
      pc.add(lon, lat, (float)z);
      return;
    }
    if (a < -180 || a > 360 || b < -90 || b > 90) {  // entête, bruit… ou mauvais système
      if (std::fabs(a) > 1000 || std::fabs(b) > 1000) {
        if (!haveSample) { sampleX = a; sampleY = b; haveSample = true; }
        ++outOfRange;
      }
      return;
    }
    if (a > 180) a -= 360;
    pc.add(a, b, (float)z);
  }
  // Pas de grille de la source projetée (mètres)
  void finish() {
    if (!o.crs || o.crs->isGeographic()) return;
    const double ux = LatticeStepUnits(sx), uy = LatticeStepUnits(sy);
    if (ux <= 0 || uy <= 0) return;
    // Un semis irrégulier aux coordonnées arrondies (ex. Litto3D « PTS » au cm)
    // ressemble à une grille de pas = résolution d'arrondi : on exige qu'une
    // part raisonnable des nœuds de l'emprise soit occupée.
    const auto [x0, x1] = std::minmax_element(sx.begin(), sx.end());
    const auto [y0, y1] = std::minmax_element(sy.begin(), sy.end());
    const double nodes = ((double)(*x1 - *x0) / ux + 1) * ((double)(*y1 - *y0) / uy + 1);
    if ((double)sx.size() < 0.02 * nodes) return;
    pc.srcStepX = ux / 1000.0 * o.crs->unitToMeter();
    pc.srcStepY = uy / 1000.0 * o.crs->unitToMeter();
  }
  std::string emptyMessage() const {
    if (outOfRange && haveSample) {
      char b[160];
      std::snprintf(b, sizeof b, pd::tr("Coordonnées hors plage lon/lat (ex. %.2f %.2f) : "), sampleX, sampleY);
      return std::string(b) + GuessCrsHint(sampleX, sampleY);
    }
    if (failed) return pd::tr("Conversion impossible : vérifier le système de coordonnées");
    return pd::tr("Aucune sonde lisible (format attendu : X Y Z)");
  }
};
}  // namespace

static void parseLine(const char* p, const char* e, ParseState& st) {
  double a, b, c;
  if (!parseNum(p, e, a) || !parseNum(p, e, b) || !parseNum(p, e, c)) return;
  st.add(a, b, c);
}

bool LoadXYZ(const std::string& path, const LoadOptions& opt, PointCloud& pc,
             std::string& err, const ProgressFn& progress,
             const std::atomic<bool>* cancel) {
  FILE* f = std::fopen(path.c_str(), "rb");
  if (!f) { err = pd::tr("Impossible d'ouvrir ") + path; return false; }
  std::fseek(f, 0, SEEK_END);
  long long total = std::ftell(f);
  std::fseek(f, 0, SEEK_SET);
  pc.clear();
  pc.reserve((size_t)std::max<long long>(1024, total / 28));  // ~28 o/ligne

  const size_t CH = 16u << 20;
  std::vector<char> buf(CH + 4096);
  size_t carry = 0; long long done = 0;
  ParseState st(opt, pc);
  for (;;) {
    size_t n = std::fread(buf.data() + carry, 1, CH, f);
    size_t len = carry + n;
    if (len == 0) break;
    const char* s = buf.data();
    const char* end = s + len;
    const char* lastNL = (n == 0) ? end : nullptr;
    if (!lastNL)
      for (const char* q = end - 1; q >= s; --q) if (*q == '\n') { lastNL = q + 1; break; }
    if (!lastNL) lastNL = end;  // ligne > 16 Mo : on la traite telle quelle
    const char* p = s;
    while (p < lastNL) {
      const char* nl = (const char*)std::memchr(p, '\n', lastNL - p);
      const char* le = nl ? nl : lastNL;
      parseLine(p, le, st);
      p = nl ? nl + 1 : lastNL;
    }
    carry = end - lastNL;
    std::memmove(buf.data(), lastNL, carry);
    done += n;
    if (progress && total > 0) progress((float)done / (float)total);
    if (cancel && *cancel) { std::fclose(f); err = "Annulé"; return false; }
    if (n == 0) break;
  }
  std::fclose(f);
  if (pc.empty()) { err = st.emptyMessage(); return false; }
  st.finish();
  pc.applySign(opt);
  return true;
}

// --------------------------------------------------------------- ESRI ASCII
// Valeur absente : égalité à la précision d'écriture près (« -3.40282e+38 »
// contre « -3.4028234663852886e+38 » dans l'entête), ou valeur non finie.
static bool NoData(double v, double nodata) {
  if (!std::isfinite(v) || std::fabs(v) >= 1e20) return true;
  return std::fabs(v - nodata) <= 1e-6 * std::max(1.0, std::fabs(nodata));
}

bool LoadESRIAscii(const std::string& path, const LoadOptions& opt,
                   PointCloud& pc, double& cellLon, double& cellLat,
                   std::string& err, const std::atomic<bool>* cancel) {
  std::ifstream in(path);
  if (!in) { err = pd::tr("Impossible d'ouvrir ") + path; return false; }
  long ncols = 0, nrows = 0; double xll = 0, yll = 0, cs = 0, dx = 0, dy = 0;
  double nodata = -9999; bool center = false;
  std::string key;
  for (int k = 0; k < 7; ++k) {
    std::streampos pos = in.tellg();
    if (!(in >> key)) break;
    std::string lk = key;
    std::transform(lk.begin(), lk.end(), lk.begin(), ::tolower);
    if (lk == "ncols") in >> ncols;
    else if (lk == "nrows") in >> nrows;
    else if (lk == "xllcorner") in >> xll;
    else if (lk == "xllcenter") { in >> xll; center = true; }
    else if (lk == "yllcorner") in >> yll;
    else if (lk == "yllcenter") { in >> yll; center = true; }
    else if (lk == "cellsize") in >> cs;
    else if (lk == "dx") in >> dx;
    else if (lk == "dy") in >> dy;
    else if (lk == "nodata_value") in >> nodata;
    else { in.seekg(pos); break; }
  }
  if (cs > 0) dx = dy = cs;
  if (ncols <= 0 || nrows <= 0 || dx <= 0 || dy <= 0) { err = pd::tr("Entête ASC invalide"); return false; }
  if (center) { xll -= dx / 2; yll -= dy / 2; }
  pc.clear();
  pc.reserve((size_t)ncols * nrows);
  // corps lu par blocs avec l'analyseur rapide (ifstream >> double est très
  // lent, surtout sous MSVC) ; un jeton illisible (nan…) vaut NODATA
  const long long total = (long long)ncols * nrows;
  long long idx = 0;
  const size_t kChunk = (size_t)8 << 20;
  std::vector<char> buf;
  std::string carry;
  bool last = false;
  double lat = 0;
  // grille projetée : conversion exacte d'un nœud sur kStep le long de la
  // ligne, interpolation linéaire entre les deux (écart < 1 mm sur 32 cellules)
  const int kStep = 32;
  std::vector<double> nlon, nlat;
  std::vector<uint8_t> nok;
  auto rowNodes = [&](double y) {
    const long nn = (ncols - 1) / kStep + 2;
    nlon.assign(nn, 0); nlat.assign(nn, 0); nok.assign(nn, 0);
    for (long k = 0; k < nn; ++k) {
      const long c = std::min<long>(k * kStep, ncols - 1);
      nok[k] = opt.crs->toWGS84(xll + (c + 0.5) * dx, y, nlon[k], nlat[k]) ? 1 : 0;
    }
  };
  while (idx < total && !last) {
    buf.assign(carry.begin(), carry.end());
    carry.clear();
    const size_t old = buf.size();
    buf.resize(old + kChunk);
    in.read(buf.data() + old, (std::streamsize)kChunk);
    const size_t got = (size_t)in.gcount();
    buf.resize(old + got);
    last = got < kChunk;
    size_t end = buf.size();
    if (!last) {  // jeton coupé en fin de bloc : reporté au bloc suivant
      while (end > 0 && !(isSep(buf[end - 1]) || buf[end - 1] == '\n')) --end;
      carry.assign(buf.begin() + (std::ptrdiff_t)end, buf.end());
    }
    const char* p = buf.data();
    const char* e = buf.data() + end;
    while (idx < total) {
      while (p < e && (isSep(*p) || *p == '\n')) ++p;
      if (p >= e) break;
      const char* s0 = p;
      double v;
      if (!parseNum(p, e, v)) {
        p = s0;
        while (p < e && !(isSep(*p) || *p == '\n')) ++p;
        v = NAN;
      }
      const long r = (long)(idx / ncols), c = (long)(idx % ncols);
      ++idx;
      if (c == 0) {
        if (cancel && *cancel && (r & 63) == 0) { err = "Annulé"; return false; }
        lat = yll + (nrows - 1 - r + 0.5) * dy;
        if (opt.crs) rowNodes(lat);
      }
      if (NoData(v, nodata)) continue;
      const double x = xll + (c + 0.5) * dx;
      if (opt.crs) {
        const long k = c / kStep;
        const long c0 = k * kStep, c1 = std::min<long>((k + 1) * kStep, ncols - 1);
        double lo, la;
        if (c == c0 && nok[k]) {
          pc.add(nlon[k], nlat[k], (float)v);
        } else if (nok[k] && nok[k + 1] && c1 > c0) {
          const double t = double(c - c0) / double(c1 - c0);
          pc.add(nlon[k] + t * (nlon[k + 1] - nlon[k]), nlat[k] + t * (nlat[k + 1] - nlat[k]), (float)v);
        } else if (opt.crs->toWGS84(x, lat, lo, la)) {  // bord du domaine : conversion exacte
          pc.add(lo, la, (float)v);
        }
      } else {
        pc.add(x, lat, (float)v);
      }
    }
  }
  if (idx < total) { err = pd::tr("Fichier ASC tronqué"); return false; }
  if (pc.empty()) {
    err = (!opt.crs && (std::fabs(xll) > 1000 || std::fabs(yll) > 1000))
              ? pd::tr("Grille hors plage lon/lat : ") + GuessCrsHint(xll, yll)
              : pd::tr("Grille vide");
    return false;
  }
  cellLon = dx; cellLat = dy;
  if (opt.crs && !opt.crs->isGeographic()) {
    pc.srcStepX = dx * opt.crs->unitToMeter();
    pc.srcStepY = dy * opt.crs->unitToMeter();
  }
  pc.applySign(opt);
  return true;
}

// ------------------------------------------------------------- LandXML TIN
static bool nextTag(const std::string& s, size_t& pos, const char* tag,
                    std::string& attrs, std::string& body) {
  std::string open = std::string("<") + tag;
  for (;;) {
    size_t a = s.find(open, pos);
    if (a == std::string::npos) return false;
    char c = s[a + open.size()];
    if (c != ' ' && c != '>' && c != '\t') { pos = a + 1; continue; }
    size_t gt = s.find('>', a);
    if (gt == std::string::npos) return false;
    attrs = s.substr(a + open.size(), gt - a - open.size());
    size_t close = s.find(std::string("</") + tag + ">", gt);
    if (close == std::string::npos) return false;
    body = s.substr(gt + 1, close - gt - 1);
    pos = close;
    return true;
  }
}

bool LoadLandXMLTin(const std::string& path, const LoadOptions& opt,
                    PointCloud& pc, std::vector<uint32_t>& tris,
                    std::string& err) {
  std::ifstream in(path, std::ios::binary);
  if (!in) { err = pd::tr("Impossible d'ouvrir ") + path; return false; }
  std::stringstream ss; ss << in.rdbuf();
  const std::string s = ss.str();
  std::unordered_map<long, uint32_t> idmap;
  pc.clear(); tris.clear();
  std::string attrs, body;
  size_t pos = 0;
  while (nextTag(s, pos, "P", attrs, body)) {
    size_t q = attrs.find("id=\"");
    long id = (q != std::string::npos) ? std::strtol(attrs.c_str() + q + 4, nullptr, 10)
                                       : (long)pc.size() + 1;
    // LandXML : "Nord Est Élévation" -> lat lon z
    std::istringstream b(body); double la, lo, zz;
    if (!(b >> la >> lo >> zz)) continue;
    if (opt.crs) {  // N E projetés -> WGS84
      double x = lo, y = la;
      if (!opt.crs->toWGS84(x, y, lo, la)) continue;
    }
    idmap[id] = (uint32_t)pc.size();
    pc.add(lo, la, (float)zz);
  }
  pos = 0;
  while (nextTag(s, pos, "F", attrs, body)) {
    std::istringstream b(body); long i, j, k;
    if (!(b >> i >> j >> k)) continue;
    auto I = idmap.find(i), J = idmap.find(j), K = idmap.find(k);
    if (I == idmap.end() || J == idmap.end() || K == idmap.end()) continue;
    tris.push_back(I->second); tris.push_back(J->second); tris.push_back(K->second);
  }
  if (pc.empty()) { err = pd::tr("Aucun point <P> trouvé"); return false; }
  // LandXML : élévations -> en Auto, convertit en profondeurs si majoritairement négatives
  pc.applySign(opt, &tris);
  return true;
}

#ifdef PD_HAVE_GDAL
bool LoadGdalRaster(const std::string& path, const LoadOptions& opt,
                    PointCloud& pc, double& cellLon, double& cellLat,
                    std::string& err) {
  GDALAllRegister();
  GDALDataset* ds = (GDALDataset*)GDALOpen(path.c_str(), GA_ReadOnly);
  if (!ds) { err = pd::tr("GDAL ne peut pas ouvrir ") + path; return false; }
  double gt[6];
  if (ds->GetGeoTransform(gt) != CE_None || gt[2] != 0 || gt[4] != 0) {
    GDALClose(ds); err = pd::tr("Géoréférencement absent ou tourné"); return false;
  }
  GDALRasterBand* band = ds->GetRasterBand(1);
  int okNd = 0; double nd = band->GetNoDataValue(&okNd);
  int nx = ds->GetRasterXSize(), ny = ds->GetRasterYSize();
  std::vector<float> row(nx);
  pc.clear(); pc.reserve((size_t)nx * ny / 2);
  for (int j = 0; j < ny; ++j) {
    if (band->RasterIO(GF_Read, 0, j, nx, 1, row.data(), nx, 1, GDT_Float32, 0, 0) != CE_None) break;
    for (int i = 0; i < nx; ++i) {
      float v = row[i];
      if ((okNd && v == (float)nd) || std::isnan(v) || v > 1e30f) continue;
      pc.add(gt[0] + (i + 0.5) * gt[1], gt[3] + (j + 0.5) * gt[5], v);
    }
  }
  cellLon = std::fabs(gt[1]); cellLat = std::fabs(gt[5]);
  GDALClose(ds);
  if (pc.empty()) { err = pd::tr("Raster vide"); return false; }
  pc.applySign(opt);
  return true;
}
#endif

}  // namespace pd

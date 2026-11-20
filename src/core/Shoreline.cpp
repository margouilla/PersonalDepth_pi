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

#include "Shoreline.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <memory>

#include "FileUtil.h"
#include "Lang.h"
#include "Projection.h"
#include "miniz.h"
#include "sqlite3.h"

namespace pd {
namespace fs = std::filesystem;

namespace {
std::string Lower(std::string s) {
  for (char& c : s) c = (char)std::tolower((unsigned char)c);
  return s;
}
std::string ExtOf(const std::string& p) { return Lower(fs::u8path(p).extension().u8string()); }

bool ReadWhole(const std::string& path, std::vector<uint8_t>& b, std::string& err) {
  FILE* f = OpenFile(path, "rb");
  if (!f) { err = pd::tr("Impossible d'ouvrir ") + path; return false; }
  std::error_code ec;
  const auto n = fs::file_size(fs::u8path(path), ec);
  b.resize(ec ? 0 : (size_t)n);
  const bool ok = b.empty() || std::fread(b.data(), 1, b.size(), f) == b.size();
  std::fclose(f);
  if (!ok) err = pd::tr("Lecture impossible : ") + path;
  return ok;
}

// conversion vers WGS84 (nullptr : déjà en lon/lat)
struct ToWgs {
  std::shared_ptr<Crs> crs;
  bool operator()(double x, double y, double& lon, double& lat) const {
    if (!crs) { lon = x; lat = y; }
    else if (!crs->toWGS84(x, y, lon, lat)) return false;
    if (lon > 180) lon -= 360;
    return std::isfinite(lon) && std::isfinite(lat) && std::fabs(lat) <= 90;
  }
};

bool CrsFromEpsg(int code, ToWgs& t, std::string& err) {
  if (code <= 0 || code == 4326 || code == 4979 || code == 4258 || code == 4171) return true;
  auto c = std::make_shared<Crs>();
  if (!Crs::Parse("EPSG:" + std::to_string(code), *c, err)) {
    err = pd::tr("Système de coordonnées non pris en charge : EPSG:") + std::to_string(code) +
          pd::tr(" (convertir le fichier en WGS84, EPSG:4326)");
    return false;
  }
  if (!c->isWGS84LonLat()) t.crs = c;
  return true;
}

void Push(std::vector<ShoreFeature>& out, ShoreFeature&& f) {
  // ligne refermée sur elle-même (premier point = dernier, à ~1 cm près) :
  // contour de lac ou d'île tracé en polyligne (export QGIS…), pris comme anneau
  if (!f.closed && f.x.size() >= 4 && std::fabs(f.x.front() - f.x.back()) < 1e-7 &&
      std::fabs(f.y.front() - f.y.back()) < 1e-7) {
    f.closed = true;
    f.x.back() = f.x.front();
    f.y.back() = f.y.front();
  }
  // anneau : point de fermeture répété retiré ; au moins 3 sommets (2 pour une ligne)
  if (f.closed && f.x.size() > 1 && f.x.front() == f.x.back() && f.y.front() == f.y.back()) {
    f.x.pop_back();
    f.y.pop_back();
  }
  if (f.x.size() < (f.closed ? 3u : 2u)) return;
  out.push_back(std::move(f));
}

// --------------------------------------------------------------------- KML
const char* LocalStart(const char* a, const char* e) {
  const char* b = a;
  for (const char* q = a; q < e; ++q)
    if (*q == ':') b = q + 1;
  return b;
}

bool ParseKml(const char* p, const char* end, std::vector<ShoreFeature>& out) {
  int geom = 0;  // 1 anneau, 2 ligne
  size_t before = out.size();
  while (p < end) {
    const char* lt = (const char*)std::memchr(p, '<', end - p);
    if (!lt) break;
    const char* gt = (const char*)std::memchr(lt, '>', end - lt);
    if (!gt) break;
    p = gt + 1;
    const bool closing = lt[1] == '/';
    const char* ns = lt + (closing ? 2 : 1);
    const char* ne = ns;
    while (ne < gt && !std::isspace((unsigned char)*ne) && *ne != '/') ++ne;
    const std::string name = Lower(std::string(LocalStart(ns, ne), ne));
    if (name == "linearring") geom = closing ? 0 : 1;
    else if (name == "linestring") geom = closing ? 0 : 2;
    else if (name == "coordinates" && !closing && geom && gt[-1] != '/') {
      const char* te = (const char*)std::memchr(p, '<', end - p);
      if (!te) te = end;
      ShoreFeature f;
      f.closed = geom == 1;
      const char* q = p;
      while (q < te) {
        while (q < te && std::isspace((unsigned char)*q)) ++q;
        if (q >= te) break;
        const char* tok = q;
        while (q < te && !std::isspace((unsigned char)*q)) ++q;
        std::string s(tok, q);
        double lon, lat;
        char* e1 = nullptr;
        lon = std::strtod(s.c_str(), &e1);
        if (e1 == s.c_str() || *e1 != ',') continue;
        char* e2 = nullptr;
        lat = std::strtod(e1 + 1, &e2);
        if (e2 == e1 + 1 || std::fabs(lat) > 90 || std::fabs(lon) > 360) continue;
        if (lon > 180) lon -= 360;
        f.x.push_back(lon);
        f.y.push_back(lat);
      }
      Push(out, std::move(f));
      p = te;
    }
  }
  return out.size() > before;
}

// KMZ : archive zip, premier fichier .kml (stocké ou deflate)
bool ReadKmz(const std::vector<uint8_t>& z, std::vector<ShoreFeature>& out, std::string& err) {
  size_t pos = 0;
  while (pos + 30 <= z.size()) {
    uint32_t sig;
    std::memcpy(&sig, &z[pos], 4);
    if (sig != 0x04034b50) break;
    uint16_t flags, method, nlen, xlen;
    uint32_t csize, usize;
    std::memcpy(&flags, &z[pos + 6], 2);
    std::memcpy(&method, &z[pos + 8], 2);
    std::memcpy(&csize, &z[pos + 18], 4);
    std::memcpy(&usize, &z[pos + 22], 4);
    std::memcpy(&nlen, &z[pos + 26], 2);
    std::memcpy(&xlen, &z[pos + 28], 2);
    const std::string name((const char*)&z[pos + 30], std::min<size_t>(nlen, z.size() - pos - 30));
    const size_t data = pos + 30 + nlen + xlen;
    if ((flags & 8) || data + csize > z.size()) break;  // taille en fin d'entrée : non géré
    if (Lower(name).size() > 4 && Lower(name).compare(Lower(name).size() - 4, 4, ".kml") == 0) {
      if (method == 0) {
        const char* s = (const char*)&z[data];
        if (ParseKml(s, s + csize, out)) return true;
      } else if (method == 8) {
        size_t n = 0;
        void* u = tinfl_decompress_mem_to_heap(&z[data], csize, &n, 0);
        if (u) {
          const bool ok = ParseKml((const char*)u, (const char*)u + n, out);
          mz_free(u);
          if (ok) return true;
        }
      }
    }
    pos = data + csize;
  }
  err = pd::tr("KMZ : aucun polygone ni ligne lisible");
  return false;
}

// ------------------------------------------------------------- shapefile
// Système du .prj (WGS84 si absent)
bool ShpCrs(const std::string& path, ToWgs& to, std::string& err) {
  fs::path prj = fs::u8path(path);
  prj.replace_extension(".prj");
  std::vector<uint8_t> pb;
  std::error_code ec;
  if (!fs::exists(prj, ec) || !ReadWhole(prj.u8string(), pb, err)) return true;
  const std::string w(pb.begin(), pb.end());
  // code EPSG reconnu, sinon projection et paramètres lus dans le WKT
  const std::string def = CrsDefFromPrj(w);
  if (def.empty()) {
    if (!WktIsProjected(w)) return true;  // WGS84 géographique
    err = pd::tr("Système du .prj non reconnu (convertir le fichier en WGS84, EPSG:4326) : ") + prj.u8string();
    return false;
  }
  auto c = std::make_shared<Crs>();
  if (!Crs::Parse(def, *c, err)) {
    err = pd::tr("Système du .prj non reconnu (convertir le fichier en WGS84, EPSG:4326) : ") + prj.u8string();
    return false;
  }
  if (!c->isWGS84LonLat()) to.crs = c;
  return true;
}

// Parties d'un enregistrement (contenu après l'entête de 8 octets)
void ShpRecord(const uint8_t* c, size_t len, const ToWgs& to, std::vector<ShoreFeature>& out) {
  if (len < 44) return;
  int32_t type;
  std::memcpy(&type, c, 4);
  const int base = type % 10 == 3 ? 3 : type % 10 == 5 ? 5 : 0;  // 3/13/23, 5/15/25
  if (!base) return;
  int32_t np, npt;
  std::memcpy(&np, c + 36, 4);
  std::memcpy(&npt, c + 40, 4);
  if (np <= 0 || npt <= 0 || 44 + 4 * (size_t)np + 16 * (size_t)npt > len) return;
  const uint8_t* parts = c + 44;
  const uint8_t* pts = parts + 4 * (size_t)np;
  for (int32_t k = 0; k < np; ++k) {
    int32_t a, e = npt;
    std::memcpy(&a, parts + 4 * k, 4);
    if (k + 1 < np) std::memcpy(&e, parts + 4 * (k + 1), 4);
    if (a < 0 || e > npt || e <= a) continue;
    ShoreFeature f;
    f.closed = base == 5;
    f.x.reserve(e - a);
    f.y.reserve(e - a);
    for (int32_t q = a; q < e; ++q) {
      double xy[2], lon, lat;
      std::memcpy(xy, pts + 16 * (size_t)q, 16);
      if (to(xy[0], xy[1], lon, lat)) { f.x.push_back(lon); f.y.push_back(lat); }
    }
    Push(out, std::move(f));
  }
}

bool ReadShp(const std::string& path, std::vector<ShoreFeature>& out, std::string& err) {
  ShpSource s;
  if (!s.open(path, err)) return false;
  const size_t before = out.size();
  for (size_t r = 0; r < s.count(); ++r)
    if (!s.read(r, out, err)) return false;
  if (out.size() == before) { err = pd::tr("Shapefile sans polygone ni polyligne : ") + path; return false; }
  return true;
}
}  // namespace

static void Seek64(FILE* f, long long o) {
#if defined(_WIN32)
  _fseeki64(f, o, SEEK_SET);
#else
  fseeko(f, (off_t)o, SEEK_SET);
#endif
}

// Shapefile lu à la demande : index .shx et emprises des enregistrements
struct ShpSource::Impl {
  ToWgs to;
  FILE* f = nullptr;
  std::vector<uint64_t> off, len;  // octets
  ~Impl() { if (f) std::fclose(f); }
};
ShpSource::ShpSource() : impl_(new Impl) {}
ShpSource::~ShpSource() = default;

bool ShpSource::open(const std::string& path, std::string& err) {
  box_.clear();
  impl_.reset(new Impl);
  Impl& I = *impl_;
  if (!ShpCrs(path, I.to, err)) return false;
  fs::path shx = fs::u8path(path);
  shx.replace_extension(".shx");
  std::vector<uint8_t> x;
  std::error_code ec;
  if (!fs::exists(shx, ec)) shx.replace_extension(".SHX");
  if (!ReadWhole(shx.u8string(), x, err) || x.size() < 100) {
    err = pd::tr("Shapefile illisible : ") + path + " (.shx)";
    return false;
  }
  auto be32 = [&](size_t o) { return (uint32_t)x[o] << 24 | (uint32_t)x[o + 1] << 16 | (uint32_t)x[o + 2] << 8 | x[o + 3]; };
  int32_t type;
  std::memcpy(&type, &x[32], 4);
  // polylignes : peuvent contenir des contours refermés (anneaux), lus à la demande
  polygons_ = type % 10 == 5 || type % 10 == 3;
  if (type % 10 != 3 && type % 10 != 5) { err = pd::tr("Shapefile sans polygone ni polyligne : ") + path; return false; }
  for (size_t o = 100; o + 8 <= x.size(); o += 8) {
    I.off.push_back(2ull * be32(o));
    I.len.push_back(2ull * be32(o + 4));
  }
  I.f = OpenFile(path, "rb");
  if (!I.f) { err = pd::tr("Impossible d'ouvrir ") + path; return false; }
  // emprise de chaque enregistrement en lon/lat (coins et milieux des côtés
  // de l'emprise d'origine, plus une marge pour les projections coniques)
  box_.resize(I.off.size());
  uint8_t h[44];
  for (size_t r = 0; r < I.off.size(); ++r) {
    Box& B = box_[r];
    B.ok = false;
    Seek64(I.f, (long long)I.off[r] + 8);
    if (I.len[r] < 44 || std::fread(h, 1, 36, I.f) != 36) continue;
    int32_t t;
    std::memcpy(&t, h, 4);
    if (t == 0) continue;
    double bb[4];
    std::memcpy(bb, h + 4, 32);
    B.x0 = B.y0 = 1e9;
    B.x1 = B.y1 = -1e9;
    for (int i = 0; i < 3; ++i)
      for (int j = 0; j < 3; ++j) {
        double lon, lat;
        if (!I.to(bb[0] + (bb[2] - bb[0]) * i / 2, bb[1] + (bb[3] - bb[1]) * j / 2, lon, lat)) continue;
        B.x0 = std::min(B.x0, lon); B.x1 = std::max(B.x1, lon);
        B.y0 = std::min(B.y0, lat); B.y1 = std::max(B.y1, lat);
        B.ok = true;
      }
    const double m = I.to.crs ? 0.02 : 0.0;
    B.x0 -= m; B.x1 += m; B.y0 -= m; B.y1 += m;
  }
  return true;
}

size_t ShpSource::count() const { return impl_->off.size(); }

bool ShpSource::read(size_t r, std::vector<ShoreFeature>& out, std::string& err) {
  Impl& I = *impl_;
  if (r >= I.off.size() || !I.f) return true;
  std::vector<uint8_t> c(I.len[r]);
  Seek64(I.f, (long long)I.off[r] + 8);
  if (!c.empty() && std::fread(c.data(), 1, c.size(), I.f) != c.size()) {
    err = std::string(pd::tr("Lecture impossible : ")) + "shp";
    return false;
  }
  ShpRecord(c.data(), c.size(), I.to, out);
  return true;
}

namespace {
// ------------------------------------------------------------- GeoPackage
struct Wkb {
  const uint8_t* p;
  size_t n, o = 0;
  bool le = true;
  bool ok = true;
  uint32_t u32() {
    if (o + 4 > n) { ok = false; return 0; }
    uint32_t v;
    std::memcpy(&v, p + o, 4);
    o += 4;
    if (!le) v = (v >> 24) | ((v >> 8) & 0xFF00) | ((v << 8) & 0xFF0000) | (v << 24);
    return v;
  }
  double f64() {
    if (o + 8 > n) { ok = false; return 0; }
    uint64_t v;
    std::memcpy(&v, p + o, 8);
    o += 8;
    if (!le) {
      uint64_t r = 0;
      for (int k = 0; k < 8; ++k) r = (r << 8) | ((v >> (8 * k)) & 0xFF);
      v = r;
    }
    double d;
    std::memcpy(&d, &v, 8);
    return d;
  }
};

bool ParseWkb(Wkb& w, const ToWgs& to, std::vector<ShoreFeature>& out, int depth = 0) {
  if (depth > 8 || w.o >= w.n) return false;
  w.le = w.p[w.o++] == 1;
  uint32_t t = w.u32();
  int nd = 2;
  if (t & 0x20000000) w.u32();  // EWKB : SRID
  if (t & 0x80000000) ++nd;     // EWKB Z
  if (t & 0x40000000) ++nd;     // EWKB M
  t &= 0x0FFFFFFF;
  if (t >= 1000) {  // ISO : 1000 Z, 2000 M, 3000 ZM
    const uint32_t dim = t / 1000;
    nd = 2 + (dim == 1 || dim == 2 ? 1 : dim == 3 ? 2 : 0);
    t %= 1000;
  }
  auto ring = [&](bool closed) {
    const uint32_t np = w.u32();
    if (!w.ok || np > (w.n - w.o) / (8u * nd)) { w.ok = false; return; }
    ShoreFeature f;
    f.closed = closed;
    f.x.reserve(np);
    f.y.reserve(np);
    for (uint32_t k = 0; k < np; ++k) {
      const double x = w.f64(), y = w.f64();
      for (int d = 2; d < nd; ++d) w.f64();
      double lon, lat;
      if (to(x, y, lon, lat)) { f.x.push_back(lon); f.y.push_back(lat); }
    }
    Push(out, std::move(f));
  };
  switch (t) {
    case 1: for (int d = 0; d < nd; ++d) w.f64(); return w.ok;  // point : ignoré
    case 2: ring(false); return w.ok;
    case 3: {
      const uint32_t nr = w.u32();
      for (uint32_t r = 0; r < nr && w.ok; ++r) ring(true);
      return w.ok;
    }
    case 4: case 5: case 6: case 7: {
      const uint32_t ng = w.u32();
      for (uint32_t g = 0; g < ng && w.ok; ++g)
        if (!ParseWkb(w, to, out, depth + 1)) return false;
      return w.ok;
    }
    default: return false;  // courbes, surfaces… : non gérées
  }
}

bool ReadGpkg(const std::string& path, std::vector<ShoreFeature>& out, std::string& err) {
  sqlite3* db = nullptr;
  if (sqlite3_open_v2(fs::u8path(path).u8string().c_str(), &db, SQLITE_OPEN_READONLY, nullptr) != SQLITE_OK) {
    if (db) sqlite3_close(db);
    err = pd::tr("Impossible d'ouvrir ") + path;
    return false;
  }
  struct Tab { std::string table, col; int srs; };
  std::vector<Tab> tabs;
  sqlite3_stmt* st = nullptr;
  if (sqlite3_prepare_v2(db, "SELECT table_name, column_name, srs_id FROM gpkg_geometry_columns", -1, &st,
                         nullptr) == SQLITE_OK) {
    while (sqlite3_step(st) == SQLITE_ROW)
      tabs.push_back({(const char*)sqlite3_column_text(st, 0), (const char*)sqlite3_column_text(st, 1),
                      sqlite3_column_int(st, 2)});
  }
  sqlite3_finalize(st);
  if (tabs.empty()) {
    sqlite3_close(db);
    err = pd::tr("GeoPackage sans table d'entités : ") + path;
    return false;
  }
  auto quote = [](const std::string& s) {
    std::string q = "\"";
    for (char c : s) { if (c == '"') q += '"'; q += c; }
    return q + "\"";
  };
  const size_t before = out.size();
  for (const Tab& t : tabs) {
    ToWgs to;
    int code = t.srs;
    if (t.srs > 0 &&
        sqlite3_prepare_v2(db, "SELECT organization, organization_coordsys_id FROM gpkg_spatial_ref_sys WHERE srs_id=?",
                           -1, &st, nullptr) == SQLITE_OK) {
      sqlite3_bind_int(st, 1, t.srs);
      if (sqlite3_step(st) == SQLITE_ROW) {
        const char* org = (const char*)sqlite3_column_text(st, 0);
        if (org && Lower(org) == "epsg") code = sqlite3_column_int(st, 1);
      }
      sqlite3_finalize(st);
    }
    std::string e2;
    if (!CrsFromEpsg(code, to, e2)) { err = e2; continue; }
    const std::string sql = "SELECT " + quote(t.col) + " FROM " + quote(t.table);
    if (sqlite3_prepare_v2(db, sql.c_str(), -1, &st, nullptr) != SQLITE_OK) continue;
    while (sqlite3_step(st) == SQLITE_ROW) {
      const uint8_t* g = (const uint8_t*)sqlite3_column_blob(st, 0);
      const size_t n = (size_t)sqlite3_column_bytes(st, 0);
      if (!g || n < 8 || g[0] != 'G' || g[1] != 'P') continue;
      const uint8_t flags = g[3];
      if (flags & 0x10) continue;  // géométrie vide
      const int env = (flags >> 1) & 7;
      const size_t off = 8 + (env == 1 ? 32 : env == 2 || env == 3 ? 48 : env == 4 ? 64 : 0);
      if (off >= n) continue;
      Wkb w{g + off, n - off};
      ParseWkb(w, to, out);
    }
    sqlite3_finalize(st);
  }
  sqlite3_close(db);
  if (out.size() == before) {
    if (err.empty()) err = pd::tr("GeoPackage sans polygone ni ligne : ") + path;
    return false;
  }
  err.clear();
  return true;
}
}  // namespace

bool IsShoreFile(const std::string& path) {
  const std::string e = ExtOf(path);
  return e == ".kml" || e == ".kmz" || e == ".shp" || e == ".gpkg";
}

bool ReadShoreFile(const std::string& path, std::vector<ShoreFeature>& out, std::string& err) {
  const std::string e = ExtOf(path);
  if (e == ".shp") return ReadShp(path, out, err);
  if (e == ".gpkg") return ReadGpkg(path, out, err);
  if (e == ".kml" || e == ".kmz") {
    std::vector<uint8_t> b;
    if (!ReadWhole(path, b, err)) return false;
    if (b.size() >= 4 && b[0] == 'P' && b[1] == 'K') return ReadKmz(b, out, err);
    const char* s = (const char*)b.data();
    if (ParseKml(s, s + b.size(), out)) return true;
    err = pd::tr("KML : aucun polygone ni ligne lisible : ") + path;
    return false;
  }
  err = pd::tr("Trait de côte : format non pris en charge (.kml .kmz .shp .gpkg) : ") + path;
  return false;
}

}  // namespace pd

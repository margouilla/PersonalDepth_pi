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

#include "Datum.h"

#include <algorithm>
#include <cctype>
#include <functional>
#include <cmath>
#include <cstring>
#include <chrono>
#include <filesystem>
#include <random>

#include "FileUtil.h"
#include "GeoGrid.h"
#include "Lang.h"
#include "TideFES.h"
#include "sqlite3.h"

namespace pd {
namespace fs = std::filesystem;

std::string DatumName(Datum d) {
  switch (d) {
    case Datum::Legacy: return pd::tr("zéro des cartes (base à migrer)");
    case Datum::Cd: return pd::tr("zéro des cartes local");
    case Datum::Lat: return pd::tr("plus basse mer astronomique (LAT)");
    case Datum::Ell: return pd::tr("ellipsoïde WGS84");
    case Datum::Msl: return pd::tr("niveau moyen");
    case Datum::Free: return pd::tr("non rattachée");
  }
  return std::string();
}

std::string DatumOriginText(Datum d, int mask, const std::string& mssModel, const std::string& national) {
  std::vector<std::string> parts;
  if (mask & kOrigBathyElli) parts.push_back(d == Datum::Cd ? pd::tr("ZH BathyElli v2.1") : "BathyElli v2.1");
  if (mask & kOrigNational) parts.push_back(national.empty() ? std::string(pd::tr("zéro national")) : national);
  if (mask & kOrigFes) parts.push_back(d == Datum::Cd ? pd::tr("LAT FES2022b") : "FES2022b");
  if (mask & kOrigMss) parts.push_back(mssModel.empty() ? std::string("MSS") : mssModel);
  if (mask & kOrigNone) parts.push_back(pd::tr("non défini"));
  std::string s;
  for (size_t k = 0; k < parts.size(); ++k) s += (k ? " + " : "") + parts[k];
  return s;
}

// ================================================================ zéros nationaux
std::vector<NationalDatum> ScanNationalDatums(const std::string& dir) {
  std::vector<NationalDatum> out;
  std::error_code ec;
  const fs::path root = fs::u8path(dir);
  if (dir.empty() || !fs::is_directory(root, ec)) return out;
  for (fs::recursive_directory_iterator it(root, fs::directory_options::skip_permission_denied, ec), end;
       !ec && it != end; it.increment(ec)) {
    if (!it->is_regular_file(ec)) continue;
    std::string fn = it->path().filename().u8string();
    for (char& c : fn) c = (char)std::tolower((unsigned char)c);
    NationalDatum d;
    d.path = it->path().u8string();
    // NOAA VDatum : <région>_mllw.gtx (MLLW − LMSL) ; incertitudes (_unc, _svu) écartées
    const bool mllw = fn.size() >= 8 && fn.compare(fn.size() - 8, 8, "mllw.gtx") == 0 &&
                      fn.find("unc") == std::string::npos && fn.find("svu") == std::string::npos;
    if (mllw) {
      FILE* f = OpenFile(d.path, "rb");
      unsigned char h[40];
      const bool ok = f && std::fread(h, 1, 40, f) == 40;
      if (f) std::fclose(f);
      if (!ok) continue;
      auto be64 = [&](int o) {
        uint64_t u = 0;
        for (int i = 0; i < 8; ++i) u = (u << 8) | h[o + i];
        double x;
        std::memcpy(&x, &u, 8);
        return x;
      };
      auto be32 = [&](int o) { return (int)((uint32_t)h[o] << 24 | (uint32_t)h[o + 1] << 16 | (uint32_t)h[o + 2] << 8 | h[o + 3]); };
      const double lat0 = be64(0), lon0 = be64(8), dla = be64(16), dlo = be64(24);
      const int nr = be32(32), nc = be32(36);
      if (!(dla > 0 && dlo > 0 && nr > 1 && nc > 1)) continue;
      d.kind = NationalDatum::CdBelowMsl;
      d.name = "MLLW NOAA VDatum";
      d.lonW = lon0 > 180 ? lon0 - 360 : lon0;
      d.lonE = d.lonW + (nc - 1) * dlo;
      d.latS = lat0;
      d.latN = lat0 + (nr - 1) * dla;
      out.push_back(d);
    } else if (fn.size() > 7 && fn.compare(fn.size() - 7, 7, ".pdgrid") == 0) {
      GeoGridFile g;
      std::string e;
      if (!g.open(d.path, e)) continue;
      std::string q = g.quantity();
      for (char& c : q) c = (char)std::toupper((unsigned char)c);
      if (q == "CD/MSL") d.kind = NationalDatum::CdBelowMsl;
      else if (q == "CD/WGS84" || q == "CD/ELL") d.kind = NationalDatum::CdEllipsoid;
      else continue;  // MSS…
      d.name = g.model().empty() ? it->path().stem().u8string() : g.model();
      out.push_back(d);
    }
  }
  std::sort(out.begin(), out.end(), [](const NationalDatum& a, const NationalDatum& b) { return a.path < b.path; });
  return out;
}

std::string NationalNames(const std::vector<NationalDatum>& v) {
  std::vector<std::string> n;
  for (const auto& d : v)
    if (std::find(n.begin(), n.end(), d.name) == n.end()) n.push_back(d.name);
  std::string s;
  for (size_t k = 0; k < n.size(); ++k) s += (k ? ", " : "") + n[k];
  return s;
}

// Fenêtre [lonW, lonE] × [latS, latN] (+ un nœud) d'une grille .gtx, sans
// lire tout le fichier (régions VDatum de 40 Mo et plus) ; Grid2D vide hors
// de la grille. Longitudes du fichier en 0..360 ou −180..180.
static bool LoadGtxWindow(const std::string& path, double lonW, double lonE, double latS, double latN,
                          Grid2D& g, std::string& err) {
  g = Grid2D();
  FILE* f = OpenFile(path, "rb");
  if (!f) { err = pd::tr("Impossible d'ouvrir ") + path; return false; }
  unsigned char h[40];
  if (std::fread(h, 1, 40, f) != 40) { std::fclose(f); err = pd::tr("Fichier .gtx tronqué"); return false; }
  auto be64 = [&](int o) {
    uint64_t u = 0;
    for (int i = 0; i < 8; ++i) u = (u << 8) | h[o + i];
    double x;
    std::memcpy(&x, &u, 8);
    return x;
  };
  auto be32 = [&](int o) { return (int)((uint32_t)h[o] << 24 | (uint32_t)h[o + 1] << 16 | (uint32_t)h[o + 2] << 8 | h[o + 3]); };
  const double lat0 = be64(0), lon0 = be64(8), dla = be64(16), dlo = be64(24);
  const int nr = be32(32), nc = be32(36);
  if (!(dla > 0 && dlo > 0 && nr > 1 && nc > 1)) { std::fclose(f); err = pd::tr("Entête .gtx invalide"); return false; }
  // longitudes de la fenêtre dans la convention du fichier
  double a = lonW, b = lonE;
  if (lon0 > 180 && a < 0) { a += 360; b += 360; }
  long i0 = (long)std::floor((a - lon0) / dlo) - 1, i1 = (long)std::ceil((b - lon0) / dlo) + 1;
  long j0 = (long)std::floor((latS - lat0) / dla) - 1, j1 = (long)std::ceil((latN - lat0) / dla) + 1;
  i0 = std::max(0L, i0); j0 = std::max(0L, j0);
  i1 = std::min((long)nc - 1, i1); j1 = std::min((long)nr - 1, j1);
  if (i0 > i1 || j0 > j1) { std::fclose(f); return true; }
  const long w = i1 - i0 + 1, hgt = j1 - j0 + 1;
  g.nx = (int)w; g.ny = (int)hgt; g.dlon = dlo; g.dlat = dla;
  g.lonW = lon0 + i0 * dlo;
  if (g.lonW > 180) g.lonW -= 360;
  g.latN = lat0 + j1 * dla;
  g.v.assign((size_t)w * hgt, NAN);
  std::vector<unsigned char> row((size_t)w * 4);
  for (long j = j0; j <= j1; ++j) {
    const long long off = 40 + ((long long)j * nc + i0) * 4;
#if defined(_WIN32)
    _fseeki64(f, off, SEEK_SET);
#else
    fseeko(f, (off_t)off, SEEK_SET);
#endif
    if (std::fread(row.data(), 1, row.size(), f) != row.size()) break;
    float* dst = &g.v[(size_t)(j1 - j) * w];  // ligne 0 = nord
    for (long i = 0; i < w; ++i) {
      const unsigned char* p = &row[(size_t)i * 4];
      const uint32_t u = (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
      float x;
      std::memcpy(&x, &u, 4);
      dst[i] = (std::fabs(x + 88.8888f) < 1e-3f || !std::isfinite(x)) ? NAN : x;
    }
  }
  std::fclose(f);
  return true;
}

// ================================================================ calcul
struct DatumProvider::Impl {
  TideAtlas atlas;
  bool atlasTried = false, atlasOk = false;
  GeoGridFile mss;
  bool mssTried = false, mssOk = false;
  // zéros nationaux ouverts (gtx en mémoire, pdgrid par tuiles)
  std::map<std::string, std::unique_ptr<Grid2D>> gtx;
  std::map<std::string, std::unique_ptr<GeoGridFile>> pdg;
};

DatumProvider::DatumProvider(const DatumConfig& c) : cfg_(c), impl_(new Impl) {}
DatumProvider::~DatumProvider() = default;

bool DatumProvider::available(Datum k) const {
  switch (k) {
    case Datum::Cd: return true;
    case Datum::Lat: return !cfg_.fesAtlas.empty();
    case Datum::Ell: return !cfg_.mssGrid.empty() || !cfg_.nmFiles.empty();
    case Datum::Msl: case Datum::Free: return true;
    default: return false;
  }
}

const std::string& DatumProvider::mssModel() {
  if (!impl_->mssTried && !cfg_.mssGrid.empty()) {
    std::string e;
    impl_->mssTried = true;
    impl_->mssOk = impl_->mss.open(cfg_.mssGrid, e);
  }
  return impl_->mss.model();
}

// LAT FES2022b (négative, sous le NM) aux nœuds de g ; NaN hors modèle
bool DatumProvider::fesLat(const Grid2D& g, std::vector<float>& lat, std::string& err,
                           const std::atomic<bool>* cancel) {
  lat.assign(g.v.size(), NAN);
  if (cfg_.fesAtlas.empty()) return true;
  if (!impl_->atlasTried) {
    impl_->atlasTried = true;
    impl_->atlasOk = impl_->atlas.open(cfg_.fesAtlas, err);
    if (!impl_->atlasOk) return false;
  }
  if (!impl_->atlasOk) { err = pd::tr("Atlas de marée FES2022 illisible"); return false; }
  const double lonW = g.lonW, lonE = g.lonW + (g.nx - 1) * g.dlon;
  const double latN = g.latN, latS = g.latN - (g.ny - 1) * g.dlat;
  TideRegion r;
  std::string e;
  if (!impl_->atlas.loadRegion(lonW - 0.05, lonE + 0.05, latS - 0.05, latN + 0.05, r, e)) {
    // zone hors du modèle (terres) : pas de LAT, sans erreur
    return true;
  }
  const size_t n = (size_t)r.nx * r.ny;
  // plus basse mer sur un sous-maillage si la zone est grande (≤ ~2500 nœuds),
  // puis interpolation bilinéaire entre ses nœuds
  size_t nValid = 0;
  for (size_t k = 0; k < n; ++k) nValid += r.valid[k];
  if (!nValid) return true;
  const int st = std::max(1, (int)std::ceil(std::sqrt(nValid / 2500.0)));
  auto onLattice = [&](int i, int j) {
    return (i % st == 0 || i == r.nx - 1) && (j % st == 0 || j == r.ny - 1);
  };
  std::vector<size_t> coarse;
  for (size_t k = 0; k < n; ++k)
    if (r.valid[k] && onLattice((int)(k % r.nx), (int)(k / r.nx))) coarse.push_back(k);
  std::vector<float> low;
  LowestAstronomicalTide(r, coarse, low, cancel, nullptr);
  if (cancel && *cancel) { err = "Annulé"; return false; }
  std::vector<float> field(n, NAN);
  for (size_t q = 0; q < coarse.size(); ++q) field[coarse[q]] = low[q];
  if (st > 1) {
    std::vector<size_t> rest;
    for (size_t k = 0; k < n; ++k) {
      if (!r.valid[k] || !std::isnan(field[k])) continue;
      const int i = (int)(k % r.nx), j = (int)(k / r.nx);
      const int ia = i / st * st, ja = j / st * st;
      const int ib = std::min(r.nx - 1, ia + st), jb = std::min(r.ny - 1, ja + st);
      const double fx = ib > ia ? (double)(i - ia) / (ib - ia) : 0, fy = jb > ja ? (double)(j - ja) / (jb - ja) : 0;
      const int ci[4] = {ia, ib, ia, ib}, cj[4] = {ja, ja, jb, jb};
      const double w[4] = {(1 - fx) * (1 - fy), fx * (1 - fy), (1 - fx) * fy, fx * fy};
      double s = 0, ws = 0;
      for (int q = 0; q < 4; ++q) {
        const float x = low.empty() ? NAN : field[(size_t)cj[q] * r.nx + ci[q]];
        if (!std::isnan(x) && w[q] > 0) { s += w[q] * x; ws += w[q]; }
      }
      if (ws > 0) field[k] = (float)(s / ws);
      else rest.push_back(k);
    }
    if (!rest.empty()) {
      LowestAstronomicalTide(r, rest, low, cancel, nullptr);
      for (size_t q = 0; q < rest.size(); ++q) field[rest[q]] = low[q];
    }
  }
  for (int j = 0; j < g.ny; ++j)
    for (int i = 0; i < g.nx; ++i)
      lat[(size_t)j * g.nx + i] = (float)r.sample(field, g.lonW + i * g.dlon, g.latN - j * g.dlat);
  return true;
}

bool DatumProvider::compute(Datum k, double lonMin, double lonMax, double latMin, double latMax,
                            Grid2D& g, std::vector<uint8_t>& origin, std::string& err,
                            const std::atomic<bool>* cancel) {
  g = Grid2D();
  origin.clear();
  if (!DatumHasSurface(k)) { err = pd::tr("Référence verticale sans surface"); return false; }
  const long i0 = (long)std::floor(lonMin * kDatumPerDeg), i1 = (long)std::ceil(lonMax * kDatumPerDeg);
  const long j0 = (long)std::floor(latMin * kDatumPerDeg), j1 = (long)std::ceil(latMax * kDatumPerDeg);
  if ((double)(i1 - i0 + 1) * (j1 - j0 + 1) > 2.5e7) {
    err = pd::tr("Emprise trop grande pour la référence verticale");
    return false;
  }
  g.dlon = g.dlat = 1.0 / kDatumPerDeg;
  g.lonW = (double)i0 / kDatumPerDeg;
  g.latN = (double)j1 / kDatumPerDeg;
  g.nx = (int)(i1 - i0 + 1);
  g.ny = (int)(j1 - j0 + 1);
  const size_t n = (size_t)g.nx * g.ny;
  g.v.assign(n, NAN);
  origin.assign(n, 0);
  // BathyElli : NM/ell (Ell) ou NM/ell − ZH/ell (Cd)
  const bool be = !cfg_.nmFiles.empty() && (k == Datum::Ell || (k == Datum::Cd && !cfg_.zhFiles.empty()));
  if (be) {
    Grid2D a = g, b = g;
    if (!SampleSurfaces(cfg_.nmFiles, a, err)) return false;
    if (k == Datum::Cd && !SampleSurfaces(cfg_.zhFiles, b, err)) return false;
    for (size_t q = 0; q < n; ++q) {
      const float v = k == Datum::Cd ? a.v[q] - b.v[q] : a.v[q];
      if (!std::isnan(v)) { g.v[q] = v; origin[q] = kOrigBathyElli; }
    }
  }
  if (cancel && *cancel) { err = "Annulé"; return false; }
  auto missing = [&]() {
    for (float v : g.v)
      if (std::isnan(v)) return true;
    return false;
  };
  // zéro des cartes officiel d'un autre pays : moyenne des fichiers qui
  // couvrent le nœud (régions VDatum voisines), prolongée de quelques nœuds
  // vers la côte (grilles limitées à l'eau)
  if (k == Datum::Cd && missing() && !cfg_.national.empty()) {
    const double lonE = g.lonW + (g.nx - 1) * g.dlon, latS = g.latN - (g.ny - 1) * g.dlat;
    std::vector<double> acc(n, 0.0);
    std::vector<int> cnt(n, 0);
    Grid2D msl;  // niveau moyen / ellipsoïde (fichiers « CD/WGS84 »)
    bool mslDone = false;
    for (const NationalDatum& f : cfg_.national) {
      if (f.lonE < g.lonW - 0.1 || f.lonW > lonE + 0.1 || f.latN < latS - 0.1 || f.latS > g.latN + 0.1) continue;
      if (cancel && *cancel) { err = "Annulé"; return false; }
      std::function<double(double, double)> at;
      if (f.path.size() > 4 && (f.path.compare(f.path.size() - 4, 4, ".gtx") == 0 ||
                                f.path.compare(f.path.size() - 4, 4, ".GTX") == 0)) {
        auto& G = impl_->gtx[f.path];
        G.reset(new Grid2D);  // fenêtre de la zone seulement
        std::string e;
        if (!LoadGtxWindow(f.path, g.lonW, lonE, latS, g.latN, *G, e)) G->v.clear();
        const Grid2D* gp = G.get();
        at = [gp](double lo, double la) { return gp->sample(lo, la); };
      } else {
        auto& P = impl_->pdg[f.path];
        if (!P) {
          P.reset(new GeoGridFile);
          std::string e;
          P->open(f.path, e);
        }
        GeoGridFile* pp = P.get();
        at = [pp](double lo, double la) { return pp->isOpen() ? pp->sample(lo, la) : NAN; };
      }
      if (f.kind == NationalDatum::CdEllipsoid && !mslDone) {
        mslDone = true;
        msl = g;
        std::fill(msl.v.begin(), msl.v.end(), NAN);
        if (!cfg_.nmFiles.empty() && !SampleSurfaces(cfg_.nmFiles, msl, err)) return false;
        if (!cfg_.mssGrid.empty()) {
          mssModel();
          for (int j = 0; j < g.ny; ++j)
            for (int i = 0; i < g.nx; ++i) {
              float& m = msl.v[(size_t)j * g.nx + i];
              if (std::isnan(m) && impl_->mssOk) m = (float)impl_->mss.sample(g.lonW + i * g.dlon, g.latN - j * g.dlat);
            }
        }
      }
      for (int j = 0; j < g.ny; ++j)
        for (int i = 0; i < g.nx; ++i) {
          const size_t q = (size_t)j * g.nx + i;
          if (!std::isnan(g.v[q])) continue;
          const double v = at(g.lonW + i * g.dlon, g.latN - j * g.dlat);
          if (std::isnan(v)) continue;
          double sv;
          if (f.kind == NationalDatum::CdBelowMsl) sv = -v;  // NM au-dessus du zéro
          else if (!std::isnan(msl.v[q])) sv = msl.v[q] - v;
          else continue;
          acc[q] += sv;
          ++cnt[q];
        }
    }
    Grid2D N = g;
    for (size_t q = 0; q < n; ++q) N.v[q] = cnt[q] ? (float)(acc[q] / cnt[q]) : NAN;
    ExtendGrid(N, 6);  // ~5 km vers la côte et les estuaires
    for (size_t q = 0; q < n; ++q)
      if (std::isnan(g.v[q]) && !std::isnan(N.v[q])) { g.v[q] = N.v[q]; origin[q] = kOrigNational; }
  }
  if ((k == Datum::Cd || k == Datum::Lat) && missing() && !cfg_.fesAtlas.empty()) {
    std::vector<float> lat;
    if (!fesLat(g, lat, err, cancel)) return false;
    Grid2D L = g;
    L.v = lat;
    ExtendGrid(L, 24);  // côte, estuaires : ~0,2° au-delà des nœuds FES
    for (size_t q = 0; q < n; ++q)
      if (std::isnan(g.v[q]) && !std::isnan(L.v[q])) { g.v[q] = -L.v[q]; origin[q] = kOrigFes; }
  }
  if (k == Datum::Ell && missing() && !cfg_.mssGrid.empty()) {
    mssModel();
    if (!impl_->mssOk) { err = pd::tr("Fichier MSS illisible : ") + cfg_.mssGrid; return false; }
    for (int j = 0; j < g.ny; ++j)
      for (int i = 0; i < g.nx; ++i) {
        const size_t q = (size_t)j * g.nx + i;
        if (!std::isnan(g.v[q])) continue;
        const double v = impl_->mss.sample(g.lonW + i * g.dlon, g.latN - j * g.dlat);
        if (!std::isnan(v)) { g.v[q] = (float)v; origin[q] = kOrigMss; }
      }
  }
  if (k == Datum::Cd)  // sans référence : écart nul (profondeurs au zéro des cartes d'origine)
    for (size_t q = 0; q < n; ++q)
      if (std::isnan(g.v[q])) { g.v[q] = 0.f; origin[q] = kOrigNone; }
  return true;
}

// ================================================================ fichier
DatumStore::~DatumStore() { close(); }

void DatumStore::close() {
  std::lock_guard<std::recursive_mutex> lk(mx_);
  if (db_) {
    if (inTx_) sqlite3_exec(db_, "ROLLBACK", nullptr, nullptr, nullptr);
    sqlite3_close(db_);
  }
  db_ = nullptr;
  inTx_ = false;
  cache_.clear();
}

bool DatumStore::open(const std::string& folder, std::string& err) {
  close();
  std::lock_guard<std::recursive_mutex> lk(mx_);
  path_ = (fs::u8path(folder) / FileName()).u8string();
  std::error_code ec;
  if (!fs::exists(fs::u8path(path_), ec)) return true;  // créé à la première écriture
  return ensureFile(err);
}

bool DatumStore::ensureFile(std::string& err) {
  if (db_) return true;
  if (path_.empty()) { err = pd::tr("Base non ouverte"); return false; }
  if (sqlite3_open_v2(fs::u8path(path_).u8string().c_str(), &db_,
                      SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_NOMUTEX,
                      nullptr) != SQLITE_OK) {
    err = pd::tr("Base : ouverture impossible de ") + path_;
    if (db_) { sqlite3_close(db_); db_ = nullptr; }
    return false;
  }
  sqlite3_busy_timeout(db_, 15000);
  char* msg = nullptr;
  if (sqlite3_exec(db_,
                   "PRAGMA journal_mode=WAL;"
                   "CREATE TABLE IF NOT EXISTS meta(key TEXT PRIMARY KEY, value TEXT);"
                   "INSERT OR IGNORE INTO meta VALUES('format', 'PersonalDepth datum 1');"
                   "INSERT OR IGNORE INTO meta VALUES('rev', '0');"
                   "CREATE TABLE IF NOT EXISTS surf(kind INTEGER, tx INTEGER, ty INTEGER,"
                   " origin INTEGER, model TEXT, data BLOB, PRIMARY KEY(kind, tx, ty));",
                   nullptr, nullptr, &msg) != SQLITE_OK) {
    err = std::string(pd::tr("Base : ")) + (msg ? msg : "") + " (" + path_ + ")";
    sqlite3_free(msg);
    return false;
  }
  // identifiant du fichier (copies des surfaces dans les .pdb : voir stamp())
  std::mt19937_64 rng(std::random_device{}() ^
                      (uint64_t)std::chrono::steady_clock::now().time_since_epoch().count());
  char uid[64];
  std::snprintf(uid, sizeof uid, "INSERT OR IGNORE INTO meta VALUES('uid', '%016llx');",
                (unsigned long long)rng());
  sqlite3_exec(db_, uid, nullptr, nullptr, nullptr);
  return true;
}

void DatumStore::bumpRev() {
  if (db_) sqlite3_exec(db_, "UPDATE meta SET value=CAST(value AS INTEGER)+1 WHERE key='rev'", nullptr, nullptr, nullptr);
  dirty_ = false;
}

std::string DatumStore::stamp() {
  std::lock_guard<std::recursive_mutex> lk(mx_);
  if (!db_) return std::string();
  std::string uid, rev;
  sqlite3_stmt* q = nullptr;
  sqlite3_prepare_v2(db_, "SELECT key, value FROM meta WHERE key IN ('uid', 'rev')", -1, &q, nullptr);
  while (q && sqlite3_step(q) == SQLITE_ROW) {
    const char* k = (const char*)sqlite3_column_text(q, 0);
    const char* v = (const char*)sqlite3_column_text(q, 1);
    if (k && v) (std::strcmp(k, "uid") == 0 ? uid : rev) = v;
  }
  sqlite3_finalize(q);
  return uid.empty() ? std::string() : uid + ":" + rev;
}

void DatumStore::TileOf(double lon, double lat, int& tx, int& ty) {
  tx = (int)std::floor(lon * kDatumPerDeg / kDatumTileNodes);
  ty = (int)std::floor(lat * kDatumPerDeg / kDatumTileNodes);
}

void DatumStore::TileBox(int tx, int ty, double& lonW, double& latS) {
  lonW = (double)tx * kDatumTileNodes / kDatumPerDeg;
  latS = (double)ty * kDatumTileNodes / kDatumPerDeg;
}

bool DatumStore::get(Datum k, int tx, int ty, Tile& t) {
  std::lock_guard<std::recursive_mutex> lk(mx_);
  const Tile* c = cached(k, tx, ty);
  if (!c) return false;
  t = *c;
  return true;
}

bool DatumStore::has(Datum k, int tx, int ty) {
  std::lock_guard<std::recursive_mutex> lk(mx_);
  return cached(k, tx, ty) != nullptr;
}

const DatumStore::Tile* DatumStore::cached(Datum k, int tx, int ty) {
  const auto key = std::make_tuple((int)k, tx, ty);
  auto it = cache_.find(key);
  if (it != cache_.end()) return it->second.ok() ? &it->second : nullptr;
  Tile t;
  if (db_) {
    sqlite3_stmt* q = nullptr;
    sqlite3_prepare_v2(db_, "SELECT origin, model, data FROM surf WHERE kind=? AND tx=? AND ty=?", -1,
                       &q, nullptr);
    sqlite3_bind_int(q, 1, (int)k);
    sqlite3_bind_int(q, 2, tx);
    sqlite3_bind_int(q, 3, ty);
    if (q && sqlite3_step(q) == SQLITE_ROW) {
      const size_t N = (size_t)kDatumTileN * kDatumTileN;
      const void* b = sqlite3_column_blob(q, 2);
      if (b && (size_t)sqlite3_column_bytes(q, 2) == N * sizeof(float)) {
        t.origin = sqlite3_column_int(q, 0);
        const unsigned char* m = sqlite3_column_text(q, 1);
        t.model = m ? (const char*)m : "";
        t.v.resize(N);
        std::memcpy(t.v.data(), b, N * sizeof(float));
      }
    }
    sqlite3_finalize(q);
  }
  if (cache_.size() > 4096) cache_.clear();
  auto& slot = cache_[key] = std::move(t);
  return slot.ok() ? &slot : nullptr;
}

bool DatumStore::put(Datum k, int tx, int ty, const Tile& t, std::string& err) {
  std::lock_guard<std::recursive_mutex> lk(mx_);
  if (!t.ok()) { err = pd::tr("Surface de référence incomplète"); return false; }
  if (!ensureFile(err)) return false;
  sqlite3_stmt* q = nullptr;
  sqlite3_prepare_v2(db_, "INSERT OR REPLACE INTO surf VALUES(?,?,?,?,?,?)", -1, &q, nullptr);
  sqlite3_bind_int(q, 1, (int)k);
  sqlite3_bind_int(q, 2, tx);
  sqlite3_bind_int(q, 3, ty);
  sqlite3_bind_int(q, 4, t.origin);
  sqlite3_bind_text(q, 5, t.model.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_blob(q, 6, t.v.data(), (int)(t.v.size() * sizeof(float)), SQLITE_TRANSIENT);
  const bool ok = q && sqlite3_step(q) == SQLITE_DONE;
  sqlite3_finalize(q);
  if (!ok) { err = std::string(pd::tr("Base : ")) + sqlite3_errmsg(db_) + " (" + path_ + ")"; return false; }
  cache_[std::make_tuple((int)k, tx, ty)] = t;
  dirty_ = true;
  if (!inTx_) bumpRev();
  return true;
}

bool DatumStore::begin(std::string& err) {
  std::lock_guard<std::recursive_mutex> lk(mx_);
  if (inTx_) return true;
  if (!ensureFile(err)) return false;
  if (sqlite3_exec(db_, "BEGIN IMMEDIATE", nullptr, nullptr, nullptr) != SQLITE_OK) {
    err = std::string(pd::tr("Base : ")) + sqlite3_errmsg(db_);
    return false;
  }
  inTx_ = true;
  return true;
}

bool DatumStore::commit(std::string& err) {
  std::lock_guard<std::recursive_mutex> lk(mx_);
  if (!inTx_) return true;
  if (dirty_) bumpRev();  // dans la transaction
  inTx_ = false;
  if (sqlite3_exec(db_, "COMMIT", nullptr, nullptr, nullptr) != SQLITE_OK) {
    err = std::string(pd::tr("Base : ")) + sqlite3_errmsg(db_);
    return false;
  }
  return true;
}

void DatumStore::rollback() {
  std::lock_guard<std::recursive_mutex> lk(mx_);
  if (!inTx_) return;
  inTx_ = false;
  dirty_ = false;
  sqlite3_exec(db_, "ROLLBACK", nullptr, nullptr, nullptr);
  cache_.clear();
}

double DatumStore::at(Datum k, double lon, double lat, int* origin) {
  std::lock_guard<std::recursive_mutex> lk(mx_);
  if (lon >= 180.0) lon -= 360.0;
  if (lon < -180.0) lon += 360.0;
  int tx, ty;
  TileOf(lon, lat, tx, ty);
  const Tile* t = cached(k, tx, ty);
  if (!t) return NAN;
  if (origin) *origin = t->origin;
  double lonW, latS;
  TileBox(tx, ty, lonW, latS);
  const double fx = std::min<double>(kDatumTileNodes, std::max(0.0, (lon - lonW) * kDatumPerDeg));
  const double fy = std::min<double>(kDatumTileNodes, std::max(0.0, (lat - latS) * kDatumPerDeg));
  const int i = std::min(kDatumTileNodes - 1, (int)fx), j = std::min(kDatumTileNodes - 1, (int)fy);
  const double ax = fx - i, ay = fy - j;
  double s = 0, w = 0;
  for (int dj = 0; dj <= 1; ++dj)
    for (int di = 0; di <= 1; ++di) {
      const float v = t->v[(size_t)(j + dj) * kDatumTileN + i + di];
      if (std::isnan(v)) continue;
      const double ww = (di ? ax : 1 - ax) * (dj ? ay : 1 - ay);
      s += ww * v;
      w += ww;
    }
  return w > 1e-9 ? s / w : NAN;
}

std::vector<std::pair<int, int>> DatumStore::tiles(Datum k) {
  std::lock_guard<std::recursive_mutex> lk(mx_);
  std::vector<std::pair<int, int>> v;
  if (!db_) return v;
  sqlite3_stmt* q = nullptr;
  sqlite3_prepare_v2(db_, "SELECT tx, ty FROM surf WHERE kind=?", -1, &q, nullptr);
  sqlite3_bind_int(q, 1, (int)k);
  while (q && sqlite3_step(q) == SQLITE_ROW) v.push_back({sqlite3_column_int(q, 0), sqlite3_column_int(q, 1)});
  sqlite3_finalize(q);
  return v;
}

}  // namespace pd

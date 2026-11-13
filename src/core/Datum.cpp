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
#include <cmath>
#include <cstring>
#include <filesystem>

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

std::string DatumOriginText(Datum d, int mask, const std::string& mssModel) {
  std::vector<std::string> parts;
  if (mask & kOrigBathyElli) parts.push_back(d == Datum::Cd ? pd::tr("ZH BathyElli v2.1") : "BathyElli v2.1");
  if (mask & kOrigFes) parts.push_back(d == Datum::Cd ? pd::tr("LAT FES2022b") : "FES2022b");
  if (mask & kOrigMss) parts.push_back(mssModel.empty() ? std::string("MSS") : mssModel);
  if (mask & kOrigNone) parts.push_back(pd::tr("non défini"));
  std::string s;
  for (size_t k = 0; k < parts.size(); ++k) s += (k ? " + " : "") + parts[k];
  return s;
}

// ================================================================ calcul
struct DatumProvider::Impl {
  TideAtlas atlas;
  bool atlasTried = false, atlasOk = false;
  GeoGridFile mss;
  bool mssTried = false, mssOk = false;
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
                   "CREATE TABLE IF NOT EXISTS surf(kind INTEGER, tx INTEGER, ty INTEGER,"
                   " origin INTEGER, model TEXT, data BLOB, PRIMARY KEY(kind, tx, ty));",
                   nullptr, nullptr, &msg) != SQLITE_OK) {
    err = std::string(pd::tr("Base : ")) + (msg ? msg : "") + " (" + path_ + ")";
    sqlite3_free(msg);
    return false;
  }
  return true;
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

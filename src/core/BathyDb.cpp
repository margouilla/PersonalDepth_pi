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

#include "BathyDb.h"
#include "SurveyShape.h"
#include "Triangulate.h"
#include "Lang.h"
#include "FileUtil.h"

#include <sys/stat.h>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <random>
#include <thread>
#include <tuple>
#include <unordered_map>

#include "sqlite3.h"
#include "WebMerc.h"

namespace pd {
namespace fs = std::filesystem;
using Key = std::tuple<int, int, int>;  // z, x, y

// ================================================================ fichier
class DbCell {
 public:
  ~DbCell() {
    for (sqlite3_stmt* s : {getSrc, putSrc, getMerged, putMerged, delMerged})
      if (s) sqlite3_finalize(s);
    if (db) {
      // opération interrompue (arrêt, erreur) : rien de partiel n'est gardé
      if (inTx) sqlite3_exec(db, "ROLLBACK", nullptr, nullptr, nullptr);
      // journal vidé si personne d'autre ne lit le fichier ; sinon on n'attend
      // pas (le journal sera repris à la prochaine fermeture)
      sqlite3_busy_timeout(db, 0);
      sqlite3_wal_checkpoint_v2(db, nullptr, SQLITE_CHECKPOINT_TRUNCATE, nullptr, nullptr);
      sqlite3_close(db);
    }
  }
  bool open(const std::string& p, bool create, std::string& err, double cellDeg = 0) {
    path = p;
    const int flags = SQLITE_OPEN_READWRITE | (create ? SQLITE_OPEN_CREATE : 0) | SQLITE_OPEN_NOMUTEX;
    if (sqlite3_open_v2(fs::u8path(p).u8string().c_str(), &db, flags, nullptr) != SQLITE_OK) {
      err = pd::tr("Base : ouverture impossible de ") + p + (db ? std::string(" (") + sqlite3_errmsg(db) + ")" : "");
      if (db) { sqlite3_close(db); db = nullptr; }
      return false;
    }
    sqlite3_busy_timeout(db, 15000);
    const char* schema =
        "PRAGMA journal_mode=WAL;"
        "PRAGMA synchronous=NORMAL;"
        "CREATE TABLE IF NOT EXISTS meta(key TEXT PRIMARY KEY, value TEXT);"
        "CREATE TABLE IF NOT EXISTS sources(id INTEGER PRIMARY KEY, name TEXT, path TEXT, date TEXT,"
        " level INTEGER, res REAL, rank INTEGER, visible INTEGER, crs TEXT, vref TEXT, zmin REAL,"
        " zmax REAL, count INTEGER, lonmin REAL, lonmax REAL, latmin REAL, latmax REAL, imported TEXT,"
        " grp TEXT, params TEXT, info TEXT);"
        "CREATE TABLE IF NOT EXISTS src_tiles(src INTEGER, z INTEGER, x INTEGER, y INTEGER,"
        " data BLOB, PRIMARY KEY(src, z, x, y));"
        "CREATE TABLE IF NOT EXISTS merged(z INTEGER, x INTEGER, y INTEGER, zmin REAL, zmax REAL,"
        " data BLOB, PRIMARY KEY(z, x, y));"
        "INSERT OR IGNORE INTO meta VALUES('format', 'PersonalDepth 1');";
    if (!exec(schema, err)) return false;
    if (create && cellDeg > 0) {  // taille des cellules du découpage (lue à l'ouverture)
      char m[96];
      std::snprintf(m, sizeof m, "INSERT OR IGNORE INTO meta VALUES('cell_deg', '%.6g');", cellDeg);
      if (!exec(m, err)) return false;
    }
    // bases plus anciennes : colonne « classe » ajoutée
    bool hasGrp = false, hasParams = false, hasInfo = false;
    sqlite3_stmt* q = nullptr;
    sqlite3_prepare_v2(db, "PRAGMA table_info(sources)", -1, &q, nullptr);
    while (q && sqlite3_step(q) == SQLITE_ROW) {
      const unsigned char* n = sqlite3_column_text(q, 1);
      if (n && std::strcmp((const char*)n, "grp") == 0) hasGrp = true;
      if (n && std::strcmp((const char*)n, "params") == 0) hasParams = true;
      if (n && std::strcmp((const char*)n, "info") == 0) hasInfo = true;
    }
    sqlite3_finalize(q);
    // bases plus anciennes : paramètres d'import de chaque source (relecture)
    return (hasGrp || exec("ALTER TABLE sources ADD COLUMN grp TEXT", err)) &&
           (hasParams || exec("ALTER TABLE sources ADD COLUMN params TEXT", err)) &&
           (hasInfo || exec("ALTER TABLE sources ADD COLUMN info TEXT", err));
  }
  bool exec(const char* sql, std::string& err) {
    char* msg = nullptr;
    if (sqlite3_exec(db, sql, nullptr, nullptr, &msg) != SQLITE_OK) {
      err = std::string(pd::tr("Base : ")) + (msg ? msg : pd::tr("erreur SQL")) + " (" + path + ")";
      sqlite3_free(msg);
      return false;
    }
    return true;
  }
  sqlite3_stmt* prep(sqlite3_stmt*& s, const char* sql) {
    if (!s) sqlite3_prepare_v2(db, sql, -1, &s, nullptr);
    else sqlite3_reset(s);
    return s;
  }
  bool begin(std::string& err) {
    if (inTx) return true;
    if (!exec("BEGIN IMMEDIATE", err)) return false;
    inTx = true;
    return true;
  }
  void rollback() {
    if (!inTx) return;
    inTx = false;
    sqlite3_exec(db, "ROLLBACK", nullptr, nullptr, nullptr);
  }
  bool commit(std::string& err) {
    if (!inTx) return true;
    inTx = false;
    return exec("COMMIT", err);
  }
  bool getBlob(sqlite3_stmt* s, DepthTile& t) {
    if (sqlite3_step(s) != SQLITE_ROW) { sqlite3_reset(s); return false; }
    const void* b = sqlite3_column_blob(s, 0);
    const int n = sqlite3_column_bytes(s, 0);
    std::string err;
    const bool ok = b && DecodeTile((const uint8_t*)b, (size_t)n, t, err);
    sqlite3_reset(s);  // fin de la lecture : pas de transaction de lecture laissée ouverte
    return ok;
  }

  sqlite3* db = nullptr;
  std::string path;
  bool inTx = false;
  sqlite3_stmt *getSrc = nullptr, *putSrc = nullptr, *getMerged = nullptr, *putMerged = nullptr,
               *delMerged = nullptr;
};

// ================================================================ outils
// Fichier d'une cellule : « PD_N48W005.pdb » (1°, ancien découpage) ou
// « PD_N4815W00430.pdb » (coin sud-ouest en degrés et minutes, cellules < 1°)
std::string CellFileName(double lon, double lat, double deg) {
  const double la = std::floor(lat / deg + 1e-9) * deg, lo = std::floor(lon / deg + 1e-9) * deg;
  char b[40];
  if (deg >= 1.0 - 1e-9) {
    std::snprintf(b, sizeof b, "PD_%c%02d%c%03d.pdb", la < 0 ? 'S' : 'N', (int)std::lround(std::fabs(la)),
                  lo < 0 ? 'W' : 'E', (int)std::lround(std::fabs(lo)));
    return b;
  }
  auto dm = [](double v, int& d, int& m) {
    const long t = std::lround(std::fabs(v) * 60.0);  // minutes entières
    d = (int)(t / 60); m = (int)(t % 60);
  };
  int ad, am, od, om;
  dm(la, ad, am);
  dm(lo, od, om);
  std::snprintf(b, sizeof b, "PD_%c%02d%02d%c%03d%02d.pdb", la < 0 ? 'S' : 'N', ad, am,
                lo < 0 ? 'W' : 'E', od, om);
  return b;
}

bool ParseCellFileName(const std::string& n, double& lonW, double& latS, bool& legacy) {
  char ns, ew;
  int a, b, c, d;
  char tail[8];
  if (n.size() == 14 && std::sscanf(n.c_str(), "PD_%c%2d%c%3d%4s", &ns, &a, &ew, &b, tail) == 5) {
    legacy = true;
    latS = (ns == 'S' ? -1 : 1) * a;
    lonW = (ew == 'W' ? -1 : 1) * b;
  } else if (n.size() == 18 &&
             std::sscanf(n.c_str(), "PD_%c%2d%2d%c%3d%2d%4s", &ns, &a, &b, &ew, &c, &d, tail) == 7) {
    legacy = false;
    latS = (ns == 'S' ? -1 : 1) * (a + b / 60.0);
    lonW = (ew == 'W' ? -1 : 1) * (c + d / 60.0);
  } else {
    return false;
  }
  return (ns == 'N' || ns == 'S') && (ew == 'E' || ew == 'W') && std::string(tail) == ".pdb";
}

static std::string Today() {
  std::time_t t = std::time(nullptr);
  char b[32];
  std::strftime(b, sizeof b, "%Y-%m-%d", std::localtime(&t));
  return b;
}

std::string GuessSurveyDate(const std::string& path) {
  const std::string d = SurveyDateFromName(path);
  if (!d.empty()) return d;
  if (const long long mt = FileMTime(path)) {
    std::time_t t = (std::time_t)mt;
    char b[32];
    std::strftime(b, sizeof b, "%Y-%m-%d", std::localtime(&t));
    return b;
  }
  return Today();
}

std::string SurveyDateFromName(const std::string& path) {
  const std::string name = fs::u8path(path).filename().u8string();
  for (size_t k = 0; k + 8 <= name.size(); ++k) {
    bool digits = true;
    for (size_t q = 0; q < 8; ++q) digits = digits && std::isdigit((unsigned char)name[k + q]);
    if (!digits || (k > 0 && std::isdigit((unsigned char)name[k - 1])) ||
        (k + 8 < name.size() && std::isdigit((unsigned char)name[k + 8])))
      continue;
    const int y = std::stoi(name.substr(k, 4)), m = std::stoi(name.substr(k + 4, 2)),
              d = std::stoi(name.substr(k + 6, 2));
    if (y >= 1900 && y <= 2100 && m >= 1 && m <= 12 && d >= 1 && d <= 31) {
      char b[16];
      std::snprintf(b, sizeof b, "%04d-%02d-%02d", y, m, d);
      return b;
    }
  }
  return std::string();
}

std::string GuessSourceGroup(const std::string& path) {
  std::string name = fs::u8path(path).stem().u8string(), tok;
  auto take = [&]() -> bool {
    int letters = 0;
    for (char c : tok) letters += std::isalpha((unsigned char)c) != 0;
    return letters >= 2;
  };
  for (size_t k = 0; k <= name.size(); ++k) {
    const char c = k < name.size() ? name[k] : ' ';
    if (c == '_' || c == '-' || c == ' ' || c == '.') {
      if (take()) {
        for (char& q : tok) q = (char)std::toupper((unsigned char)q);
        return tok;
      }
      tok.clear();
    } else {
      tok += c;
    }
  }
  return "Divers";
}

double EstimateResolution(const PointCloud& pc, bool& gridded) {
  gridded = false;
  if (pc.empty()) return 0;
  const SurveyShape sh = AnalyzeSurvey(pc);
  gridded = sh.isGrid();
  return sh.storeRes;
}

// Ordre de priorité des sources pour une règle donnée (la première l'emporte)
static bool Before(const DbSource& a, const DbSource& b, DbPriority p) {
  auto res = [](const DbSource& s, const DbSource& t) -> int {
    if (s.level != t.level) return s.level > t.level ? -1 : 1;
    if (std::fabs(s.res - t.res) > 1e-6) return s.res < t.res ? -1 : 1;
    return 0;
  };
  auto date = [](const DbSource& s, const DbSource& t) -> int {
    if (s.date != t.date) return s.date > t.date ? -1 : 1;
    return 0;
  };
  int c = 0;
  switch (p) {
    case DbPriority::Newest: c = date(a, b); if (!c) c = res(a, b); break;
    case DbPriority::Manual: c = a.rank != b.rank ? (a.rank < b.rank ? -1 : 1) : res(a, b); break;
    default: c = res(a, b); if (!c) c = date(a, b); break;
  }
  if (!c) c = a.rank != b.rank ? (a.rank < b.rank ? -1 : 1) : 0;
  if (!c) c = a.id < b.id ? -1 : (a.id > b.id ? 1 : 0);
  return c < 0;
}

// ================================================================ base
BathyDb::BathyDb() = default;
BathyDb::~BathyDb() = default;

bool BathyDb::open(const std::string& folder, std::string& err, double newDeg) {
  cells_.clear();
  sources_.clear();
  folder_ = folder;
  std::error_code ec;
  fs::create_directories(fs::u8path(folder), ec);
  if (!fs::is_directory(fs::u8path(folder), ec)) {
    err = pd::tr("Dossier de base inaccessible : ") + folder;
    return false;
  }
  // découpage du dossier : 1° pour les anciennes bases, sinon celui inscrit
  // dans les fichiers ; `newDeg` pour une base vide
  cellDeg_ = newDeg > 0 ? newDeg : kDefaultCellDeg;
  bool legacy = false;
  std::string first;
  for (auto& e : fs::directory_iterator(fs::u8path(folder), ec)) {
    const std::string n = e.path().filename().u8string();
    double lw, ls;
    bool lg;
    if (!ParseCellFileName(n, lw, ls, lg)) continue;
    if (lg) legacy = true;
    else if (first.empty()) first = n;
  }
  if (legacy) {
    cellDeg_ = 1.0;
  } else if (!first.empty()) {
    if (DbCell* c = cellByName(first, false, err)) {
      sqlite3_stmt* q = nullptr;
      sqlite3_prepare_v2(c->db, "SELECT value FROM meta WHERE key='cell_deg'", -1, &q, nullptr);
      if (q && sqlite3_step(q) == SQLITE_ROW) {
        const double d = sqlite3_column_double(q, 0);
        if (d > 0.01 && d <= 1.0) cellDeg_ = d;
      }
      sqlite3_finalize(q);
    }
  }
  return reloadSources(err);
}

std::vector<DbCellInfo> BathyDb::cellInfo() {
  std::string err;
  std::vector<DbCellInfo> out;
  for (DbCell* c : allCells(err)) {
    DbCellInfo i;
    i.name = fs::u8path(c->path).filename().u8string();
    bool lg;
    if (!ParseCellFileName(i.name, i.lonW, i.latS, lg)) continue;
    i.deg = lg ? 1.0 : cellDeg_;
    std::error_code ec;
    i.bytes = (long long)fs::file_size(fs::u8path(c->path), ec);
    const auto wal = fs::file_size(fs::u8path(c->path + "-wal"), ec);
    if (!ec) i.bytes += (long long)wal;
    sqlite3_stmt* q = nullptr;
    sqlite3_prepare_v2(c->db, "SELECT COUNT(*) FROM sources", -1, &q, nullptr);
    if (q && sqlite3_step(q) == SQLITE_ROW) i.sources = sqlite3_column_int(q, 0);
    sqlite3_finalize(q);
    out.push_back(i);
  }
  std::sort(out.begin(), out.end(), [](const DbCellInfo& a, const DbCellInfo& b) {
    return a.latS != b.latS ? a.latS > b.latS : a.lonW < b.lonW;  // nord -> sud, ouest -> est
  });
  return out;
}

void BathyDb::setHiddenCells(const std::set<std::string>& names) {
  hidden_.clear();
  for (const auto& n : names) {
    double lw, ls;
    bool lg;
    if (!ParseCellFileName(n, lw, ls, lg)) continue;
    const double d = lg ? 1.0 : cellDeg_;
    hidden_.insert({(int)std::floor(lw / d + 1e-6), (int)std::floor(ls / d + 1e-6)});
  }
}

bool BathyDb::hiddenAt(double lon, double lat) const {
  if (hidden_.empty()) return false;
  return hidden_.count({(int)std::floor(lon / cellDeg_ + 1e-9), (int)std::floor(lat / cellDeg_ + 1e-9)}) > 0;
}

DbCell* BathyDb::cellByName(const std::string& name, bool create, std::string& err) {
  auto it = cells_.find(name);
  if (it != cells_.end()) return it->second.get();
  const fs::path p = fs::u8path(folder_) / fs::u8path(name);
  std::error_code ec;
  if (!create && !fs::exists(p, ec)) return nullptr;
  auto c = std::make_unique<DbCell>();
  if (!c->open(p.u8string(), create, err, cellDeg_)) return nullptr;
  DbCell* raw = c.get();
  cells_[name] = std::move(c);
  return raw;
}

DbCell* BathyDb::cellFor(int z, int x, int y, bool create, std::string& err) {
  double lon, lat;
  PixelToLonLat(z, (x + 0.5) * kTileSize, (y + 0.5) * kTileSize, lon, lat);
  return cellByName(CellFileName(lon, lat, cellDeg_), create, err);
}

std::vector<DbCell*> BathyDb::allCells(std::string& err) {
  std::error_code ec;
  for (auto& e : fs::directory_iterator(fs::u8path(folder_), ec)) {
    const std::string n = e.path().filename().u8string();
    double lw, ls;
    bool lg;
    if (ParseCellFileName(n, lw, ls, lg)) cellByName(n, false, err);
  }
  std::vector<DbCell*> v;
  for (auto& kv : cells_) v.push_back(kv.second.get());
  return v;
}

bool BathyDb::reloadSources(std::string& err) {
  std::map<int64_t, DbSource> m;
  for (DbCell* c : allCells(err)) {
    sqlite3_stmt* s = nullptr;
    if (sqlite3_prepare_v2(c->db,
                           "SELECT id,name,path,date,level,res,rank,visible,crs,vref,zmin,zmax,count,"
                           "lonmin,lonmax,latmin,latmax,imported,grp,params,info FROM sources",
                           -1, &s, nullptr) != SQLITE_OK)
      continue;
    while (sqlite3_step(s) == SQLITE_ROW) {
      DbSource d;
      auto txt = [&](int i) {
        const unsigned char* t = sqlite3_column_text(s, i);
        return t ? std::string((const char*)t) : std::string();
      };
      d.id = sqlite3_column_int64(s, 0);
      d.name = txt(1); d.path = txt(2); d.date = txt(3);
      d.level = sqlite3_column_int(s, 4);
      d.res = sqlite3_column_double(s, 5);
      d.rank = sqlite3_column_int(s, 6);
      d.visible = sqlite3_column_int(s, 7) != 0;
      d.crs = txt(8); d.vref = txt(9);
      d.zmin = (float)sqlite3_column_double(s, 10);
      d.zmax = (float)sqlite3_column_double(s, 11);
      d.count = sqlite3_column_int64(s, 12);
      d.lonMin = sqlite3_column_double(s, 13); d.lonMax = sqlite3_column_double(s, 14);
      d.latMin = sqlite3_column_double(s, 15); d.latMax = sqlite3_column_double(s, 16);
      d.imported = txt(17);
      d.group = txt(18);
      d.params = txt(19);
      d.info = txt(20);
      if (d.group.empty()) d.group = GuessSourceGroup(d.name);  // bases antérieures aux classes
      m[d.id] = d;  // même ligne dans chaque fichier touché
    }
    sqlite3_finalize(s);
  }
  sources_.clear();
  for (auto& kv : m) sources_.push_back(kv.second);
  std::sort(sources_.begin(), sources_.end(),
            [](const DbSource& a, const DbSource& b) { return a.rank < b.rank; });
  return true;
}

bool BathyDb::needsRepair() const {
  for (const DbSource& d : sources_)
    if (!DepthTile::plausible(d.zmin * 1000.0) || !DepthTile::plausible(d.zmax * 1000.0)) return true;
  return false;
}

bool BathyDb::repairStats(std::string& err) {
  std::vector<DbCell*> cells = allCells(err);
  // Bases créées avant le filtre des valeurs de remplissage : statistiques
  // recalculées sur les tuiles natives (les valeurs aberrantes sont ignorées
  // à la lecture), une fois pour toutes.
  for (DbSource& d : sources_) {
    if (DepthTile::plausible(d.zmin * 1000.0) && DepthTile::plausible(d.zmax * 1000.0)) continue;
    long long count = 0;
    int32_t lo = INT32_MAX, hi = INT32_MIN;
    for (DbCell* c : cells) {
      sqlite3_stmt* q = nullptr;
      if (sqlite3_prepare_v2(c->db, "SELECT data FROM src_tiles WHERE src=? AND z=?", -1, &q,
                             nullptr) != SQLITE_OK)
        continue;
      sqlite3_bind_int64(q, 1, d.id);
      sqlite3_bind_int(q, 2, d.level);
      DepthTile t;
      std::string e;
      while (sqlite3_step(q) == SQLITE_ROW) {
        const uint8_t* b = (const uint8_t*)sqlite3_column_blob(q, 0);
        if (!b || !DecodeTile(b, (size_t)sqlite3_column_bytes(q, 0), t, e)) continue;
        for (int32_t v : t.v)
          if (v != DepthTile::kEmpty) { ++count; lo = std::min(lo, v); hi = std::max(hi, v); }
      }
      sqlite3_finalize(q);
    }
    if (!count) continue;
    d.count = count; d.zmin = lo / 1000.f; d.zmax = hi / 1000.f;
    for (DbCell* c : cells) {
      sqlite3_stmt* q = nullptr;
      if (sqlite3_prepare_v2(c->db, "UPDATE sources SET zmin=?, zmax=?, count=? WHERE id=?", -1, &q,
                             nullptr) != SQLITE_OK)
        continue;
      sqlite3_bind_double(q, 1, d.zmin);
      sqlite3_bind_double(q, 2, d.zmax);
      sqlite3_bind_int64(q, 3, d.count);
      sqlite3_bind_int64(q, 4, d.id);
      sqlite3_step(q);
      sqlite3_finalize(q);
    }
  }
  return true;
}

const DbSource* BathyDb::source(int64_t id) const {
  for (const auto& s : sources_)
    if (s.id == id) return &s;
  return nullptr;
}

bool BathyDb::readSrc(int64_t id, int z, int x, int y, DepthTile& t) {
  if (pendingSrc_ && !flushWrites(flushErr_)) return false;  // lecture après écriture
  std::string err;
  DbCell* c = cellFor(z, x, y, false, err);
  if (!c) return false;
  sqlite3_stmt* s = c->prep(c->getSrc, "SELECT data FROM src_tiles WHERE src=? AND z=? AND x=? AND y=?");
  sqlite3_bind_int64(s, 1, id);
  sqlite3_bind_int(s, 2, z); sqlite3_bind_int(s, 3, x); sqlite3_bind_int(s, 4, y);
  return c->getBlob(s, t);
}

// Écritures de tuiles regroupées : l'encodage (prédiction + compression) est
// réparti sur les cœurs, l'écriture SQLite reste séquentielle
bool BathyDb::writeSrc(int64_t id, int z, int x, int y, const DepthTile& t, std::string& err) {
  if (t.count() == 0) return true;  // tuile vide : rien à écrire
  pending_.push_back({false, id, z, x, y, t});
  ++pendingSrc_;
  return pending_.size() < kWriteBatch || flushWrites(err);
}

bool BathyDb::writeMerged(int z, int x, int y, DepthTile&& t, std::string& err) {
  pending_.push_back({true, 0, z, x, y, std::move(t)});
  ++pendingMerged_;
  return pending_.size() < kWriteBatch || flushWrites(err);
}

bool BathyDb::flushWrites(std::string& err) {
  if (pending_.empty()) return true;
  const size_t n = pending_.size();
  std::vector<std::vector<uint8_t>> blobs(n);
  std::vector<int32_t> lo(n, INT32_MAX), hi(n, INT32_MIN);
  std::atomic<size_t> next(0);
  auto work = [&]() {
    for (size_t k; (k = next++) < n;) {
      EncodeTile(pending_[k].t, blobs[k]);
      if (pending_[k].merged)
        for (int32_t v : pending_[k].t.v)
          if (v != DepthTile::kEmpty) { lo[k] = std::min(lo[k], v); hi[k] = std::max(hi[k], v); }
    }
  };
  const unsigned hw = std::max(1u, std::min(8u, std::thread::hardware_concurrency()));
  std::vector<std::thread> th;
  for (unsigned w = 1; w < hw && w < n; ++w) th.emplace_back(work);
  work();
  for (auto& t : th) t.join();
  bool ok = true;
  for (size_t k = 0; k < n && ok; ++k) {
    const PendingWrite& w = pending_[k];
    if (blobs[k].empty()) continue;
    DbCell* c = cellFor(w.z, w.x, w.y, true, err);
    if (!c || !c->begin(err)) { ok = false; break; }
    sqlite3_stmt* s;
    if (w.merged) {
      s = c->prep(c->putMerged, "INSERT OR REPLACE INTO merged VALUES(?,?,?,?,?,?)");
      sqlite3_bind_int(s, 1, w.z); sqlite3_bind_int(s, 2, w.x); sqlite3_bind_int(s, 3, w.y);
      sqlite3_bind_double(s, 4, lo[k] / 1000.0);
      sqlite3_bind_double(s, 5, hi[k] / 1000.0);
      sqlite3_bind_blob(s, 6, blobs[k].data(), (int)blobs[k].size(), SQLITE_STATIC);
    } else {
      s = c->prep(c->putSrc, "INSERT OR REPLACE INTO src_tiles VALUES(?,?,?,?,?)");
      sqlite3_bind_int64(s, 1, w.id);
      sqlite3_bind_int(s, 2, w.z); sqlite3_bind_int(s, 3, w.x); sqlite3_bind_int(s, 4, w.y);
      sqlite3_bind_blob(s, 5, blobs[k].data(), (int)blobs[k].size(), SQLITE_STATIC);
    }
    if (sqlite3_step(s) != SQLITE_DONE) { err = std::string(pd::tr("Base : ")) + sqlite3_errmsg(c->db); ok = false; }
    sqlite3_reset(s);
  }
  pending_.clear();
  pendingSrc_ = pendingMerged_ = 0;
  return ok;
}

bool BathyDb::readMerged(int z, int x, int y, DepthTile& t) {
  if (pendingMerged_ && !flushWrites(flushErr_)) return false;
  std::string err;
  DbCell* c = cellFor(z, x, y, false, err);
  if (!c) return false;
  sqlite3_stmt* s = c->prep(c->getMerged, "SELECT data FROM merged WHERE z=? AND x=? AND y=?");
  sqlite3_bind_int(s, 1, z); sqlite3_bind_int(s, 2, x); sqlite3_bind_int(s, 3, y);
  return c->getBlob(s, t);
}

bool BathyDb::upsertSource(const DbSource& d, std::string& err) {
  if (!flushWrites(err)) return false;
  for (auto& kv : cells_) {
    DbCell* c = kv.second.get();
    // fichiers où la source a des tuiles (ou déjà une ligne)
    sqlite3_stmt* q = nullptr;
    sqlite3_prepare_v2(c->db, "SELECT 1 FROM src_tiles WHERE src=? LIMIT 1", -1, &q, nullptr);
    sqlite3_bind_int64(q, 1, d.id);
    const bool has = sqlite3_step(q) == SQLITE_ROW;
    sqlite3_finalize(q);
    if (!has) continue;
    if (!c->begin(err)) return false;
    sqlite3_stmt* s = nullptr;
    sqlite3_prepare_v2(c->db,
                       "INSERT OR REPLACE INTO sources (id,name,path,date,level,res,rank,visible,crs,vref,zmin,zmax,"
                       "count,lonmin,lonmax,latmin,latmax,imported,grp,params,info) VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?)", -1,
                       &s, nullptr);
    int k = 1;
    sqlite3_bind_int64(s, k++, d.id);
    sqlite3_bind_text(s, k++, d.name.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(s, k++, d.path.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(s, k++, d.date.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(s, k++, d.level);
    sqlite3_bind_double(s, k++, d.res);
    sqlite3_bind_int(s, k++, d.rank);
    sqlite3_bind_int(s, k++, d.visible ? 1 : 0);
    sqlite3_bind_text(s, k++, d.crs.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(s, k++, d.vref.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_double(s, k++, d.zmin);
    sqlite3_bind_double(s, k++, d.zmax);
    sqlite3_bind_int64(s, k++, d.count);
    sqlite3_bind_double(s, k++, d.lonMin);
    sqlite3_bind_double(s, k++, d.lonMax);
    sqlite3_bind_double(s, k++, d.latMin);
    sqlite3_bind_double(s, k++, d.latMax);
    sqlite3_bind_text(s, k++, d.imported.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(s, k++, d.group.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(s, k++, d.params.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(s, k++, d.info.c_str(), -1, SQLITE_TRANSIENT);
    const int rc = sqlite3_step(s);
    sqlite3_finalize(s);
    if (rc != SQLITE_DONE) { err = std::string(pd::tr("Base : ")) + sqlite3_errmsg(c->db); return false; }
  }
  return true;
}

bool BathyDb::commitAll(std::string& err) {
  if (!flushWrites(err)) return false;
  if (!flushErr_.empty()) { err = flushErr_; flushErr_.clear(); return false; }
  bool ok = true;
  for (auto& kv : cells_) ok = kv.second->commit(err) && ok;
  return ok;
}

// ------------------------------------------------------------- fusion
bool BathyDb::remergeTile(int z, int x, int y, DbPriority prio, std::string& err) {
  double lonW, lonE, latS, latN;
  TileBounds(z, x, y, lonW, lonE, latS, latN);
  std::vector<const DbSource*> cand;
  for (const auto& s : sources_)
    if (s.visible && s.lonMax >= lonW && s.lonMin <= lonE && s.latMax >= latS && s.latMin <= latN)
      cand.push_back(&s);
  std::sort(cand.begin(), cand.end(),
            [prio](const DbSource* a, const DbSource* b) { return Before(*a, *b, prio); });
  DepthTile m;
  m.reset();
  std::vector<uint8_t> idx(DepthTile::kN, 0);
  std::vector<int64_t> ids;
  // tuiles des sources, par ordre de priorité
  std::vector<std::pair<const DbSource*, DepthTile>> got;
  bool anyInterp = false;
  for (const DbSource* s : cand) {
    DepthTile t;
    const int d = z - s->level;
    bool have;
    if (d <= 0) {
      have = readSrc(s->id, z, x, y, t);
    } else {  // source plus grossière : pixels répétés
      DepthTile p;
      have = d < 31 && readSrc(s->id, s->level, x >> d, y >> d, p);
      if (have) {
        t.reset();
        const long long gx0 = (long long)x * kTileSize, gy0 = (long long)y * kTileSize;
        const long long px0 = (long long)(x >> d) * kTileSize, py0 = (long long)(y >> d) * kTileSize;
        for (int j = 0; j < 256; ++j) {
          const long long sj = ((gy0 + j) >> d) - py0;
          for (int i = 0; i < 256; ++i) {
            const long long si = ((gx0 + i) >> d) - px0;
            t.v[j * 256 + i] = p.v[sj * 256 + si];
            if (p.isInterp((int)(sj * 256 + si))) t.setInterp(j * 256 + i, true);
          }
        }
      }
    }
    if (!have) continue;
    anyInterp = anyInterp || !t.interp.empty();
    got.emplace_back(s, std::move(t));
  }
  // passe 1 : sondes mesurées ; passe 2 : valeurs interpolées dans les vides
  std::vector<int> slotOf(got.size(), -1);
  for (int pass = 0; pass < (anyInterp ? 2 : 1); ++pass) {
    for (size_t g = 0; g < got.size(); ++g) {
      const DepthTile& t = got[g].second;
      for (int k = 0; k < DepthTile::kN; ++k) {
        const int32_t v = t.v[k];
        if (v == DepthTile::kEmpty || t.isInterp(k) != (pass == 1)) continue;
        bool take;
        if (pass == 0) take = m.v[k] == DepthTile::kEmpty || (prio == DbPriority::Shoalest && v < m.v[k]);
        else take = m.v[k] == DepthTile::kEmpty ||
                    (m.isInterp(k) && prio == DbPriority::Shoalest && v < m.v[k]);
        if (!take) continue;
        if (slotOf[g] < 0) {
          if (ids.size() >= 255) continue;
          slotOf[g] = (int)ids.size();
          ids.push_back(got[g].first->id);
        }
        m.v[k] = v;
        idx[k] = (uint8_t)slotOf[g];
        if (pass == 1) m.setInterp(k, true);
      }
    }
  }
  DbCell* c = cellFor(z, x, y, !ids.empty(), err);
  if (!c) return ids.empty();
  if (!c->begin(err)) return false;
  if (ids.empty()) {
    if (pendingMerged_ && !flushWrites(err)) return false;
    sqlite3_stmt* s = c->prep(c->delMerged, "DELETE FROM merged WHERE z=? AND x=? AND y=?");
    sqlite3_bind_int(s, 1, z); sqlite3_bind_int(s, 2, x); sqlite3_bind_int(s, 3, y);
    sqlite3_step(s);
    return true;
  }
  m.srcIds = ids;
  if (ids.size() > 1) m.src = idx;
  return writeMerged(z, x, y, std::move(m), err);
}

bool BathyDb::remergeSet(const std::set<Key>& keys, DbPriority prio, std::string& err,
                         const DbProgress& progress, const std::atomic<bool>* cancel) {
  size_t k = 0;
  for (const Key& key : keys) {
    if (cancel && *cancel) { err = "Annulé"; return false; }
    if (!remergeTile(std::get<0>(key), std::get<1>(key), std::get<2>(key), prio, err)) return false;
    if (progress && (++k % 64 == 0 || k == keys.size()))
      progress((float)k / keys.size(), pd::tr("Fusion des tuiles"));
  }
  return true;
}

// Tuiles fusionnées existantes plus fines que la source, dans son emprise
void BathyDb::affectedByBox(const DbSource& s, std::set<Key>& keys) {
  std::string err;
  for (DbCell* c : allCells(err)) {
    for (int z = s.level + 1; z <= kDbMaxLevel; ++z) {
      double x0, y0, x1, y1;
      LonLatToPixel(z, s.lonMin, s.latMax, x0, y0);
      LonLatToPixel(z, s.lonMax, s.latMin, x1, y1);
      sqlite3_stmt* q = nullptr;
      sqlite3_prepare_v2(c->db, "SELECT x,y FROM merged WHERE z=? AND x BETWEEN ? AND ? AND y BETWEEN ? AND ?",
                         -1, &q, nullptr);
      sqlite3_bind_int(q, 1, z);
      sqlite3_bind_int64(q, 2, (long long)std::floor(x0 / kTileSize));
      sqlite3_bind_int64(q, 3, (long long)std::floor(x1 / kTileSize));
      sqlite3_bind_int64(q, 4, (long long)std::floor(y0 / kTileSize));
      sqlite3_bind_int64(q, 5, (long long)std::floor(y1 / kTileSize));
      while (sqlite3_step(q) == SQLITE_ROW)
        keys.insert(Key(z, sqlite3_column_int(q, 0), sqlite3_column_int(q, 1)));
      sqlite3_finalize(q);
    }
  }
}

// ------------------------------------------------------------- import
void BathyDb::rollbackAll() {
  pending_.clear();
  pendingSrc_ = pendingMerged_ = 0;
  flushErr_.clear();
  for (auto& kv : cells_) kv.second->rollback();
  std::string e;
  reloadSources(e);
}

bool BathyDb::importCloud(const PointCloud& pc, DbSource meta, bool gridded, DbPriority prio,
                          std::string& err, const DbProgress& progress,
                          const std::atomic<bool>* cancel, int64_t* newId) {
  // échec ou arrêt : la source partiellement écrite est annulée
  if (pc.empty()) { err = pd::tr("Source vide"); return false; }
  if (importCloudImpl(&pc, nullptr, 0.5 * (pc.latMin + pc.latMax), meta, gridded, prio, err, progress,
                      cancel, newId))
    return true;
  rollbackAll();
  return false;
}

bool BathyDb::importCloudStream(const CloudFeed& feed, double midLat, DbSource meta, bool gridded,
                                DbPriority prio, std::string& err, const DbProgress& progress,
                                const std::atomic<bool>* cancel, int64_t* newId) {
  if (importCloudImpl(nullptr, &feed, midLat, meta, gridded, prio, err, progress, cancel, newId))
    return true;
  rollbackAll();
  return false;
}

bool BathyDb::importCloudImpl(const PointCloud* whole, const CloudFeed* feed, double midLat,
                              DbSource meta, bool gridded, DbPriority prio, std::string& err,
                              const DbProgress& progress, const std::atomic<bool>* cancel,
                              int64_t* newId) {
  const int zs = meta.level > 0 ? meta.level : LevelForResolution(meta.res, midLat);
  std::mt19937_64 rng((uint64_t)std::chrono::high_resolution_clock::now().time_since_epoch().count() ^
                      (uint64_t)std::random_device{}());
  meta.id = (int64_t)(rng() >> 2) + 1;
  meta.level = zs;
  if (meta.imported.empty()) meta.imported = Today();
  if (meta.date.empty()) meta.date = meta.imported;
  int maxRank = -1;
  for (const auto& s : sources_) maxRank = std::max(maxRank, s.rank);
  meta.rank = maxRank + 1;
  if (whole) {
    meta.lonMin = whole->lonMin; meta.lonMax = whole->lonMax;
    meta.latMin = whole->latMin; meta.latMax = whole->latMax;
  } else {  // par blocs : emprise cumulée
    meta.lonMin = meta.latMin = 1e9;
    meta.lonMax = meta.latMax = -1e9;
  }

  // Tuiles du niveau natif : en mémoire si la source est de taille raisonnable
  // (une seule écriture par tuile), sinon écrites au fil de l'eau
  using TK = std::pair<int, int>;
  bool inMemory = false;  // par blocs : toujours au fil de l'eau
  if (whole) {
    double gx0, gy0, gx1, gy1;
    LonLatToPixel(zs, whole->lonMin, whole->latMax, gx0, gy0);
    LonLatToPixel(zs, whole->lonMax, whole->latMin, gx1, gy1);
    const double tileBound = (std::floor(gx1 / 256) - std::floor(gx0 / 256) + 1) *
                             (std::floor(gy1 / 256) - std::floor(gy0 / 256) + 1);
    inMemory = tileBound <= 700;  // ~180 Mo au plus
  }
  std::map<TK, DepthTile> mem;             // tuiles en mémoire (toutes si inMemory)
  std::map<TK, uint64_t> used;
  std::set<TK> keysZs;                     // tuiles déjà écrites (mode flux)
  uint64_t clock = 0;
  // Écrit des tuiles sorties de la mémoire, fusionnées avec leur version déjà
  // écrite : toutes les relectures d'abord, puis toutes les écritures (une
  // relecture force l'écriture de ce qui attend : une par une, l'écriture
  // groupée et la compression sur plusieurs cœurs étaient perdues).
  auto flushSet = [&](const std::vector<TK>& keys) -> bool {
    for (const TK& key : keys) {
      if (!keysZs.count(key)) continue;
      DepthTile& t = mem[key];
      DepthTile old;
      if (readSrc(meta.id, zs, key.first, key.second, old))
        for (int k = 0; k < DepthTile::kN; ++k)
          if (old.v[k] != DepthTile::kEmpty && (t.v[k] == DepthTile::kEmpty || old.v[k] < t.v[k]))
            t.v[k] = old.v[k];
    }
    bool ok = true;
    for (const TK& key : keys) {
      keysZs.insert(key);
      ok = ok && writeSrc(meta.id, zs, key.first, key.second, mem[key], err);
      mem.erase(key);
      used.erase(key);
    }
    return ok;
  };

  // 1) répartition des sondes par pixel : plus faible sonde du pixel
  size_t rejected = 0;
  DepthTile* curTile = nullptr;
  TK curKey(0, 0);
  float fed0 = 0.f;  // part déjà lue (par blocs)
  CloudSink bin = [&](const PointCloud& pc, float fed) -> bool {
  if (!whole && !pc.empty()) {
    meta.lonMin = std::min(meta.lonMin, pc.lonMin); meta.lonMax = std::max(meta.lonMax, pc.lonMax);
    meta.latMin = std::min(meta.latMin, pc.latMin); meta.latMax = std::max(meta.latMax, pc.latMax);
  }
  const size_t n = pc.size();
  // dépôt d'une sonde (pixel ix, iy du niveau zs, valeur en mm) : la plus faible
  auto put = [&](long long ix, long long iy, int32_t v) -> bool {
    const TK key((int)(ix >> 8), (int)(iy >> 8));
    if (!curTile || key != curKey) {  // points voisins : le plus souvent la même tuile
      DepthTile& t = mem[key];
      if (!t.allocated()) t.reset();
      if (!inMemory) used[key] = ++clock;
      curTile = &t;
      curKey = key;
    }
    int32_t& c = curTile->v[(size_t)(iy & 255) * 256 + (size_t)(ix & 255)];
    if (c == DepthTile::kEmpty || v < c) c = v;
    if (!inMemory && mem.size() > 320) {  // écrit les 64 tuiles les moins récemment touchées
      std::vector<std::pair<uint64_t, TK>> age;
      for (auto& kv : used) age.push_back({kv.second, kv.first});
      std::nth_element(age.begin(), age.begin() + 64, age.end());
      std::vector<TK> out;
      for (int q = 0; q < 64; ++q) out.push_back(age[q].second);
      if (!flushSet(out)) return false;
      curTile = nullptr;
    }
    return true;
  };
  auto step = [&](size_t k) -> bool {
    if (cancel && *cancel) { err = "Annulé"; return false; }
    if (progress) progress(0.4f * (fed0 + (fed - fed0) * k / n), pd::tr("Répartition des sondes"));
    return true;
  };
  if (inMemory) {  // toutes les tuiles en mémoire : ordre du fichier
    for (size_t k = 0; k < n; ++k) {
      const double mm = std::round((double)pc.z[k] * 1000.0);
      if (!DepthTile::plausible(mm)) { ++rejected; continue; }  // remplissage, NaN
      double gx, gy;
      LonLatToPixel(zs, pc.lon(k), pc.lat(k), gx, gy);
      if (!put((long long)std::floor(gx), (long long)std::floor(gy), (int32_t)mm)) return false;
      if ((k & 0xFFFFF) == 0 && !step(k)) return false;
    }
  } else {
    // Mode flux : sondes triées par tuile, par paquets de 4 M (64 Mo), pour que
    // chaque tuile ne soit visitée qu'une fois par paquet. Sinon une route de
    // sondes repasse sans cesse par des tuiles déjà écrites sur disque (relues,
    // fusionnées, réécrites) : 16 s au lieu de 7 pour 10 M sondes en lignes.
    struct SP { uint64_t key; uint32_t pix; int32_t v; };
    std::vector<SP> sp;
    const size_t kChunk = (size_t)4 << 20;
    for (size_t c0 = 0; c0 < n; c0 += kChunk) {
      if (!step(c0)) return false;
      const size_t c1 = std::min(n, c0 + kChunk);
      sp.clear();
      sp.reserve(c1 - c0);
      for (size_t k = c0; k < c1; ++k) {
        const double mm = std::round((double)pc.z[k] * 1000.0);
        if (!DepthTile::plausible(mm)) { ++rejected; continue; }  // remplissage, NaN
        double gx, gy;
        LonLatToPixel(zs, pc.lon(k), pc.lat(k), gx, gy);
        const long long px = (long long)std::floor(gx), py = (long long)std::floor(gy);
        // clé : rangée puis colonne de tuile (indices de tuile >= 0 en Web Mercator)
        sp.push_back({((uint64_t)(uint32_t)(py >> 8) << 32) | (uint32_t)(px >> 8),
                      (uint32_t)((py & 255) * 256 + (px & 255)), (int32_t)mm});
      }
      std::sort(sp.begin(), sp.end(), [](const SP& a, const SP& b) { return a.key < b.key; });
      for (const SP& p : sp) {
        const long long tx = (long long)(uint32_t)p.key, ty = (long long)(uint32_t)(p.key >> 32);
        if (!put(tx * 256 + (p.pix & 255), ty * 256 + (p.pix >> 8), p.v)) return false;
      }
    }
    std::vector<SP>().swap(sp);
  }
  fed0 = fed;
  return true;
  };
  if (whole) {
    if (!bin(*whole, 1.f)) return false;
  } else {
    // une erreur dans `bin` (écriture, arrêt) arrête la lecture : err renseigné
    if (!(*feed)(bin, err) || !err.empty()) {
      if (err.empty()) err = pd::tr("Aucune sonde importée");
      return false;
    }
    if (meta.lonMin > meta.lonMax) { err = pd::tr("Aucune sonde importée"); return false; }
  }
  if (!inMemory) {
    while (!mem.empty()) {
      std::vector<TK> out;
      for (auto it = mem.begin(); it != mem.end() && out.size() < 64; ++it) out.push_back(it->first);
      if (!flushSet(out)) return false;
    }
  } else {
    for (auto& kv : mem) keysZs.insert(kv.first);
  }

  // accès aux tuiles natives (mémoire, sinon base avec petit cache)
  std::map<TK, DepthTile> rcache;
  auto tileAt = [&](const TK& key) -> const DepthTile* {
    auto m = mem.find(key);
    if (m != mem.end()) return &m->second;
    if (!keysZs.count(key)) return nullptr;
    auto it = rcache.find(key);
    if (it == rcache.end()) {
      if (rcache.size() > 48) rcache.clear();
      DepthTile t;
      if (!readSrc(meta.id, zs, key.first, key.second, t)) return nullptr;
      it = rcache.emplace(key, std::move(t)).first;
    }
    return &it->second;
  };

  // 2) levé maillé rééchantillonné : comble les pixels isolés restés vides
  //    entre deux sondes (rangées « sautées » par le changement de maillage)
  if (gridded) {
    size_t q = 0;
    std::map<TK, std::vector<int32_t>> filled;
    for (const TK& key : keysZs) {
      const DepthTile* tp = tileAt(key);
      if (!tp) continue;
      const DepthTile t = *tp;
      auto at = [&](int i, int j) -> int32_t {
        TK o = key;
        if (i < 0) { o.first--; i += 256; } else if (i > 255) { o.first++; i -= 256; }
        if (j < 0) { o.second--; j += 256; } else if (j > 255) { o.second++; j -= 256; }
        if (o == key) return t.v[j * 256 + i];
        const DepthTile* n2 = tileAt(o);
        return n2 ? n2->v[j * 256 + i] : DepthTile::kEmpty;
      };
      bool changed = false;
      std::vector<int32_t> out = t.v;
      const int32_t* V = t.v.data();
      for (int j = 0; j < 256; ++j)
        for (int i = 0; i < 256; ++i) {
          const int k = j * 256 + i;
          if (V[k] != DepthTile::kEmpty) continue;
          int32_t best = DepthTile::kEmpty;
          int32_t a, b, c, d;
          if (i > 0 && i < 255 && j > 0 && j < 255) {  // intérieur : accès direct
            a = V[k - 1]; b = V[k + 1]; c = V[k - 256]; d = V[k + 256];
          } else {
            a = at(i - 1, j); b = at(i + 1, j); c = at(i, j - 1); d = at(i, j + 1);
          }
          if (a != DepthTile::kEmpty && b != DepthTile::kEmpty) best = std::min(a, b);
          if (c != DepthTile::kEmpty && d != DepthTile::kEmpty)
            best = best == DepthTile::kEmpty ? std::min(c, d) : std::min(best, std::min(c, d));
          if (best != DepthTile::kEmpty) { out[j * 256 + i] = best; changed = true; }
        }
      if (changed) filled[key].swap(out);  // appliqué après le parcours (voisins inchangés)
      if (progress && (++q % 32 == 0)) progress(0.4f + 0.1f * q / keysZs.size(), pd::tr("Comblement des trous"));
    }
    for (auto& kv : filled) {
      if (inMemory) {
        mem[kv.first].v.swap(kv.second);
      } else {
        DepthTile t;
        t.reset();
        t.v.swap(kv.second);
        if (!writeSrc(meta.id, zs, kv.first.first, kv.first.second, t, err)) return false;
      }
    }
    rcache.clear();
  }

  // 2 bis) nuage : surface interpolée (triangulation de Delaunay des pixels
  //    mesurés, interpolation linéaire) dans les pixels vides, sans franchir
  //    une arête de plus de D ni garder les sondes presque alignées. Calcul
  //    tuile par tuile avec une marge de D (sommets des tuiles voisines).
  //    Niveau : celui des sondes, sauf levé en lignes (D bien plus grand que
  //    le pas des sondes) : niveau plus grossier, pixel ~ D / 8, la surface
  //    entre les lignes étant reprise à l'affichage aux niveaux plus fins.
  const int ilevel = meta.interpD > 0
                         ? std::min(zs, LevelForResolution(std::max(meta.res, meta.interpD / 8.0), midLat))
                         : zs;
  long long nInterp = 0;
  auto interpolate = [&](int zl, std::set<TK>& keys, const std::function<const DepthTile*(const TK&)>& get,
                         const std::function<bool(const TK&, DepthTile&)>& put, float p0, float p1) -> bool {
    const double pxM = 156543.03392804097 * std::cos(midLat * M_PI / 180.0) / std::ldexp(1.0, zl);
    const double Dpx = meta.interpD / std::max(1e-6, pxM);
    if (Dpx < 1.5) return true;
    const int mt = std::max(1, (int)std::ceil(Dpx / 256.0));
    struct Vx { int32_t x, y, v; };
    std::map<TK, std::vector<Vx>> vcache;
    auto vertsOf = [&](const TK& k) -> const std::vector<Vx>& {
      auto it = vcache.find(k);
      if (it != vcache.end()) return it->second;
      std::vector<Vx>& out = vcache[k];
      const DepthTile* t = get(k);
      if (t)
        for (int q = 0; q < DepthTile::kN; ++q)
          if (t->v[q] != DepthTile::kEmpty && !t->isInterp(q))
            out.push_back({k.first * 256 + (q & 255), k.second * 256 + (q >> 8), t->v[q]});
      return out;
    };
    std::set<TK> tset;
    for (const TK& k : keys)
      for (int dy = -mt; dy <= mt; ++dy)
        for (int dx = -mt; dx <= mt; ++dx) tset.insert(TK(k.first + dx, k.second + dy));
    std::vector<TK> targets(tset.begin(), tset.end());
    std::sort(targets.begin(), targets.end(), [](const TK& u, const TK& v) {
      return u.second != v.second ? u.second < v.second : u.first < v.first;
    });
    std::set<TK> added;
    size_t q = 0;
    std::vector<double> xy;
    std::vector<int32_t> val;
    std::vector<int> filled;  // pixels remplis (i, j) pour une autre méthode que le TIN
    const InterpMethod method = (InterpMethod)meta.interpMethod;
    const double D2 = Dpx * Dpx;
    for (const TK& T : targets) {
      if (cancel && *cancel) { err = "Annulé"; return false; }
      if (progress && (++q % 16 == 0))
        progress(p0 + (p1 - p0) * q / targets.size(), pd::tr("Interpolation entre les sondes"));
      for (auto it = vcache.begin(); it != vcache.end();)  // rangées passées
        it = it->first.second < T.second - mt ? vcache.erase(it) : std::next(it);
      const double bx0 = T.first * 256.0 - Dpx, bx1 = T.first * 256.0 + 256 + Dpx;
      const double by0 = T.second * 256.0 - Dpx, by1 = T.second * 256.0 + 256 + Dpx;
      xy.clear();
      val.clear();
      for (int dy = -mt; dy <= mt; ++dy)
        for (int dx = -mt; dx <= mt; ++dx) {
          const TK k(T.first + dx, T.second + dy);
          if (!keys.count(k)) continue;
          for (const Vx& p : vertsOf(k)) {
            const double px = p.x + 0.5, py = p.y + 0.5;
            if (px < bx0 || px > bx1 || py < by0 || py > by1) continue;
            xy.push_back(px - T.first * 256.0);
            xy.push_back(py - T.second * 256.0);
            val.push_back(p.v);
          }
        }
      if (val.size() < 3) continue;
      const DepthTile* base = keys.count(T) ? get(T) : nullptr;
      DepthTile out;
      if (base) out = *base;
      else out.reset();
      bool changed = false;
      filled.clear();
      const auto tri = Triangulate(xy);
      for (size_t t = 0; t + 2 < tri.size(); t += 3) {
        const size_t ia = tri[t], ib = tri[t + 1], ic = tri[t + 2];
        const double x0 = xy[2 * ia], y0 = xy[2 * ia + 1], x1 = xy[2 * ib], y1 = xy[2 * ib + 1];
        const double x2 = xy[2 * ic], y2 = xy[2 * ic + 1];
        if (std::max({x0, x1, x2}) < 0 || std::min({x0, x1, x2}) > 256 ||
            std::max({y0, y1, y2}) < 0 || std::min({y0, y1, y2}) > 256)
          continue;  // hors de la tuile
        const double e0 = (x1 - x0) * (x1 - x0) + (y1 - y0) * (y1 - y0);
        const double e1 = (x2 - x1) * (x2 - x1) + (y2 - y1) * (y2 - y1);
        const double e2 = (x0 - x2) * (x0 - x2) + (y0 - y2) * (y0 - y2);
        const double L2 = std::max({e0, e1, e2});
        if (L2 > D2) continue;  // arête plus longue que D
        const double den = (y1 - y2) * (x0 - x2) + (x2 - x1) * (y0 - y2);
        if (std::fabs(den) < 1e-9) continue;
        {  // sondes presque alignées (profil unique, route sinueuse) : plus grand angle
           // proche de 180° ; les triangles fins entre deux lignes sont gardés
          const double b2 = std::min({e0, e1, e2}), a2 = e0 + e1 + e2 - L2 - b2;
          if ((a2 + b2 - L2) / (2 * std::sqrt(a2 * b2) + 1e-30) < -0.985) continue;
        }
        const int i0 = std::max(0, (int)std::floor(std::min({x0, x1, x2})));
        const int i1 = std::min(255, (int)std::ceil(std::max({x0, x1, x2})));
        const int j0 = std::max(0, (int)std::floor(std::min({y0, y1, y2})));
        const int j1 = std::min(255, (int)std::ceil(std::max({y0, y1, y2})));
        for (int j = j0; j <= j1; ++j)
          for (int i = i0; i <= i1; ++i) {
            const int k = j * 256 + i;
            if (out.v[k] != DepthTile::kEmpty) continue;  // mesuré (ou déjà rempli)
            const double px = i + 0.5, py = j + 0.5;
            const double wa = ((y1 - y2) * (px - x2) + (x2 - x1) * (py - y2)) / den;
            const double wb = ((y2 - y0) * (px - x2) + (x0 - x2) * (py - y2)) / den;
            const double wc = 1 - wa - wb;
            if (wa < -1e-9 || wb < -1e-9 || wc < -1e-9) continue;
            out.v[k] = (int32_t)std::lround(wa * val[ia] + wb * val[ib] + wc * val[ic]);
            out.setInterp(k, true);
            changed = true;
            ++nInterp;
            if (method != InterpMethod::Tin) { filled.push_back(i); filled.push_back(j); }
          }
      }
      if (!changed) continue;
      // autre méthode : mêmes pixels (zone du TIN limité par D), autres valeurs
      if (method != InterpMethod::Tin && !filled.empty()) {
        std::vector<double> dval(val.begin(), val.end()), res;
        InterpolatePixels(method, xy, dval, filled, Dpx, res);
        for (size_t q = 0; q < res.size(); ++q)
          if (std::isfinite(res[q]))
            out.v[filled[2 * q + 1] * 256 + filled[2 * q]] = (int32_t)std::lround(res[q]);
        filled.clear();
      }
      if (!keys.count(T)) added.insert(T);
      if (!put(T, out)) return false;
    }
    for (const TK& k : added) keys.insert(k);
    return true;
  };
  if (!gridded && meta.interpD > 0 && ilevel == zs) {  // au niveau des sondes
    if (!interpolate(
            zs, keysZs, tileAt,
            [&](const TK& T, DepthTile& out) {
              if (inMemory) { mem[T] = std::move(out); return true; }
              rcache.erase(T);
              return writeSrc(meta.id, zs, T.first, T.second, out, err);
            },
            0.4f, 0.5f))
      return false;
  }

  // 3) statistiques, écriture et pyramide de la source (plus faible sonde de 2 × 2)
  long long count = 0;
  int32_t lo = INT32_MAX, hi = INT32_MIN;
  std::set<Key> touched;
  for (const TK& key : keysZs) {
    const DepthTile* t = tileAt(key);
    if (!t) continue;
    for (int q = 0; q < DepthTile::kN; ++q) {  // sondes mesurées seulement
      const int32_t v = t->v[q];
      if (v != DepthTile::kEmpty && !t->isInterp(q)) { ++count; lo = std::min(lo, v); hi = std::max(hi, v); }
    }
    if (inMemory && !writeSrc(meta.id, zs, key.first, key.second, *t, err)) return false;
    touched.insert(Key(zs, key.first, key.second));
  }
  if (!count) { err = pd::tr("Aucune sonde importée"); return false; }
  meta.count = count;
  meta.zmin = lo / 1000.f;
  meta.zmax = hi / 1000.f;
  std::map<TK, DepthTile> level;  // niveau courant (enfants) en mémoire si possible
  if (inMemory) level.swap(mem);
  if (!buildPyramid(meta.id, zs, keysZs, level, inMemory, touched, err,
                    [&](float f) { if (progress) progress(0.5f + 0.1f * f, pd::tr("Pyramide de la source")); },
                    cancel))
    return false;
  // levé en lignes : surface interpolée au niveau grossier, puis sa pyramide
  if (!gridded && meta.interpD > 0 && ilevel < zs) {
    std::set<TK> keys;
    for (const Key& k : touched)
      if (std::get<0>(k) == ilevel) keys.insert(TK(std::get<1>(k), std::get<2>(k)));
    std::map<TK, DepthTile> lc;  // tuiles du niveau grossier (peu nombreuses)
    auto get = [&](const TK& k) -> const DepthTile* {
      auto it = lc.find(k);
      if (it != lc.end()) return &it->second;
      DepthTile t;
      if (!readSrc(meta.id, ilevel, k.first, k.second, t)) return nullptr;
      return &lc.emplace(k, std::move(t)).first->second;
    };
    std::set<TK> written;
    if (!interpolate(
            ilevel, keys, get,
            [&](const TK& T, DepthTile& out) {
              written.insert(T);
              touched.insert(Key(ilevel, T.first, T.second));
              return writeSrc(meta.id, ilevel, T.first, T.second, out, err);
            },
            0.6f, 0.65f))
      return false;
    lc.clear();
    std::map<TK, DepthTile> none;
    if (!written.empty() &&
        !buildPyramid(meta.id, ilevel, keys, none, false, touched, err, nullptr, cancel))
      return false;
  }
  meta.interpCount = nInterp;
  if (meta.interpD > 0) {
    char b[200];
    std::snprintf(b, sizeof b, pd::tr(" ; %s, D utilisée %.0f m, %lld pixels interpolés"),
                  pd::tr(InterpMethodName((InterpMethod)meta.interpMethod)), meta.interpD, nInterp);
    meta.info += b;
  }

  // 4) inscription de la source, puis fusion des tuiles concernées
  if (!upsertSource(meta, err)) return false;
  sources_.push_back(meta);
  affectedByBox(meta, touched);
  if (!remergeSet(touched, prio, err,
                  [&](float f, const std::string& m) { if (progress) progress(0.6f + 0.4f * f, m); },
                  cancel))
    return false;
  if (!commitAll(err)) return false;
  if (newId) *newId = meta.id;
  if (progress) progress(1.f, pd::tr("Import terminé"));
  return true;
}

// ------------------------------------------------------------- découpage
bool BathyDb::resplit(double deg, std::string& err, const DbProgress& progress,
                      const std::atomic<bool>* cancel) {
  if (!(deg >= 0.05 && deg <= 1.0)) { err = pd::tr("Taille de cellule invalide"); return false; }
  if (!commitAll(err)) return false;
  const fs::path dir = fs::u8path(folder_), tmp = dir / "_redecoupage";
  std::error_code ec;
  fs::remove_all(tmp, ec);
  fs::create_directories(tmp, ec);
  std::vector<DbCell*> cells = allCells(err);
  long long total = 0, done = 0;
  for (DbCell* c : cells) {
    sqlite3_stmt* q = nullptr;
    sqlite3_prepare_v2(c->db, "SELECT (SELECT COUNT(*) FROM src_tiles) + (SELECT COUNT(*) FROM merged)",
                       -1, &q, nullptr);
    if (q && sqlite3_step(q) == SQLITE_ROW) total += sqlite3_column_int64(q, 0);
    sqlite3_finalize(q);
  }
  bool ok = true;
  {
    BathyDb out;
    out.folder_ = tmp.u8string();
    out.cellDeg_ = deg;
    // copie brute des tuiles (sans décodage) vers le fichier de leur centre
    auto copy = [&](DbCell* c, const char* sel, bool merged) -> bool {
      sqlite3_stmt* q = nullptr;
      if (sqlite3_prepare_v2(c->db, sel, -1, &q, nullptr) != SQLITE_OK) {
        err = std::string(pd::tr("Base : ")) + sqlite3_errmsg(c->db);
        return false;
      }
      const int zc = merged ? 0 : 1;  // colonne de z
      while (sqlite3_step(q) == SQLITE_ROW) {
        const int z = sqlite3_column_int(q, zc), x = sqlite3_column_int(q, zc + 1),
                  y = sqlite3_column_int(q, zc + 2);
        DbCell* o = out.cellFor(z, x, y, true, err);
        if (!o || !o->begin(err)) { sqlite3_finalize(q); return false; }
        sqlite3_stmt* w = merged
            ? o->prep(o->putMerged, "INSERT OR REPLACE INTO merged VALUES(?,?,?,?,?,?)")
            : o->prep(o->putSrc, "INSERT OR REPLACE INTO src_tiles VALUES(?,?,?,?,?)");
        const int n = sqlite3_column_count(q);
        for (int k = 0; k < n; ++k) sqlite3_bind_value(w, k + 1, sqlite3_column_value(q, k));
        const int rc = sqlite3_step(w);
        sqlite3_reset(w);
        if (rc != SQLITE_DONE) {
          err = std::string(pd::tr("Base : ")) + sqlite3_errmsg(o->db);
          sqlite3_finalize(q);
          return false;
        }
        if ((++done & 1023) == 0) {
          if (cancel && *cancel) { err = "Annulé"; sqlite3_finalize(q); return false; }
          if (progress) progress(0.95f * done / std::max(1LL, total), pd::tr("Copie des tuiles"));
        }
      }
      sqlite3_finalize(q);
      return true;
    };
    for (DbCell* c : cells) {
      ok = copy(c, "SELECT src,z,x,y,data FROM src_tiles", false) &&
           copy(c, "SELECT z,x,y,zmin,zmax,data FROM merged", true) && out.commitAll(err);
      if (!ok) break;
    }
    if (ok) {  // sources inscrites dans chaque fichier où elles ont des tuiles
      out.sources_ = sources_;
      for (const DbSource& d : sources_)
        if (!(ok = out.upsertSource(d, err))) break;
      ok = ok && out.commitAll(err);
    }
    if (!ok) out.rollbackAll();
  }  // fichiers de la nouvelle base fermés
  if (!ok) {
    fs::remove_all(tmp, ec);
    return false;
  }
  if (progress) progress(0.97f, pd::tr("Remplacement des fichiers"));
  cells_.clear();  // anciens fichiers fermés
  std::vector<fs::path> olds, news;
  for (auto& e : fs::directory_iterator(dir, ec)) olds.push_back(e.path());
  for (auto& e : fs::directory_iterator(tmp, ec)) news.push_back(e.path());
  for (const fs::path& ep : olds) {
    const std::string n = ep.filename().u8string();
    double lw, ls;
    bool lg;
    if (!ParseCellFileName(n, lw, ls, lg)) continue;
    // un fichier encore ouvert ailleurs (lecture de la vue) : quelques essais
    bool gone = false;
    for (int k = 0; k < 50 && !gone; ++k) {
      fs::remove(ep, ec);
      gone = !fs::exists(ep, ec);
      if (!gone) std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }
    if (!gone) {
      err = pd::tr("Impossible de supprimer ") + n + pd::tr(" (fichier ouvert ?) ; la nouvelle base est dans ") +
            tmp.u8string();
      return false;
    }
    fs::remove(fs::u8path(ep.u8string() + "-wal"), ec);
    fs::remove(fs::u8path(ep.u8string() + "-shm"), ec);
  }
  for (const fs::path& ep : news) {
    const std::string n = ep.filename().u8string();
    double lw, ls;
    bool lg;
    if (!ParseCellFileName(n, lw, ls, lg)) continue;
    fs::rename(ep, dir / ep.filename(), ec);
    if (ec) { err = pd::tr("Déplacement impossible : ") + n + " (" + ec.message() + ")"; return false; }
  }
  fs::remove_all(tmp, ec);
  return open(folder_, err);
}

// ------------------------------------------------------------- gestion
static void SourceKeys(std::vector<DbCell*> cells, int64_t id, std::set<Key>& keys) {
  for (DbCell* c : cells) {
    sqlite3_stmt* q = nullptr;
    sqlite3_prepare_v2(c->db, "SELECT z,x,y FROM src_tiles WHERE src=?", -1, &q, nullptr);
    sqlite3_bind_int64(q, 1, id);
    while (sqlite3_step(q) == SQLITE_ROW)
      keys.insert(Key(sqlite3_column_int(q, 0), sqlite3_column_int(q, 1), sqlite3_column_int(q, 2)));
    sqlite3_finalize(q);
  }
}

// Pyramide d'une source (plus faible sonde de 2 × 2) depuis le niveau `zs`,
// tuiles `child` ; `level` : tuiles du niveau zs déjà en mémoire (facultatif)
bool BathyDb::buildPyramid(int64_t id, int zs, std::set<std::pair<int, int>> child,
                           std::map<std::pair<int, int>, DepthTile>& level, bool keepAll,
                           std::set<Key>& touched, std::string& err,
                           const std::function<void(float)>& progress,
                           const std::atomic<bool>* cancel) {
  using TK = std::pair<int, int>;
  for (int z = zs - 1; z >= kDbMinLevel; --z) {
    if (cancel && *cancel) { err = "Annulé"; return false; }
    std::set<TK> parent;
    for (const auto& c : child) parent.insert({c.first >> 1, c.second >> 1});
    std::map<TK, DepthTile> next;
    for (const auto& p : parent) {
      DepthTile out;
      out.reset();
      bool any = false;
      for (int dy = 0; dy < 2; ++dy)
        for (int dx = 0; dx < 2; ++dx) {
          const TK ck(2 * p.first + dx, 2 * p.second + dy);
          if (!child.count(ck)) continue;
          DepthTile tmp;
          const DepthTile* c = nullptr;
          auto it = level.find(ck);
          if (it != level.end()) c = &it->second;
          else if (readSrc(id, z + 1, ck.first, ck.second, tmp)) c = &tmp;
          if (!c) continue;
          for (int j = 0; j < 256; ++j)
            for (int i = 0; i < 256; ++i) {
              const int32_t v = c->v[j * 256 + i];
              if (v == DepthTile::kEmpty) continue;
              const int ko = (dy * 128 + j / 2) * 256 + dx * 128 + i / 2;
              int32_t& o = out.v[ko];
              // une sonde mesurée l'emporte sur une valeur interpolée, puis la plus faible
              const bool vi = c->isInterp(j * 256 + i), oi = out.isInterp(ko);
              if (o == DepthTile::kEmpty || (oi && !vi) || (oi == vi && v < o)) {
                o = v;
                out.setInterp(ko, vi);
              }
              any = true;
            }
        }
      if (!any) continue;
      if (!writeSrc(id, z, p.first, p.second, out, err)) return false;
      touched.insert(Key(z, p.first, p.second));
      if (keepAll || parent.size() <= 700) next.emplace(p, std::move(out));
    }
    // seuls les parents écrits servent d'enfants au niveau suivant
    std::set<TK> written;
    for (const auto& p : parent)
      if (touched.count(Key(z, p.first, p.second))) written.insert(p);
    level.swap(next);
    child.swap(written);
    if (progress) progress((float)(zs - z) / std::max(1, zs - kDbMinLevel));
  }
  level.clear();
  return true;
}

// Retire des sources les sondes hors de [zMin, zMax] (m, ZH positif vers le
// bas) et les valeurs de remplissage, puis reconstruit pyramides et fusion
bool BathyDb::cleanSources(const std::vector<int64_t>& ids, float zMin, float zMax, DbPriority prio,
                           std::string& err, const DbProgress& progress,
                           const std::atomic<bool>* cancel, long long* removed) {
  if (cleanSourcesImpl(ids, zMin, zMax, prio, err, progress, cancel, removed)) return true;
  rollbackAll();
  return false;
}

bool BathyDb::cleanSourcesImpl(const std::vector<int64_t>& ids, float zMin, float zMax,
                               DbPriority prio, std::string& err, const DbProgress& progress,
                               const std::atomic<bool>* cancel, long long* removed) {
  using TK = std::pair<int, int>;
  const int64_t lo = (int64_t)std::llround((double)zMin * 1000.0),
                hi = (int64_t)std::llround((double)zMax * 1000.0);
  long long gone = 0;
  std::set<Key> keys;
  std::vector<DbCell*> cells = allCells(err);
  for (size_t n = 0; n < ids.size(); ++n) {
    auto sit = std::find_if(sources_.begin(), sources_.end(),
                            [&](const DbSource& d) { return d.id == ids[n]; });
    if (sit == sources_.end()) continue;
    DbSource& d = *sit;
    const std::string label = pd::tr("Nettoyage ") + std::to_string(n + 1) + "/" + std::to_string(ids.size());
    auto prog = [&](float f, const char* what) {
      if (progress) progress(0.8f * (n + f) / ids.size(), label + " : " + what);
    };
    if (!flushWrites(err)) return false;
    // anciennes tuiles (toutes refusionnées ensuite) et tuiles natives
    std::set<Key> old;
    SourceKeys(cells, d.id, old);
    keys.insert(old.begin(), old.end());
    std::vector<TK> native;
    for (const Key& k : old)
      if (std::get<0>(k) == d.level) native.push_back({std::get<1>(k), std::get<2>(k)});
    // niveaux inférieurs effacés : reconstruits depuis le niveau natif
    for (DbCell* c : cells) {
      sqlite3_stmt* q = nullptr;
      sqlite3_prepare_v2(c->db, "DELETE FROM src_tiles WHERE src=? AND z<?", -1, &q, nullptr);
      sqlite3_bind_int64(q, 1, d.id);
      sqlite3_bind_int(q, 2, d.level);
      sqlite3_step(q);
      sqlite3_finalize(q);
    }
    std::set<TK> kept;
    long long count = 0;
    int32_t zlo = INT32_MAX, zhi = INT32_MIN;
    for (size_t k = 0; k < native.size(); ++k) {
      if (cancel && *cancel) { err = "Annulé"; return false; }
      const TK& t = native[k];
      DepthTile tile;
      if (!readSrc(d.id, d.level, t.first, t.second, tile)) continue;  // lecture : filtre ±12 km
      bool changed = false, any = false;
      for (int32_t& v : tile.v) {
        if (v == DepthTile::kEmpty) continue;
        if (v < lo || v > hi) { v = DepthTile::kEmpty; ++gone; changed = true; continue; }
        any = true; ++count;
        zlo = std::min(zlo, v); zhi = std::max(zhi, v);
      }
      DbCell* c = cellFor(d.level, t.first, t.second, false, err);
      if (!any) {
        if (c) {
          sqlite3_stmt* q = nullptr;
          sqlite3_prepare_v2(c->db, "DELETE FROM src_tiles WHERE src=? AND z=? AND x=? AND y=?", -1,
                             &q, nullptr);
          sqlite3_bind_int64(q, 1, d.id);
          sqlite3_bind_int(q, 2, d.level);
          sqlite3_bind_int(q, 3, t.first);
          sqlite3_bind_int(q, 4, t.second);
          sqlite3_step(q);
          sqlite3_finalize(q);
        }
        continue;
      }
      // réécrite aussi quand des valeurs aberrantes ont été écartées à la lecture
      if (!writeSrc(d.id, d.level, t.first, t.second, tile, err)) return false;
      (void)changed;
      kept.insert(t);
      if ((k & 31) == 0) prog(0.5f * k / std::max<size_t>(1, native.size()), pd::tr("tuiles natives"));
    }
    std::map<TK, DepthTile> none;
    std::set<Key> touched;
    if (!buildPyramid(d.id, d.level, kept, none, false, touched, err,
                      [&](float f) { prog(0.5f + 0.5f * f, pd::tr("pyramide")); }, cancel))
      return false;
    keys.insert(touched.begin(), touched.end());
    if (count) { d.count = count; d.zmin = zlo / 1000.f; d.zmax = zhi / 1000.f; }
    if (!upsertSource(d, err)) return false;
  }
  if (removed) *removed = gone;
  if (!remergeSet(keys, prio, err,
                  [&](float f, const std::string& m) { if (progress) progress(0.8f + 0.2f * f, m); },
                  cancel))
    return false;
  return commitAll(err);
}

bool BathyDb::removeSources(const std::vector<int64_t>& ids, DbPriority prio, std::string& err,
                            const DbProgress& progress) {
  if (!flushWrites(err)) return false;
  std::vector<DbSource> gone;
  for (int64_t id : ids)
    if (const DbSource* sp = source(id)) gone.push_back(*sp);
  if (gone.empty()) { err = pd::tr("Source inconnue"); return false; }
  std::set<Key> keys;
  auto cells = allCells(err);
  for (const DbSource& s : gone) {
    SourceKeys(cells, s.id, keys);
    for (DbCell* c : cells) {
      if (!c->begin(err)) return false;
      char sql[160];
      std::snprintf(sql, sizeof sql, "DELETE FROM src_tiles WHERE src=%lld; DELETE FROM sources WHERE id=%lld;",
                    (long long)s.id, (long long)s.id);
      if (!c->exec(sql, err)) return false;
    }
    sources_.erase(std::remove_if(sources_.begin(), sources_.end(),
                                  [&](const DbSource& d) { return d.id == s.id; }),
                   sources_.end());
  }
  for (const DbSource& s : gone) affectedByBox(s, keys);
  if (!remergeSet(keys, prio, err, progress, nullptr)) return false;
  if (!commitAll(err)) return false;
  // journal vidé (les pages libérées sont réutilisées par les imports suivants)
  for (DbCell* c : allCells(err)) c->exec("PRAGMA wal_checkpoint(TRUNCATE);", err);
  return true;
}

bool BathyDb::setVisible(const std::vector<int64_t>& ids, bool visible, DbPriority prio,
                         std::string& err, const DbProgress& progress) {
  auto cells = allCells(err);
  std::set<Key> keys;
  for (int64_t id : ids) {
    DbSource* sp = nullptr;
    for (auto& s : sources_)
      if (s.id == id) sp = &s;
    if (!sp || sp->visible == visible) continue;
    sp->visible = visible;
    for (DbCell* c : cells) {
      if (!c->begin(err)) return false;
      char sql[128];
      std::snprintf(sql, sizeof sql, "UPDATE sources SET visible=%d WHERE id=%lld;", visible ? 1 : 0,
                    (long long)id);
      if (!c->exec(sql, err)) return false;
    }
    SourceKeys(cells, id, keys);
    affectedByBox(*sp, keys);
  }
  if (!remergeSet(keys, prio, err, progress, nullptr)) return false;
  return commitAll(err);
}

bool BathyDb::setGroup(const std::vector<int64_t>& ids, const std::string& group, std::string& err) {
  auto cells = allCells(err);
  for (int64_t id : ids) {
    for (auto& s : sources_)
      if (s.id == id) s.group = group;
    for (DbCell* c : cells) {
      if (!c->begin(err)) return false;
      sqlite3_stmt* q = nullptr;
      sqlite3_prepare_v2(c->db, "UPDATE sources SET grp=? WHERE id=?", -1, &q, nullptr);
      sqlite3_bind_text(q, 1, group.c_str(), -1, SQLITE_TRANSIENT);
      sqlite3_bind_int64(q, 2, id);
      const int rc = sqlite3_step(q);
      sqlite3_finalize(q);
      if (rc != SQLITE_DONE) { err = std::string(pd::tr("Base : ")) + sqlite3_errmsg(c->db); return false; }
    }
  }
  return commitAll(err);
}

bool BathyDb::setParams(const std::vector<int64_t>& ids, const std::string& params,
                        std::string& err) {
  if (!flushWrites(err)) return false;
  for (DbCell* c : allCells(err)) {
    if (!c->begin(err)) return false;
    for (int64_t id : ids) {
      sqlite3_stmt* q = nullptr;
      sqlite3_prepare_v2(c->db, "UPDATE sources SET params=? WHERE id=?", -1, &q, nullptr);
      sqlite3_bind_text(q, 1, params.c_str(), -1, SQLITE_TRANSIENT);
      sqlite3_bind_int64(q, 2, id);
      sqlite3_step(q);
      sqlite3_finalize(q);
    }
  }
  for (auto& d : sources_)
    if (std::find(ids.begin(), ids.end(), d.id) != ids.end()) d.params = params;
  return commitAll(err);
}

// Date du levé (AAAA-MM-JJ) : métadonnée ; la fusion n'est refaite, sur les
// seules tuiles des sources concernées, que si la règle de priorité utilise la date
bool BathyDb::setDates(const std::vector<int64_t>& ids, const std::string& date, DbPriority prio,
                       std::string& err, const DbProgress& progress) {
  if (!flushWrites(err)) return false;
  std::vector<DbCell*> cells = allCells(err);
  std::set<Key> keys;
  for (int64_t id : ids) {
    for (DbCell* c : cells) {
      sqlite3_stmt* q = nullptr;
      sqlite3_prepare_v2(c->db, "UPDATE sources SET date=? WHERE id=?", -1, &q, nullptr);
      sqlite3_bind_text(q, 1, date.c_str(), -1, SQLITE_TRANSIENT);
      sqlite3_bind_int64(q, 2, id);
      if (!c->begin(err)) { sqlite3_finalize(q); return false; }
      sqlite3_step(q);
      sqlite3_finalize(q);
    }
    for (auto& d : sources_)
      if (d.id == id) d.date = date;
    if (prio == DbPriority::ResolutionDate || prio == DbPriority::Newest) SourceKeys(cells, id, keys);
  }
  if (!keys.empty() && !remergeSet(keys, prio, err, progress, nullptr)) return false;
  return commitAll(err);
}

bool BathyDb::setRanks(const std::vector<int64_t>& order, DbPriority prio, std::string& err,
                       const DbProgress& progress) {
  auto cells = allCells(err);
  for (size_t r = 0; r < order.size(); ++r) {
    for (auto& s : sources_)
      if (s.id == order[r]) s.rank = (int)r;
    for (DbCell* c : cells) {
      if (!c->begin(err)) return false;
      char sql[128];
      std::snprintf(sql, sizeof sql, "UPDATE sources SET rank=%d WHERE id=%lld;", (int)r, (long long)order[r]);
      if (!c->exec(sql, err)) return false;
    }
  }
  std::sort(sources_.begin(), sources_.end(),
            [](const DbSource& a, const DbSource& b) { return a.rank < b.rank; });
  if (!commitAll(err)) return false;
  return prio == DbPriority::Manual ? remergeAll(prio, err, progress) : true;
}

bool BathyDb::remergeAll(DbPriority prio, std::string& err, const DbProgress& progress,
                         const std::atomic<bool>* cancel) {
  if (!flushWrites(err)) return false;
  std::set<Key> keys;
  for (DbCell* c : allCells(err))
    for (const char* sql : {"SELECT DISTINCT z,x,y FROM src_tiles", "SELECT z,x,y FROM merged"}) {
      sqlite3_stmt* q = nullptr;
      sqlite3_prepare_v2(c->db, sql, -1, &q, nullptr);
      while (sqlite3_step(q) == SQLITE_ROW)
        keys.insert(Key(sqlite3_column_int(q, 0), sqlite3_column_int(q, 1), sqlite3_column_int(q, 2)));
      sqlite3_finalize(q);
    }
  if (!remergeSet(keys, prio, err, progress, cancel)) return false;
  return commitAll(err);
}

// ------------------------------------------------------------- lecture
int BathyDb::maxMergedLevel(double lonMin, double lonMax, double latMin, double latMax) {
  std::string err;
  if (pendingMerged_) flushWrites(flushErr_);
  int best = 0;
  for (DbCell* c : allCells(err)) {
    // fichiers voisins de l'emprise seulement (les tuiles grossières, rangées
    // au fichier de leur centre, peuvent déborder : marge de 1°)
    const std::string n = fs::u8path(c->path).filename().u8string();
    double lon0, lat0;
    bool lg;
    if (ParseCellFileName(n, lon0, lat0, lg)) {
      const double d = lg ? 1.0 : cellDeg_;
      if (lat0 > latMax + 1 || lat0 + d < latMin - 1 || lon0 > lonMax + 1 || lon0 + d < lonMin - 1) continue;
    }
    sqlite3_stmt* q = nullptr;
    sqlite3_prepare_v2(c->db, "SELECT MAX(z) FROM merged", -1, &q, nullptr);
    if (sqlite3_step(q) == SQLITE_ROW) best = std::max(best, sqlite3_column_int(q, 0));
    sqlite3_finalize(q);
  }
  return best;
}

bool BathyDb::buildView(double lonMin, double lonMax, double latMin, double latMax, int z,
                        size_t maxCells, PointCloud& out, double& dlon, double& dlat, int& usedLevel,
                        std::string& err, const std::atomic<bool>* cancel,
                        std::vector<int64_t>* srcIds, std::vector<uint8_t>* interp,
                        bool skipInterp) {
  out.clear();
  if (srcIds) srcIds->clear();
  if (interp) interp->clear();
  const int zmax = maxMergedLevel(lonMin, lonMax, latMin, latMax);
  if (zmax < kDbMinLevel) { err = pd::tr("Base vide dans cette zone"); return false; }
  int zz = std::max(kDbMinLevel, std::min(z, zmax));
  const double midLat = 0.5 * (latMin + latMax);
  const double cosL = std::max(0.05, std::cos(midLat * M_PI / 180.0));
  for (;;) {
    dlon = 360.0 / std::ldexp((double)kTileSize, zz);
    dlat = dlon * cosL;
    const double cells = (lonMax - lonMin) / dlon * (latMax - latMin) / dlat;
    if (cells <= (double)maxCells || zz == kDbMinLevel) break;
    --zz;
  }
  usedLevel = zz;
  double latAnchor = 0;
  {  // vue très large : cellules plus grandes que les pixels du niveau le plus grossier
    const double cells = (lonMax - lonMin) / dlon * (latMax - latMin) / dlat;
    if (cells > (double)maxCells) {
      const double f = std::ceil(std::sqrt(cells / (double)maxCells));
      dlon *= f;
      dlat *= f;
    }
  }
  // Cellules alignées sur les pixels de la base (pas de saut d'une vue à
  // l'autre) : colonnes = colonnes de pixels du niveau zz ; lignes calées sur
  // la rangée de pixels Mercator du centre de la vue, avec la hauteur exacte de
  // cette rangée. Sinon toute la vue était décalée en latitude d'une même
  // fraction de cellule (jusqu'à une demi-cellule : sondes et couleurs à côté
  // de la route quand la vue est au pas des données).
  {
    const double pix = 360.0 / std::ldexp((double)kTileSize, zz);
    const double f = dlon / pix;  // 1, ou plus pour une vue très large
    double gxm, gym, lonT, latT, latB;
    LonLatToPixel(zz, 0.0, midLat, gxm, gym);
    PixelToLonLat(zz, 0.0, std::floor(gym), lonT, latT);
    PixelToLonLat(zz, 0.0, std::floor(gym) + 1.0, lonT, latB);
    dlat = (latT - latB) * f;
    latAnchor = latT;
  }
  const double lon0 = std::floor(lonMin / dlon) * dlon;
  const double lat1 = latAnchor + std::ceil((latMax - latAnchor) / dlat) * dlat;
  const int nx = (int)std::ceil((lonMax - lon0) / dlon), ny = (int)std::ceil((lat1 - latMin) / dlat);
  std::vector<long long> gxs(nx), gys(ny);
  for (int i = 0; i < nx; ++i) {
    double gx, gy;
    LonLatToPixel(zz, lon0 + (i + 0.5) * dlon, midLat, gx, gy);
    gxs[i] = (long long)std::floor(gx);
  }
  for (int j = 0; j < ny; ++j) {
    double gx, gy;
    LonLatToPixel(zz, lon0, lat1 - (j + 0.5) * dlat, gx, gy);
    gys[j] = (long long)std::floor(gy);
  }
  // tuiles fusionnées ; là où le niveau demandé n'existe pas (zone couverte
  // seulement par des sources plus grossières), on remonte la pyramide
  std::map<std::tuple<int, long long, long long>, DepthTile> tiles;
  auto tileOf = [&](int L, long long tx, long long ty) -> const DepthTile* {
    const auto key = std::make_tuple(L, tx, ty);
    auto it = tiles.find(key);
    if (it == tiles.end()) {
      DepthTile t;
      if (!readMerged(L, (int)tx, (int)ty, t)) t = DepthTile();
      it = tiles.emplace(key, std::move(t)).first;
    }
    return it->second.allocated() ? &it->second : nullptr;
  };
  // Surface interpolée lissée (bilinéaire) depuis le niveau L = zz - d :
  // 1 = valeur (au moins un pixel interpolé parmi les quatre), 0 = sinon (voir
  // un niveau plus haut). Un trou entouré de sondes mesurées seulement (levé
  // dense) n'est jamais rempli : aucun pixel interpolé autour, à aucun niveau.
  auto bilinear = [&](int d, long long gx, long long gy, int32_t& v, int64_t& src) -> int {
    const int L = zz - d;
    const double sc = std::ldexp(1.0, -d);
    const double fx = (gx + 0.5) * sc - 0.5, fy = (gy + 0.5) * sc - 0.5;
    const long long i0 = (long long)std::floor(fx), j0 = (long long)std::floor(fy);
    const double tx = fx - i0, ty = fy - j0;
    double acc = 0, ws = 0;
    int n = 0, ni = 0;
    for (int dj = 0; dj <= 1; ++dj)
      for (int di = 0; di <= 1; ++di) {
        const long long px = i0 + di, py = j0 + dj;
        if (px < 0 || py < 0) continue;
        const DepthTile* t = tileOf(L, px >> 8, py >> 8);
        if (!t) continue;
        const int k = (int)((py & 255) * 256 + (px & 255));
        if (t->v[k] == DepthTile::kEmpty) continue;
        const double w = (di ? tx : 1 - tx) * (dj ? ty : 1 - ty) + 1e-6;
        acc += w * t->v[k];
        ws += w;
        ++n;
        if (t->isInterp(k)) { ++ni; src = t->sourceAt(k); }
      }
    if (!ni && n == 4) {  // entouré de mesures : surface interpolée juste autour ?
      // (croisement de lignes d'un levé : rempli ; trou d'un levé dense : non)
      for (int dj = -1; dj <= 2 && !ni; ++dj)
        for (int di = -1; di <= 2 && !ni; ++di) {
          const long long px = i0 + di, py = j0 + dj;
          if (px < 0 || py < 0 || (di >= 0 && di <= 1 && dj >= 0 && dj <= 1)) continue;
          const DepthTile* t = tileOf(L, px >> 8, py >> 8);
          if (t && t->isInterp((int)((py & 255) * 256 + (px & 255)))) ni = 1;
        }
    }
    if (!ni) return 0;  // mesures seulement (ou rien) : voir un niveau plus haut
    v = (int32_t)std::lround(acc / ws);
    return 1;
  };
  auto pixel = [&](long long gx, long long gy, int32_t& v, int64_t& src, bool& ip) -> bool {
    bool fineExists = false;  // une tuile plus fine existe : pas de valeur mesurée grossière
    for (int d = 0; zz - d >= kDbMinLevel && d <= 10; ++d) {
      const long long px = gx >> d, py = gy >> d;
      const DepthTile* t = tileOf(zz - d, px >> 8, py >> 8);
      if (!t) {  // tuile absente : niveau inférieur (rien plus haut si un niveau fin existait)
        if (fineExists) return false;
        continue;
      }
      const int k = (int)((py & 255) * 256 + (px & 255));
      v = t->v[k];
      if (v != DepthTile::kEmpty && (d == 0 || (!fineExists && !t->isInterp(k)))) {
        src = t->sourceAt(k);
        ip = t->isInterp(k);
        return true;  // valeur du niveau (ou d'une source plus grossière)
      }
      // vide ici, ou surface interpolée d'un niveau plus grossier : lissée
      if (bilinear(d, gx, gy, v, src) == 1) { ip = true; return true; }
      fineExists = true;  // niveau plus grossier
    }
    return false;
  };
  out.reserve((size_t)nx * ny / 4);
  for (int j = 0; j < ny; ++j) {
    if (cancel && *cancel) { err = "Annulé"; return false; }
    const double lat = lat1 - (j + 0.5) * dlat;
    for (int i = 0; i < nx; ++i) {
      int32_t v;
      int64_t src;
      bool ip = false;
      const double lon = lon0 + (i + 0.5) * dlon;
      if (hiddenAt(lon, lat)) continue;  // fichier .pdb masqué
      if (!pixel(gxs[i], gys[j], v, src, ip)) continue;
      if (ip && skipInterp) continue;    // zones interpolées masquées
      out.add(lon, lat, v / 1000.f);
      if (srcIds) srcIds->push_back(src);
      if (interp) interp->push_back(ip ? 1 : 0);
    }
    if (tiles.size() > 600) tiles.clear();  // mémoire bornée (vues très larges)
  }
  if (out.empty()) { err = pd::tr("Aucune donnée de la base dans cette zone"); return false; }
  out.computeBounds();
  return true;
}

int64_t BathyDb::sourceAt(double lon, double lat, int z) {
  if (hiddenAt(lon, lat)) return 0;
  for (int zz = z; zz >= kDbMinLevel; --zz) {
    double gx, gy;
    LonLatToPixel(zz, lon, lat, gx, gy);
    const long long ix = (long long)std::floor(gx), iy = (long long)std::floor(gy);
    DepthTile t;
    if (!readMerged(zz, (int)(ix >> 8), (int)(iy >> 8), t)) continue;  // niveau absent ici
    const int k = (int)((iy & 255) * 256 + (ix & 255));
    return t.v[k] == DepthTile::kEmpty ? 0 : t.sourceAt(k);
  }
  return 0;
}

}  // namespace pd

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
#include "Lang.h"

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
        " grp TEXT, params TEXT);"
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
    bool hasGrp = false, hasParams = false;
    sqlite3_stmt* q = nullptr;
    sqlite3_prepare_v2(db, "PRAGMA table_info(sources)", -1, &q, nullptr);
    while (q && sqlite3_step(q) == SQLITE_ROW) {
      const unsigned char* n = sqlite3_column_text(q, 1);
      if (n && std::strcmp((const char*)n, "grp") == 0) hasGrp = true;
      if (n && std::strcmp((const char*)n, "params") == 0) hasParams = true;
    }
    sqlite3_finalize(q);
    // bases plus anciennes : paramètres d'import de chaque source (relecture)
    return (hasGrp || exec("ALTER TABLE sources ADD COLUMN grp TEXT", err)) &&
           (hasParams || exec("ALTER TABLE sources ADD COLUMN params TEXT", err));
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
  struct stat st;
  if (stat(path.c_str(), &st) == 0) {
    std::time_t t = st.st_mtime;
    char b[32];
    std::strftime(b, sizeof b, "%Y-%m-%d", std::localtime(&t));
    return b;
  }
  return Today();
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
  if (pc.srcStepX > 0 && pc.srcStepY > 0) {
    gridded = true;
    return std::max(pc.srcStepX, pc.srcStepY);
  }
  const double midLat = 0.5 * (pc.latMin + pc.latMax);
  const double cosL = std::max(0.01, std::cos(midLat * M_PI / 180.0));
  {
    const size_t n = std::min<size_t>(pc.size(), 300000);
    std::vector<int64_t> a(pc.qlat.begin(), pc.qlat.begin() + n), b(pc.qlon.begin(), pc.qlon.begin() + n);
    const double sl = LatticeStepUnits(a), so = sl > 0 ? LatticeStepUnits(b) : 0;
    if (sl > 0 && so > 0) {
      gridded = true;
      return std::max(sl * PointCloud::kQ * 110540.0, so * PointCloud::kQ * 111320.0 * cosL);
    }
  }
  const double area = std::max(1.0, (pc.lonMax - pc.lonMin) * 111320.0 * cosL *
                                        (pc.latMax - pc.latMin) * 110540.0);
  return std::sqrt(area / (double)pc.size());
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
                           "lonmin,lonmax,latmin,latmax,imported,grp,params FROM sources",
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
                       "count,lonmin,lonmax,latmin,latmax,imported,grp,params) VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?)", -1,
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
  DepthTile t;
  for (const DbSource* s : cand) {
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
          }
        }
      }
    }
    if (!have) continue;
    int slot = -1;
    for (int k = 0; k < DepthTile::kN; ++k) {
      const int32_t v = t.v[k];
      if (v == DepthTile::kEmpty) continue;
      const bool take = m.v[k] == DepthTile::kEmpty || (prio == DbPriority::Shoalest && v < m.v[k]);
      if (!take) continue;
      if (slot < 0) {
        if (ids.size() >= 255) break;
        slot = (int)ids.size();
        ids.push_back(s->id);
      }
      m.v[k] = v;
      idx[k] = (uint8_t)slot;
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
  if (importCloudImpl(pc, meta, gridded, prio, err, progress, cancel, newId)) return true;
  rollbackAll();
  return false;
}

bool BathyDb::importCloudImpl(const PointCloud& pc, DbSource meta, bool gridded, DbPriority prio,
                              std::string& err, const DbProgress& progress,
                              const std::atomic<bool>* cancel, int64_t* newId) {
  if (pc.empty()) { err = pd::tr("Source vide"); return false; }
  const double midLat = 0.5 * (pc.latMin + pc.latMax);
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
  meta.lonMin = pc.lonMin; meta.lonMax = pc.lonMax; meta.latMin = pc.latMin; meta.latMax = pc.latMax;

  // Tuiles du niveau natif : en mémoire si la source est de taille raisonnable
  // (une seule écriture par tuile), sinon écrites au fil de l'eau
  using TK = std::pair<int, int>;
  double gx0, gy0, gx1, gy1;
  LonLatToPixel(zs, pc.lonMin, pc.latMax, gx0, gy0);
  LonLatToPixel(zs, pc.lonMax, pc.latMin, gx1, gy1);
  const double tileBound = (std::floor(gx1 / 256) - std::floor(gx0 / 256) + 1) *
                           (std::floor(gy1 / 256) - std::floor(gy0 / 256) + 1);
  const bool inMemory = tileBound <= 700;  // ~180 Mo au plus
  std::map<TK, DepthTile> mem;             // tuiles en mémoire (toutes si inMemory)
  std::map<TK, uint64_t> used;
  std::set<TK> keysZs;                     // tuiles déjà écrites (mode flux)
  uint64_t clock = 0;
  auto flushOne = [&](const TK& key) -> bool {
    DepthTile& t = mem[key];
    DepthTile old;
    if (keysZs.count(key) && readSrc(meta.id, zs, key.first, key.second, old))
      for (int k = 0; k < DepthTile::kN; ++k)
        if (old.v[k] != DepthTile::kEmpty && (t.v[k] == DepthTile::kEmpty || old.v[k] < t.v[k]))
          t.v[k] = old.v[k];
    keysZs.insert(key);
    const bool ok = writeSrc(meta.id, zs, key.first, key.second, t, err);
    mem.erase(key);
    used.erase(key);
    return ok;
  };

  // 1) répartition des sondes par pixel : plus faible sonde du pixel
  const size_t n = pc.size();
  size_t rejected = 0;
  DepthTile* curTile = nullptr;
  TK curKey(0, 0);
  for (size_t k = 0; k < n; ++k) {
    const double mm = std::round((double)pc.z[k] * 1000.0);
    if (!DepthTile::plausible(mm)) { ++rejected; continue; }  // remplissage, NaN
    const int32_t v = (int32_t)mm;
    double gx, gy;
    LonLatToPixel(zs, pc.lon(k), pc.lat(k), gx, gy);
    const long long ix = (long long)std::floor(gx), iy = (long long)std::floor(gy);
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
      for (int q = 0; q < 64; ++q)
        if (!flushOne(age[q].second)) return false;
      curTile = nullptr;
    }
    if ((k & 0xFFFFF) == 0) {
      if (cancel && *cancel) { err = "Annulé"; return false; }
      if (progress) progress(0.4f * k / n, pd::tr("Répartition des sondes"));
    }
  }
  if (!inMemory) {
    while (!mem.empty())
      if (!flushOne(mem.begin()->first)) return false;
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

  // 3) statistiques, écriture et pyramide de la source (plus faible sonde de 2 × 2)
  long long count = 0;
  int32_t lo = INT32_MAX, hi = INT32_MIN;
  std::set<Key> touched;
  for (const TK& key : keysZs) {
    const DepthTile* t = tileAt(key);
    if (!t) continue;
    for (int32_t v : t->v)
      if (v != DepthTile::kEmpty) { ++count; lo = std::min(lo, v); hi = std::max(hi, v); }
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
              int32_t& o = out.v[(size_t)(dy * 128 + j / 2) * 256 + dx * 128 + i / 2];
              if (o == DepthTile::kEmpty || v < o) o = v;
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
                        std::vector<int64_t>* srcIds) {
  out.clear();
  if (srcIds) srcIds->clear();
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
  {  // vue très large : cellules plus grandes que les pixels du niveau le plus grossier
    const double cells = (lonMax - lonMin) / dlon * (latMax - latMin) / dlat;
    if (cells > (double)maxCells) {
      const double f = std::ceil(std::sqrt(cells / (double)maxCells));
      dlon *= f;
      dlat *= f;
    }
  }
  // cellules alignées sur un maillage global (pas de saut d'une vue à l'autre)
  const double lon0 = std::floor(lonMin / dlon) * dlon, lat1 = std::ceil(latMax / dlat) * dlat;
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
  auto pixel = [&](long long gx, long long gy, int32_t& v, int64_t& src) -> bool {
    for (int d = 0; zz - d >= kDbMinLevel; ++d) {
      const long long px = gx >> d, py = gy >> d;
      const auto key = std::make_tuple(zz - d, px >> 8, py >> 8);
      auto it = tiles.find(key);
      if (it == tiles.end()) {
        DepthTile t;
        if (!readMerged(zz - d, (int)(px >> 8), (int)(py >> 8), t)) t = DepthTile();
        it = tiles.emplace(key, std::move(t)).first;
      }
      if (!it->second.allocated()) continue;  // tuile absente : niveau inférieur
      const int k = (int)((py & 255) * 256 + (px & 255));
      v = it->second.v[k];
      if (v == DepthTile::kEmpty) return false;
      src = it->second.sourceAt(k);
      return true;
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
      const double lon = lon0 + (i + 0.5) * dlon;
      if (hiddenAt(lon, lat)) continue;  // fichier .pdb masqué
      if (!pixel(gxs[i], gys[j], v, src)) continue;
      out.add(lon, lat, v / 1000.f);
      if (srcIds) srcIds->push_back(src);
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

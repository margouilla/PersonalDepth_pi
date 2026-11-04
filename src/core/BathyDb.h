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

// Base de bathymétrie : les sources importées et leur fusion, dans des
// fichiers SQLite découpés automatiquement par carré de 1° (« PD_N48W005.pdb »),
// rangés dans un même dossier.
//
// Chaque fichier contient :
//   sources    : métadonnées de chaque source (répétées dans chaque fichier touché)
//   src_tiles  : sondes de chaque source, pixel par pixel (plus faible sonde du
//                pixel, valeur exacte), à son niveau natif puis en pyramide
//   merged     : surface fusionnée (une valeur par pixel, celle de la source
//                prioritaire, et le numéro de cette source), tous niveaux
// Une tuile (z, x, y) est rangée dans le fichier du carré qui contient son centre.
#pragma once
#include <atomic>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <tuple>
#include <vector>

#include "PointCloud.h"
#include "TileCodec.h"

struct sqlite3;

namespace pd {

// Fichier .pdb d'une cellule géographique du dossier de base
struct DbCellInfo {
  std::string name;
  double lonW = 0, latS = 0, deg = 1;  // coin sud-ouest et taille (°)
  long long bytes = 0;
  int sources = 0;
};
constexpr double kDefaultCellDeg = 0.25;  // découpage des nouvelles bases (15')
std::string CellFileName(double lon, double lat, double deg);
bool ParseCellFileName(const std::string& name, double& lonW, double& latS, bool& legacy);

enum class DbPriority { ResolutionDate = 0, Shoalest = 1, Newest = 2, Manual = 3 };

struct DbSource {
  int64_t id = 0;
  std::string name, path, date;  // date AAAA-MM-JJ
  int level = 0;                 // niveau natif
  double res = 0;                // résolution du levé (m)
  int rank = 0;                  // rang manuel (0 = en tête)
  bool visible = true;
  std::string crs, vref;         // système d'origine, référence verticale appliquée
  float zmin = 0, zmax = 0;      // m
  long long count = 0;           // pixels au niveau natif
  double lonMin = 0, lonMax = 0, latMin = 0, latMax = 0;
  std::string imported;          // date d'import
  std::string group;             // classe (Litto3D, PFE…)
};

using DbProgress = std::function<void(float, const std::string&)>;

class DbCell;  // un fichier SQLite

class BathyDb {
 public:
  BathyDb();
  ~BathyDb();
  BathyDb(const BathyDb&) = delete;
  BathyDb& operator=(const BathyDb&) = delete;

  // Ouvre (ou crée) le dossier de la base et lit la liste des sources
  // `newDeg` : taille des cellules si le dossier ne contient pas encore de base
  bool open(const std::string& folder, std::string& err, double newDeg = 0);
  double cellDeg() const { return cellDeg_; }
  std::vector<DbCellInfo> cellInfo();
  // Fichiers .pdb masqués : leur emprise est exclue de la vue et des sondes
  void setHiddenCells(const std::set<std::string>& names);
  bool hiddenAt(double lon, double lat) const;
  // Redécoupe le dossier en cellules de `deg` degrés (copie des tuiles dans
  // un sous-dossier, puis remplacement des anciens fichiers)
  bool resplit(double deg, std::string& err, const DbProgress& progress = nullptr,
               const std::atomic<bool>* cancel = nullptr);
  const std::string& folder() const { return folder_; }
  const std::vector<DbSource>& sources() const { return sources_; }
  const DbSource* source(int64_t id) const;
  bool reloadSources(std::string& err);
  // Bases créées avant le filtre des valeurs de remplissage (±12 km) : zmin /
  // zmax aberrants, recalculés sur les tuiles natives (long : hors interface)
  bool needsRepair() const;
  bool repairStats(std::string& err);

  // Import d'un nuage déjà converti (lon/lat WGS84, profondeur ZH positive).
  // `gridded` : levé maillé (les trous isolés dus au rééchantillonnage sont comblés).
  bool importCloud(const PointCloud& pc, DbSource meta, bool gridded, DbPriority prio,
                   std::string& err, const DbProgress& progress = nullptr,
                   const std::atomic<bool>* cancel = nullptr, int64_t* newId = nullptr);
  bool removeSource(int64_t id, DbPriority prio, std::string& err,
                    const DbProgress& progress = nullptr) {
    return removeSources({id}, prio, err, progress);
  }
  bool setVisible(int64_t id, bool visible, DbPriority prio, std::string& err,
                  const DbProgress& progress = nullptr) {
    return setVisible(std::vector<int64_t>{id}, visible, prio, err, progress);
  }
  // Opérations groupées : une seule refusion des tuiles concernées
  bool removeSources(const std::vector<int64_t>& ids, DbPriority prio, std::string& err,
                     const DbProgress& progress = nullptr);
  bool setVisible(const std::vector<int64_t>& ids, bool visible, DbPriority prio,
                  std::string& err, const DbProgress& progress = nullptr);
  bool setGroup(const std::vector<int64_t>& ids, const std::string& group, std::string& err);
  // Rangs manuels (ordre de la liste) ; refusionne si la règle est « manuelle »
  bool setRanks(const std::vector<int64_t>& order, DbPriority prio, std::string& err,
                const DbProgress& progress = nullptr);
  // Retire les sondes hors de [zMin, zMax] (m, ZH positif vers le bas) et les
  // valeurs de remplissage des sources, reconstruit leurs pyramides et refusionne
  bool cleanSources(const std::vector<int64_t>& ids, float zMin, float zMax, DbPriority prio,
                    std::string& err, const DbProgress& progress = nullptr,
                    const std::atomic<bool>* cancel = nullptr, long long* removed = nullptr);
  bool remergeAll(DbPriority prio, std::string& err, const DbProgress& progress = nullptr,
                  const std::atomic<bool>* cancel = nullptr);

  // Lecture de la surface fusionnée
  bool readMerged(int z, int x, int y, DepthTile& t);
  int maxMergedLevel(double lonMin, double lonMax, double latMin, double latMax);
  // Cellules régulières lon/lat (centres) couvrant l'emprise, au niveau z
  // (ou moins si absent) : profondeurs fusionnées, pour l'affichage
  bool buildView(double lonMin, double lonMax, double latMin, double latMax, int z,
                 size_t maxCells, PointCloud& out, double& dlon, double& dlat, int& usedLevel,
                 std::string& err, const std::atomic<bool>* cancel = nullptr,
                 std::vector<int64_t>* srcIds = nullptr);
  // Source retenue au point (fusion), 0 si aucune
  int64_t sourceAt(double lon, double lat, int z);

 private:
  DbCell* cellFor(int z, int x, int y, bool create, std::string& err);
  DbCell* cellByName(const std::string& name, bool create, std::string& err);
  std::vector<DbCell*> allCells(std::string& err);
  bool readSrc(int64_t id, int z, int x, int y, DepthTile& t);
  bool writeSrc(int64_t id, int z, int x, int y, const DepthTile& t, std::string& err);
  bool remergeTile(int z, int x, int y, DbPriority prio, std::string& err);
  bool remergeSet(const std::set<std::tuple<int, int, int>>& keys, DbPriority prio,
                  std::string& err, const DbProgress& progress, const std::atomic<bool>* cancel);
  void affectedByBox(const DbSource& s, std::set<std::tuple<int, int, int>>& keys);
  bool upsertSource(const DbSource& s, std::string& err);
  bool buildPyramid(int64_t id, int zs, std::set<std::pair<int, int>> child,
                    std::map<std::pair<int, int>, DepthTile>& level, bool keepAll,
                    std::set<std::tuple<int, int, int>>& touched, std::string& err,
                    const std::function<void(float)>& progress,
                    const std::atomic<bool>* cancel = nullptr);
  bool commitAll(std::string& err);

  void rollbackAll();
  // écritures de tuiles en attente (encodage parallèle par lots)
  struct PendingWrite {
    bool merged;
    int64_t id;
    int z, x, y;
    DepthTile t;
  };
  static constexpr size_t kWriteBatch = 64;
  std::vector<PendingWrite> pending_;
  size_t pendingSrc_ = 0, pendingMerged_ = 0;
  std::string flushErr_;
  bool flushWrites(std::string& err);
  bool writeMerged(int z, int x, int y, DepthTile&& t, std::string& err);
  bool importCloudImpl(const PointCloud& pc, DbSource meta, bool gridded, DbPriority prio,
                       std::string& err, const DbProgress& progress,
                       const std::atomic<bool>* cancel, int64_t* newId);
  bool cleanSourcesImpl(const std::vector<int64_t>& ids, float zMin, float zMax, DbPriority prio,
                        std::string& err, const DbProgress& progress,
                        const std::atomic<bool>* cancel, long long* removed);
  std::string folder_;
  double cellDeg_ = kDefaultCellDeg;
  std::set<std::pair<int, int>> hidden_;
  std::vector<DbSource> sources_;
  std::map<std::string, std::unique_ptr<DbCell>> cells_;
};

// Résolution d'un nuage (m) ; gridded = levé maillé détecté
double EstimateResolution(const PointCloud& pc, bool& gridded);
// Date du levé : AAAAMMJJ dans le nom du fichier, sinon date du fichier
std::string GuessSurveyDate(const std::string& path);
// Classe proposée : premier mot du nom de fichier qui n'est pas un nombre
// (LITTO3D_FRA_… -> LITTO3D, 20210325-31_PFE-G3_… -> PFE)
std::string GuessSourceGroup(const std::string& path);

}  // namespace pd

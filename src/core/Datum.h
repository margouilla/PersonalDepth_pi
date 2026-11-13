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

// Référentiel vertical commun des bases : les profondeurs sont rangées sous
// le niveau moyen (NM). Chaque autre référence k est reliée au NM par une
// surface S_k = hauteur du NM au-dessus de la référence k (m) :
//   profondeur_NM = profondeur_k + S_k        (import)
//   profondeur_k  = profondeur_NM − S_k       (affichage)
//   S_cd  : zéro des cartes local — France : ZH (BathyElli NM/ell − ZH/ell),
//           ailleurs : plus basse mer astronomique FES2022b (LAT) ;
//           sans référence disponible : 0, « non défini »
//   S_lat : LAT FES2022b (19 ans)
//   S_ell : ellipsoïde WGS84 — France : BathyElli NM/ell, ailleurs : MSS
//           (CNES_CLS 2025 ou autre grille .pdgrid)
// Les surfaces sont calculées une fois, sur un maillage mondial au 1/120°,
// et rangées par tuiles de 0,25° dans un fichier du dossier de la base :
// import, affichage, migration et mise à jour lisent les mêmes valeurs.
#pragma once
#include <atomic>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "VerticalRef.h"

struct sqlite3;

namespace pd {

// Référence d'origine d'une source (et référence d'affichage pour Cd..Ell)
enum class Datum : int {
  Legacy = -1,  // base antérieure : profondeurs au zéro des cartes, non converties
  Cd = 0,       // zéro des cartes local (ZH, CD…)
  Lat = 1,      // plus basse mer astronomique
  Ell = 2,      // ellipsoïde WGS84 (hauteurs GNSS)
  Msl = 3,      // niveau moyen (GEBCO…)
  Free = 4      // non rattachée (lac, retenue) : jamais convertie
};
constexpr int kDatumSurfaces = 3;      // surfaces rangées : Cd, Lat, Ell
constexpr int kDatumPerDeg = 120;      // nœuds par degré (1/120°, ~0,9 km)
constexpr int kDatumTileNodes = 30;    // intervalles par tuile : 0,25°
constexpr int kDatumTileN = kDatumTileNodes + 1;  // nœuds par côté (bords partagés)
inline bool DatumHasSurface(Datum d) { return (int)d >= 0 && (int)d < kDatumSurfaces; }
// Nom court affiché (traduit)
std::string DatumName(Datum d);

// Origine des nœuds d'une surface (bits)
enum DatumOrigin : int { kOrigBathyElli = 1, kOrigFes = 2, kOrigMss = 4, kOrigNone = 8 };
// « ZH BathyElli v2.1 + LAT FES2022b »… (traduit)
std::string DatumOriginText(Datum d, int mask, const std::string& mssModel = std::string());

struct DatumConfig {
  std::vector<std::string> nmFiles, zhFiles;  // BathyElli v2.1 NM/ell et ZH/ell
  std::string fesAtlas;                       // FES2022b .pdtide
  std::string mssGrid;                        // MSS .pdgrid (niveau moyen / WGS84)
};

// Calcul des surfaces depuis les fichiers de référence
class DatumProvider {
 public:
  explicit DatumProvider(const DatumConfig& c);
  ~DatumProvider();
  const DatumConfig& config() const { return cfg_; }
  // la référence peut être calculée hors France (Cd : toujours, au pire 0)
  bool available(Datum k) const;
  const std::string& mssModel();
  // Surface k aux nœuds du maillage mondial couvrant l'emprise (ligne 0 =
  // nord) ; `origin` : bits DatumOrigin par nœud. Cd : nœuds sans référence
  // à 0 (kOrigNone) ; Lat, Ell : NaN hors couverture.
  bool compute(Datum k, double lonMin, double lonMax, double latMin, double latMax, Grid2D& g,
               std::vector<uint8_t>& origin, std::string& err,
               const std::atomic<bool>* cancel = nullptr);

 private:
  bool fesLat(const Grid2D& g, std::vector<float>& lat, std::string& err,
              const std::atomic<bool>* cancel);
  DatumConfig cfg_;
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

// Surfaces rangées dans le dossier de la base (fichier créé à la première écriture)
class DatumStore {
 public:
  struct Tile {
    std::vector<float> v;  // kDatumTileN², ligne 0 = sud
    int origin = 0;        // bits DatumOrigin de la tuile
    std::string model;     // modèle MSS (Ell)
    bool ok() const { return v.size() == (size_t)kDatumTileN * kDatumTileN; }
  };
  ~DatumStore();
  static const char* FileName() { return "PersonalDepth_datum.sqlite"; }
  bool open(const std::string& folder, std::string& err);
  void close();
  static void TileOf(double lon, double lat, int& tx, int& ty);
  static void TileBox(int tx, int ty, double& lonW, double& latS);
  bool get(Datum k, int tx, int ty, Tile& t);
  bool has(Datum k, int tx, int ty);
  bool put(Datum k, int tx, int ty, const Tile& t, std::string& err);
  bool begin(std::string& err);
  bool commit(std::string& err);
  void rollback();
  // S_k au point (m) ; NaN si la tuile manque ; `origin` facultatif
  double at(Datum k, double lon, double lat, int* origin = nullptr);
  // tuiles rangées pour k
  std::vector<std::pair<int, int>> tiles(Datum k);

 private:
  bool ensureFile(std::string& err);
  const Tile* cached(Datum k, int tx, int ty);
  std::string path_;
  sqlite3* db_ = nullptr;
  bool inTx_ = false;
  std::map<std::tuple<int, int, int>, Tile> cache_;
  std::recursive_mutex mx_;
};

}  // namespace pd

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

// Masque terre : trait de côte des fonds de carte d'OpenCPN, pour ne pas
// interpoler sur les îles et la côte. Deux sources, téléchargées par le
// gestionnaire de cartes d'OpenCPN :
//   - OSMSHP : polygones de terre d'OpenStreetMap (osmcoastline), shapefile
//     WGS84 basemap_<qualité>.shp, découpés par carrés de 1° ;
//   - GSHHG : fichiers poly-<qualité>-1.dat d'OpenCPN (cellules de 1°,
//     niveaux terre / lac / île dans un lac / étang / Antarctique).
// La meilleure qualité présente dans le dossier est prise.
#pragma once
#include <cstdint>
#include <cstdio>
#include <map>
#include <string>
#include <utility>
#include <vector>

namespace pd {

class LandMask {
 public:
  ~LandMask();
  // `path` : dossier (OSMSHP ou GSHHG, meilleure qualité) ou fichier .shp / .dat
  bool open(const std::string& path, std::string& err);
  // Fichier retenu pour `path` (dossier ou fichier) et son nom court, sans
  // lecture ; false si aucun trait de côte reconnu
  static bool Find(const std::string& path, std::string& file, std::string& name);
  bool isOpen() const { return kind_ != None; }
  const std::string& name() const { return name_; }  // « OSM basemap_full », « GSHHG f »
  const std::string& file() const { return file_; }
  // Terre (1) ou mer (0) aux centres des 256 × 256 pixels de la tuile Web
  // Mercator (z, x, y), ligne 0 au nord ; false : erreur de lecture
  bool tile(int z, int x, int y, std::vector<uint8_t>& land, std::string& err);
  bool landAt(double lon, double lat);  // (contrôles)

 private:
  struct Ring {
    std::vector<double> x, y;  // lon (-180..180), lat ; fermé implicitement
  };
  struct Poly {
    double x0, x1, y0, y1;     // emprise
    std::vector<Ring> rings;   // pair-impair entre les anneaux
    bool water = false;        // GSHHG : lac / étang (retranché)
  };
  bool loadCell(int cx, int cy, std::string& err);  // cellule de 1° (lon -180..179)
  bool readShpRecord(uint32_t rec, Poly& p, std::string& err);
  enum Kind { None, Shp, Gshhg } kind_ = None;
  std::string name_, file_;
  FILE* f_ = nullptr;
  // OSMSHP : index des enregistrements par cellule de 1°, positions (.shx)
  std::map<std::pair<int, int>, std::vector<uint32_t>> shpIndex_;
  std::vector<uint32_t> shxOff_;
  // GSHHG : entête
  int pasx_ = 1, pasy_ = 1;
  // cellules lues : polygones (dans l'ordre d'application)
  std::map<std::pair<int, int>, std::vector<int>> cellPolys_;
  std::map<uint32_t, int> recPoly_;  // OSMSHP : enregistrement -> polygone (partagé)
  std::vector<Poly> polys_;
};

}  // namespace pd

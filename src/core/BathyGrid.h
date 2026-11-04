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

// Grille de profondeur multi-résolution, construite une fois au chargement.
// La palette n'y intervient jamais : changer de couleurs ne recalcule rien ici.
#pragma once
#include <cmath>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "PointCloud.h"

namespace pd {

enum class GridMode {
  Shoalest,  // plus faible sonde de la cellule (sécurité de navigation)
  Mean,      // moyenne des sondes
  Tin        // plus faible sonde + comblement des trous par TIN (Delaunay)
};

// Grille régulière d'origine dans un système projeté (levé déjà maillé en
// Lambert-93, UTM…). La grille d'affichage est alors rééchantillonnée par
// interpolation bilinéaire de ces nœuds, au lieu de prendre la sonde la plus
// proche : pas d'effet d'escalier dû à la rotation entre les deux grilles.
struct LatticeSource {
  int nx = 0, ny = 0;
  double e0 = 0, n0 = 0, dx = 1, dy = 1;  // nœud (0,0) et pas, unités du système
  std::vector<float> z;                    // NaN = nœud absent
  std::function<bool(double lon, double lat, double& e, double& n)> toSrc;
  float at(int i, int j) const { return z[(size_t)j * nx + i]; }
};

struct GridParams {
  GridMode mode = GridMode::Shoalest;
  double cellMeters = 0;      // 0 = automatique d'après la densité
  double forceDLon = 0, forceDLat = 0;  // pas imposé (rasters)
  int tinMaxEdgeCells = 16;   // ne pas jeter de triangles au-dessus des grands vides
  // 64 M cellules = 256 Mo au niveau 0 ; 16 M en 32 bits (espace d'adresses de 2 Go)
  size_t maxCells = sizeof(void*) >= 8 ? (64u << 20) : (16u << 20);
  const LatticeSource* lattice = nullptr;  // rééchantillonnage d'une grille projetée
};

struct GridLevel {
  int nx = 0, ny = 0;
  double lonW = 0, latN = 0;  // coin nord-ouest ; ligne 0 = nord
  double dlon = 0, dlat = 0;
  std::vector<float> z;        // NaN = pas de donnée
  std::vector<uint8_t> shade;  // ombrage ; 160 = neutre (relief plat)

  float at(int i, int j) const { return z[(size_t)j * nx + i]; }
  bool cellOf(double lon, double lat, int& i, int& j) const {
    double fi = (lon - lonW) / dlon, fj = (latN - lat) / dlat;
    if (fi < 0 || fj < 0 || fi >= nx || fj >= ny) return false;
    i = (int)fi; j = (int)fj; return true;
  }
  double cellLon(int i) const { return lonW + (i + 0.5) * dlon; }
  double cellLat(int j) const { return latN - (j + 0.5) * dlat; }
  double lonE() const { return lonW + nx * dlon; }
  double latS() const { return latN - ny * dlat; }
  float sampleBilinear(double lon, double lat) const;
};

class BathyGrid {
 public:
  std::vector<GridLevel> levels;  // 0 = pleine résolution
  GridMode mode = GridMode::Shoalest;
  float zMin = 0, zMax = 0;
  bool nativeLattice = false;  // fichier détecté comme grille régulière
  size_t filledGaps = 0;       // cellules isolées comblées
  int shadeSmooth = 0;         // adoucissement du relief ombré (cellules du niveau 0)

  bool build(const PointCloud& pc, const GridParams& p,
             const std::vector<uint32_t>* tinTriangles, std::string& err);
  // Exagération verticale, azimut et hauteur du soleil (degrés)
  void computeHillshade(float exaggeration = 3.f, float azimuth = 315.f,
                        float altitude = 45.f);
  // Niveau le plus grossier dont la cellule reste <= maxPx pixels à l'écran
  int levelFor(double pixelsPerDegLon, double maxPx = 1.0) const;
  bool empty() const { return levels.empty(); }
  size_t memoryBytes() const;

 private:
  void buildPyramid();
  void closeGaps(GridLevel& g);
  static void resampleLattice(GridLevel& g, const LatticeSource& src);
  void tinFill(GridLevel& g, const PointCloud* pc,
               const std::vector<uint32_t>* tris, int maxEdge);
};

}  // namespace pd

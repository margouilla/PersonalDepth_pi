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

// Référence verticale : ramène des profondeurs exprimées dans une référence
// terrestre (IGN69, NAVD88, ODN…) au zéro hydrographique (ZH).
//
// Notation : C(lon, lat) = altitude du ZH dans la référence terrestre
//   (ex. Brest ≈ -3,6 m IGN69). Profondeur_ZH = Profondeur_terrestre + C.
// C est obtenu, au choix :
//   - par différence de deux surfaces ellipsoïdales : C = h_ZH - N_ref
//     (France : BathyElli v2.1 « ZH / ell » du Shom et RAF20 de l'IGN) ;
//   - directement depuis une grille d'écart déjà calculée ;
// puis rééchantillonné sur une grille régulière fine couvrant le levé.
#pragma once
#include <string>
#include <vector>

#include "PointCloud.h"

namespace pd {

// Grille régulière lon/lat (ligne 0 = nord), NaN = absent
struct Grid2D {
  int nx = 0, ny = 0;
  double lonW = 0, latN = 0, dlon = 0, dlat = 0;
  std::vector<float> v;
  float at(int i, int j) const { return v[(size_t)j * nx + i]; }
  bool empty() const { return v.empty(); }
  double sample(double lon, double lat) const;  // bilinéaire, NaN hors grille
};

struct GeoPoint { double lon, lat, v; };

// Lecteurs (formats courants des grilles de référence verticale)
bool LoadTacGrid(const std::string& path, Grid2D& g, std::string& err);   // IGN .tac/.mnt (RAF, RAC…)
bool LoadAscGrid(const std::string& path, Grid2D& g, std::string& err);   // ESRI ASCII
bool LoadGtxGrid(const std::string& path, Grid2D& g, std::string& err);   // NOAA/PROJ .gtx
bool LoadAnyGrid(const std::string& path, Grid2D& g, std::string& err);   // selon extension
// Surface en points « lon lat valeur [...] » (BathyElli .glhi, .xyz, .txt)
bool LoadPointSurface(const std::string& path, std::vector<GeoPoint>& pts, std::string& err);

// Surface(s) (points ou grilles) échantillonnée(s) aux nœuds de `target`
// (TIN des points, puis prolongement ~20 km au-delà de leur enveloppe)
bool SampleSurfaces(const std::vector<std::string>& files, Grid2D& target, std::string& err);

struct VerticalRefParams {
  enum class Mode { None, Surfaces, DirectGrid } mode = Mode::None;
  std::vector<std::string> zhFiles;  // surface(s) ZH/ellipsoïde (points ou grille)
  std::string refGrid;               // géoïde(s) : RAC23;RAF20… (« ; », par priorité) ;
                                     // vide : Z en hauteurs ellipsoïdales (GNSS)
  std::string directFile;            // grille ou points de C déjà calculés
  bool directInvert = false;         // le fichier donne -C (ZH positif sous la référence)
  std::string label;                 // libellé affiché
  bool active() const { return mode != Mode::None; }
};

class VerticalCorrection {
 public:
  // Prépare C sur l'emprise [lonMin..lonMax]x[latMin..latMax] (+ marge)
  bool build(const VerticalRefParams& p, double lonMin, double lonMax, double latMin,
             double latMax, std::string& err);
  double at(double lon, double lat) const { return grid_.sample(lon, lat); }  // C (m)
  // Applique Profondeur_ZH = Profondeur + C à chaque sonde ; renvoie le
  // nombre de sondes hors couverture (laissées sans conversion) et C min/max
  size_t apply(PointCloud& pc, float& cMin, float& cMax) const;
  const Grid2D& grid() const { return grid_; }

 private:
  Grid2D grid_;
};

}  // namespace pd

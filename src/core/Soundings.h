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

// Index spatial des sondes + sélection des valeurs à afficher (la plus faible
// sonde par case d'écran, comme sur une carte marine).
#pragma once
#include <cmath>
#include <cstdint>
#include <vector>

#include "BathyGrid.h"
#include "PointCloud.h"

namespace pd {

// Projection Mercator -> pixels écran par transformation affine ajustée sur
// trois points du viewport (exact en Mercator, quelle que soit la rotation).
struct ScreenMapper {
  double a = 1, b = 0, c = 0, d = 0, e = 1, f = 0;  // x = a*lon+b*my+c ; y = d*lon+e*my+f
  static double mercY(double lat) {
    return std::log(std::tan(M_PI / 4 + lat * M_PI / 360.0)) * 180.0 / M_PI;
  }
  // (lon_k, lat_k) -> (x_k, y_k) pour k = 0..2, points non alignés
  bool fit(const double lon[3], const double lat[3], const double x[3], const double y[3]);
  void map(double lon, double lat, float& x, float& y) const {
    double m = mercY(lat);
    x = (float)(a * lon + b * m + c);
    y = (float)(d * lon + e * m + f);
  }
  double pixelsPerDegLon() const { return std::hypot(a, d); }
};

class SoundingIndex {
 public:
  // skip : sondes exclues (valeurs interpolées de la base), facultatif
  void build(const PointCloud& pc, const std::vector<uint8_t>* skip = nullptr);
  size_t estimateInBox(double lonMin, double lonMax, double latMin, double latMax) const;
  // Sonde réelle la plus proche (distance en degrés "équivalents")
  bool nearest(const PointCloud& pc, double lon, double lat, double maxDeg,
               uint32_t& idx) const;
  template <class F>
  void forEachInBox(double lonMin, double lonMax, double latMin, double latMax, F fn) const {
    int bx0, by0, bx1, by1;
    if (!bucketRange(lonMin, lonMax, latMin, latMax, bx0, by0, bx1, by1)) return;
    for (int by = by0; by <= by1; ++by)
      for (int bx = bx0; bx <= bx1; ++bx) {
        size_t b = (size_t)by * nbx_ + bx;
        for (uint32_t k = start_[b]; k < start_[b + 1]; ++k) fn(perm_[k]);
      }
  }
  bool empty() const { return perm_.empty(); }

 private:
  bool bucketRange(double lonMin, double lonMax, double latMin, double latMax,
                   int& bx0, int& by0, int& bx1, int& by1) const;
  double lon0_ = 0, lat0_ = 0, blon_ = 1, blat_ = 1;
  int nbx_ = 0, nby_ = 0;
  std::vector<uint32_t> start_, perm_;
};

struct Label {
  float x, y;     // position écran
  float depth;    // valeur exacte de la sonde
  bool fromGrid;  // vrai si issue d'une cellule de grille (dézoom)
  float w = 0;    // largeur du texte en pixels (renseignée par le rendu)
  bool contour = false;  // valeur d'isobathe (prioritaire sur les sondes)
};

// Supprime les étiquettes qui se chevauchent ; priorité aux valeurs
// d'isobathes, puis à la plus faible profondeur. h = hauteur du texte, pad = marge minimale entre étiquettes.
void Declutter(std::vector<Label>& labels, float h, float pad);

// Choisit au plus une sonde (la plus faible) par case de spacingPx pixels.
// Utilise les sondes réelles si leur nombre dans la vue <= maxScan, sinon les
// cellules du niveau de grille adapté (en mode « plus faible sonde » seulement).
std::vector<Label> SelectSoundings(const PointCloud& pc, const SoundingIndex& idx,
                                   const BathyGrid& grid, const ScreenMapper& sm,
                                   double lonMin, double lonMax, double latMin,
                                   double latMax, int screenW, int screenH,
                                   float spacingPx, size_t maxScan = 4000000);

}  // namespace pd

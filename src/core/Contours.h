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

// Isobathes calculées à la volée sur un niveau de grille (marching squares).
// Aucune donnée n'est modifiée : changer les niveaux ne coûte qu'un recalcul
// des lignes, mis en cache par le rendu.
#pragma once
#include <vector>

#include "BathyGrid.h"

namespace pd {

struct Isoline {
  float level = 0;
  bool closed = false;
  std::vector<double> pts;  // lon0, lat0, lon1, lat1, ...
  size_t size() const { return pts.size() / 2; }
};

// levels : profondeurs (m), triées ou non. Les cellules dont un coin est vide
// sont ignorées (pas de ligne inventée au bord des données).
std::vector<Isoline> ComputeIsolines(const GridLevel& g, const std::vector<float>& levels);

// Lissage géométrique de Chaikin (les extrémités des lignes ouvertes sont fixes)
void SmoothIsoline(Isoline& l, int iterations = 2);

// Liste des niveaux : intervalle régulier entre zMin et zMax (max 400 niveaux)
std::vector<float> RegularLevels(float zMin, float zMax, float step);

}  // namespace pd

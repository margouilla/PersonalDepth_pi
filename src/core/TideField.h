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

// Hauteur d'eau au-dessus du ZH à l'instant affiché : uniforme (station de
// marée) ou maillée (atlas FES). Profondeur affichée = sonde ZH + h(lon, lat).
#pragma once
#include <cmath>
#include <vector>

namespace pd {

struct TideField {
  float h0 = 0.f;                    // valeur uniforme, et de repli hors grille
  int nx = 0, ny = 0;                // grille : nœud (0,0) au sud-ouest
  double lon0 = 0, lat0 = 0, dlon = 0, dlat = 0;
  std::vector<float> v;              // sans NaN (trous comblés à la construction)
  float hMin = 0.f, hMax = 0.f;
  unsigned version = 0;              // change à chaque nouveau champ

  bool gridded() const { return !v.empty(); }
  float at(double lon, double lat) const {
    if (v.empty()) return h0;
    double x = (lon - lon0) / dlon;
    if (x < -0.5) x += 360.0 / dlon;  // grille à cheval sur 0°
    double y = (lat - lat0) / dlat;
    // hors grille : valeur du bord le plus proche
    x = x < 0 ? 0 : (x > nx - 1 ? nx - 1 : x);
    y = y < 0 ? 0 : (y > ny - 1 ? ny - 1 : y);
    int i = (int)x, j = (int)y;
    if (i > nx - 2) i = nx > 1 ? nx - 2 : 0;
    if (j > ny - 2) j = ny > 1 ? ny - 2 : 0;
    const double fx = nx > 1 ? x - i : 0, fy = ny > 1 ? y - j : 0;
    const float* r0 = &v[(size_t)j * nx + i];
    const float* r1 = ny > 1 ? r0 + nx : r0;
    const int dx = nx > 1 ? 1 : 0;
    return (float)((1 - fy) * ((1 - fx) * r0[0] + fx * r0[dx]) +
                   fy * ((1 - fx) * r1[0] + fx * r1[dx]));
  }
  // Complète les nœuds NaN par le voisinage (puis h0) et calcule min/max
  void finish() {
    if (v.empty()) { hMin = hMax = h0; return; }
    bool any = true;
    for (int pass = 0; pass < nx + ny && any; ++pass) {
      any = false;
      std::vector<float> nv = v;
      for (int j = 0; j < ny; ++j)
        for (int i = 0; i < nx; ++i) {
          if (!std::isnan(v[(size_t)j * nx + i])) continue;
          double acc = 0;
          int n = 0;
          for (int dj = -1; dj <= 1; ++dj)
            for (int di = -1; di <= 1; ++di) {
              const int ii = i + di, jj = j + dj;
              if (ii < 0 || jj < 0 || ii >= nx || jj >= ny) continue;
              const float q = v[(size_t)jj * nx + ii];
              if (!std::isnan(q)) { acc += q; ++n; }
            }
          if (n) nv[(size_t)j * nx + i] = (float)(acc / n);
          else any = true;
        }
      v.swap(nv);
    }
    hMin = 1e30f; hMax = -1e30f;
    for (float& q : v) {
      if (std::isnan(q)) q = h0;
      hMin = q < hMin ? q : hMin;
      hMax = q > hMax ? q : hMax;
    }
  }
};

}  // namespace pd

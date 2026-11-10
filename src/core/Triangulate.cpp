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

#include "Triangulate.h"
#include <cstdint>
#include <limits>
#include <cmath>
#include <algorithm>

#include <stdexcept>

#include "delaunator.hpp"

namespace pd {
std::vector<size_t> Triangulate(const std::vector<double>& xy) {
  if (xy.size() < 6) return {};
  try {
    delaunator::Delaunator d(xy);
    return d.triangles;
  } catch (const std::exception&) {  // points tous alignés : aucun triangle
    return {};
  }
}

const char* InterpMethodName(InterpMethod m) {
  switch (m) {
    case InterpMethod::NaturalNeighbour: return "voisin naturel";
    case InterpMethod::Idw: return "IDW";
    default: return "TIN";
  }
}

namespace {
// sommets rangés dans une grille de seaux (recherche des voisins)
struct Buckets {
  double x0 = 0, y0 = 0, h = 1;
  int nx = 0, ny = 0;
  std::vector<int> start, idx;  // seau c : idx[start[c] .. start[c+1])
  void build(const std::vector<double>& xy, double cell) {
    const size_t n = xy.size() / 2;
    double x1 = -1e300, y1 = -1e300;
    x0 = y0 = 1e300;
    for (size_t i = 0; i < n; ++i) {
      x0 = std::min(x0, xy[2 * i]); y0 = std::min(y0, xy[2 * i + 1]);
      x1 = std::max(x1, xy[2 * i]); y1 = std::max(y1, xy[2 * i + 1]);
    }
    h = std::max(cell, 1e-6);
    nx = std::max(1, std::min(4096, (int)((x1 - x0) / h) + 1));
    ny = std::max(1, std::min(4096, (int)((y1 - y0) / h) + 1));
    h = std::max(h, std::max((x1 - x0) / nx, (y1 - y0) / ny) * 1.000001);
    start.assign((size_t)nx * ny + 1, 0);
    std::vector<int> cellOf(n);
    for (size_t i = 0; i < n; ++i) {
      const int c = cellIndex(xy[2 * i], xy[2 * i + 1]);
      cellOf[i] = c;
      ++start[c + 1];
    }
    for (size_t c = 0; c + 1 < start.size(); ++c) start[c + 1] += start[c];
    idx.assign(n, 0);
    std::vector<int> fill(start.begin(), start.end() - 1);
    for (size_t i = 0; i < n; ++i) idx[fill[cellOf[i]]++] = (int)i;
  }
  int cellIndex(double x, double y) const {
    const int i = std::max(0, std::min(nx - 1, (int)((x - x0) / h)));
    const int j = std::max(0, std::min(ny - 1, (int)((y - y0) / h)));
    return j * nx + i;
  }
  // appelle f(indice) pour les sommets des seaux à moins de r de (x, y)
  template <class F>
  void around(double x, double y, double r, F f) const {
    const int i0 = std::max(0, (int)std::floor((x - r - x0) / h)), i1 = std::min(nx - 1, (int)std::floor((x + r - x0) / h));
    const int j0 = std::max(0, (int)std::floor((y - r - y0) / h)), j1 = std::min(ny - 1, (int)std::floor((y + r - y0) / h));
    for (int j = j0; j <= j1; ++j)
      for (int i = i0; i <= i1; ++i) {
        const int c = j * nx + i;
        for (int q = start[c]; q < start[c + 1]; ++q) f(idx[q]);
      }
  }
  // sommet le plus proche à moins de rmax (-1 sinon), distance au carré
  int nearest(const std::vector<double>& xy, double x, double y, double rmax, double& d2) const {
    int best = -1;
    d2 = rmax * rmax;
    for (double r = h; ; r = std::min(rmax, r * 2)) {
      around(x, y, r, [&](int k) {
        const double dx = xy[2 * k] - x, dy = xy[2 * k + 1] - y, e = dx * dx + dy * dy;
        if (e < d2) { d2 = e; best = k; }
      });
      if ((best >= 0 && d2 <= r * r) || r >= rmax) break;
    }
    return best;
  }
};
}  // namespace

void InterpolatePixels(InterpMethod m, const std::vector<double>& xy, const std::vector<double>& val,
                       const std::vector<int>& pix, double maxDist, std::vector<double>& out) {
  const size_t np = pix.size() / 2, nv = xy.size() / 2;
  out.assign(np, std::numeric_limits<double>::quiet_NaN());
  if (!np || nv < 1 || m == InterpMethod::Tin) return;
  Buckets B;
  if (m == InterpMethod::Idw) {
    B.build(xy, std::max(1.0, maxDist / 4));
    const double r2max = maxDist * maxDist;
    std::vector<std::pair<double, int>> near;
    for (size_t p = 0; p < np; ++p) {
      const double x = pix[2 * p] + 0.5, y = pix[2 * p + 1] + 0.5;
      near.clear();
      B.around(x, y, maxDist, [&](int k) {
        const double dx = xy[2 * k] - x, dy = xy[2 * k + 1] - y, d2 = dx * dx + dy * dy;
        if (d2 <= r2max) near.push_back({d2, k});
      });
      if (near.empty()) continue;
      const size_t kk = std::min<size_t>(12, near.size());
      std::partial_sort(near.begin(), near.begin() + kk, near.end());
      if (near[0].first < 1e-12) { out[p] = val[near[0].second]; continue; }
      double sw = 0, sv = 0;
      for (size_t q = 0; q < kk; ++q) {
        const double w = 1.0 / near[q].first;  // 1 / d²
        sw += w;
        sv += w * val[near[q].second];
      }
      out[p] = sv / sw;
    }
    return;
  }
  // voisin naturel, Sibson discret : on parcourt les pixels q autour des pixels
  // demandés (marge = rayon maximal), on cherche leur sommet le plus proche et on
  // « peint » sa valeur dans le disque de rayon r(q) (accumulation, puis moyenne)
  int xmin = INT32_MAX, ymin = INT32_MAX, xmax = INT32_MIN, ymax = INT32_MIN;
  for (size_t p = 0; p < np; ++p) {
    xmin = std::min(xmin, pix[2 * p]); xmax = std::max(xmax, pix[2 * p]);
    ymin = std::min(ymin, pix[2 * p + 1]); ymax = std::max(ymax, pix[2 * p + 1]);
  }
  const int rcap = (int)std::ceil(std::min(maxDist, 32.0));  // rayon plafonné (coût en r²)
  const int W = xmax - xmin + 1, H = ymax - ymin + 1;
  std::vector<int> target((size_t)W * H, -1);  // pixel demandé -> rang dans pix
  for (size_t p = 0; p < np; ++p) target[(size_t)(pix[2 * p + 1] - ymin) * W + (pix[2 * p] - xmin)] = (int)p;
  std::vector<double> acc(np, 0.0);
  std::vector<int> cnt(np, 0);
  B.build(xy, std::max(1.0, (double)rcap / 2));
  for (int qy = ymin - rcap; qy <= ymax + rcap; ++qy)
    for (int qx = xmin - rcap; qx <= xmax + rcap; ++qx) {
      double d2;
      const int s = B.nearest(xy, qx + 0.5, qy + 0.5, rcap + 0.5, d2);
      if (s < 0) continue;
      const double r2 = d2;
      const int r = (int)std::floor(std::sqrt(r2));
      const double v = val[s];
      for (int dy = -r; dy <= r; ++dy) {
        const int py = qy + dy;
        if (py < ymin || py > ymax) continue;
        for (int dx = -r; dx <= r; ++dx) {
          if (dx * dx + dy * dy > r2) continue;
          const int px = qx + dx;
          if (px < xmin || px > xmax) continue;
          const int t = target[(size_t)(py - ymin) * W + (px - xmin)];
          if (t < 0) continue;
          acc[t] += v;
          ++cnt[t];
        }
      }
    }
  for (size_t p = 0; p < np; ++p)
    if (cnt[p]) out[p] = acc[p] / cnt[p];
}
}  // namespace pd

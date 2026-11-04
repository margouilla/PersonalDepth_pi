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

#include "Soundings.h"

#include <algorithm>
#include <limits>

namespace pd {

bool ScreenMapper::fit(const double lon[3], const double lat[3], const double x[3],
                       const double y[3]) {
  double m[3] = {mercY(lat[0]), mercY(lat[1]), mercY(lat[2])};
  double det = lon[0] * (m[1] - m[2]) - m[0] * (lon[1] - lon[2]) + (lon[1] * m[2] - lon[2] * m[1]);
  if (std::fabs(det) < 1e-15) return false;
  auto solve = [&](const double v[3], double& A, double& B, double& C) {
    A = (v[0] * (m[1] - m[2]) - m[0] * (v[1] - v[2]) + (v[1] * m[2] - v[2] * m[1])) / det;
    B = (lon[0] * (v[1] - v[2]) - v[0] * (lon[1] - lon[2]) + (lon[1] * v[2] - lon[2] * v[1])) / det;
    C = (lon[0] * (m[1] * v[2] - m[2] * v[1]) - m[0] * (lon[1] * v[2] - lon[2] * v[1]) +
         v[0] * (lon[1] * m[2] - lon[2] * m[1])) / det;
  };
  solve(x, a, b, c);
  solve(y, d, e, f);
  return true;
}

void SoundingIndex::build(const PointCloud& pc) {
  perm_.clear(); start_.clear();
  if (pc.empty()) return;
  double spanLon = std::max(pc.lonMax - pc.lonMin, 1e-9);
  double spanLat = std::max(pc.latMax - pc.latMin, 1e-9);
  double cosL = std::cos(0.5 * (pc.latMin + pc.latMax) * M_PI / 180.0);
  double nb = std::max(1.0, pc.size() / 32.0);  // ~32 sondes par case
  double aspect = (spanLon * cosL) / spanLat;
  nbx_ = std::clamp((int)std::sqrt(nb * aspect), 1, 65535);
  nby_ = std::clamp((int)(nb / nbx_), 1, 65535);
  lon0_ = pc.lonMin; lat0_ = pc.latMin;
  blon_ = spanLon / nbx_ * (1 + 1e-9); blat_ = spanLat / nby_ * (1 + 1e-9);
  size_t NB = (size_t)nbx_ * nby_;
  std::vector<uint32_t> bucket(pc.size());
  start_.assign(NB + 1, 0);
  for (size_t k = 0; k < pc.size(); ++k) {
    int bx = std::min(nbx_ - 1, (int)((pc.lon(k) - lon0_) / blon_));
    int by = std::min(nby_ - 1, (int)((pc.lat(k) - lat0_) / blat_));
    bucket[k] = (uint32_t)((size_t)by * nbx_ + bx);
    ++start_[bucket[k] + 1];
  }
  for (size_t b = 0; b < NB; ++b) start_[b + 1] += start_[b];
  std::vector<uint32_t> fill(start_.begin(), start_.end() - 1);
  perm_.resize(pc.size());
  for (size_t k = 0; k < pc.size(); ++k) perm_[fill[bucket[k]]++] = (uint32_t)k;
}

bool SoundingIndex::bucketRange(double lonMin, double lonMax, double latMin,
                                double latMax, int& bx0, int& by0, int& bx1,
                                int& by1) const {
  if (perm_.empty()) return false;
  bx0 = (int)std::floor((lonMin - lon0_) / blon_);
  bx1 = (int)std::floor((lonMax - lon0_) / blon_);
  by0 = (int)std::floor((latMin - lat0_) / blat_);
  by1 = (int)std::floor((latMax - lat0_) / blat_);
  if (bx1 < 0 || by1 < 0 || bx0 >= nbx_ || by0 >= nby_) return false;
  bx0 = std::max(bx0, 0); by0 = std::max(by0, 0);
  bx1 = std::min(bx1, nbx_ - 1); by1 = std::min(by1, nby_ - 1);
  return true;
}

size_t SoundingIndex::estimateInBox(double lonMin, double lonMax, double latMin,
                                    double latMax) const {
  int bx0, by0, bx1, by1;
  if (!bucketRange(lonMin, lonMax, latMin, latMax, bx0, by0, bx1, by1)) return 0;
  size_t n = 0;
  for (int by = by0; by <= by1; ++by)
    n += start_[(size_t)by * nbx_ + bx1 + 1] - start_[(size_t)by * nbx_ + bx0];
  return n;
}

bool SoundingIndex::nearest(const PointCloud& pc, double lon, double lat,
                            double maxDeg, uint32_t& idx) const {
  double cosL = std::cos(lat * M_PI / 180.0), best = maxDeg * maxDeg;
  bool found = false;
  forEachInBox(lon - maxDeg / cosL, lon + maxDeg / cosL, lat - maxDeg, lat + maxDeg,
               [&](uint32_t k) {
                 double dx = (pc.lon(k) - lon) * cosL, dy = pc.lat(k) - lat;
                 double d2 = dx * dx + dy * dy;
                 if (d2 < best) { best = d2; idx = k; found = true; }
               });
  return found;
}

void Declutter(std::vector<Label>& v, float h, float pad) {
  if (v.size() < 2) return;
  std::stable_sort(v.begin(), v.end(), [](const Label& a, const Label& b) {
    if (a.contour != b.contour) return a.contour;
    return a.depth < b.depth;
  });
  float maxW = h;
  for (const Label& L : v) maxW = std::max(maxW, L.w);
  const float bw = maxW + 2 * pad, bh = h + 2 * pad;  // une case >= une étiquette
  float x0 = 1e30f, y0 = 1e30f, x1 = -1e30f, y1 = -1e30f;
  for (const Label& L : v) {
    x0 = std::min(x0, L.x); y0 = std::min(y0, L.y);
    x1 = std::max(x1, L.x); y1 = std::max(y1, L.y);
  }
  const int nbx = (int)((x1 - x0) / bw) + 1, nby = (int)((y1 - y0) / bh) + 1;
  std::vector<std::vector<uint32_t>> buckets((size_t)nbx * nby);
  std::vector<Label> kept;
  kept.reserve(v.size());
  for (const Label& L : v) {
    const int bx = (int)((L.x - x0) / bw), by = (int)((L.y - y0) / bh);
    bool clash = false;
    for (int dy = -1; dy <= 1 && !clash; ++dy)
      for (int dx = -1; dx <= 1 && !clash; ++dx) {
        int xx = bx + dx, yy = by + dy;
        if (xx < 0 || yy < 0 || xx >= nbx || yy >= nby) continue;
        for (uint32_t k : buckets[(size_t)yy * nbx + xx]) {
          const Label& K = kept[k];
          if (std::fabs(K.x - L.x) < 0.5f * (K.w + L.w) + pad &&
              std::fabs(K.y - L.y) < h + pad) { clash = true; break; }
        }
      }
    if (clash) continue;
    buckets[(size_t)by * nbx + bx].push_back((uint32_t)kept.size());
    kept.push_back(L);
  }
  v.swap(kept);
}

std::vector<Label> SelectSoundings(const PointCloud& pc, const SoundingIndex& idx,
                                   const BathyGrid& grid, const ScreenMapper& sm,
                                   double lonMin, double lonMax, double latMin,
                                   double latMax, int W, int H, float sp,
                                   size_t maxScan) {
  std::vector<Label> out;
  if (sp < 4) sp = 4;
  const int ncx = (int)(W / sp) + 1, ncy = (int)(H / sp) + 1;
  std::vector<Label> best((size_t)ncx * ncy, Label{0, 0, std::numeric_limits<float>::max(), false});
  auto offer = [&](double lon, double lat, float z, bool fromGrid) {
    float x, y;
    sm.map(lon, lat, x, y);
    if (x < 0 || y < 0 || x >= W || y >= H) return;
    Label& L = best[(size_t)(int)(y / sp) * ncx + (int)(x / sp)];
    if (z < L.depth) L = Label{x, y, z, fromGrid};
  };
  if (!idx.empty() && idx.estimateInBox(lonMin, lonMax, latMin, latMax) <= maxScan) {
    idx.forEachInBox(lonMin, lonMax, latMin, latMax,
                     [&](uint32_t k) { offer(pc.lon(k), pc.lat(k), pc.z[k], false); });
  } else if (!grid.empty() && grid.mode == GridMode::Shoalest) {
    const GridLevel& g = grid.levels[grid.levelFor(sm.pixelsPerDegLon(), sp / 3.0)];
    int i0 = std::max(0, (int)((lonMin - g.lonW) / g.dlon));
    int i1 = std::min(g.nx - 1, (int)((lonMax - g.lonW) / g.dlon));
    int j0 = std::max(0, (int)((g.latN - latMax) / g.dlat));
    int j1 = std::min(g.ny - 1, (int)((g.latN - latMin) / g.dlat));
    for (int j = j0; j <= j1; ++j)
      for (int i = i0; i <= i1; ++i) {
        float z = g.at(i, j);
        if (!std::isnan(z)) offer(g.cellLon(i), g.cellLat(j), z, true);
      }
  }
  for (auto& L : best)
    if (L.depth != std::numeric_limits<float>::max()) out.push_back(L);
  return out;
}

}  // namespace pd

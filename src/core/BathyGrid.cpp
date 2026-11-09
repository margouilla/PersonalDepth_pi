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

#include "BathyGrid.h"
#include "Lang.h"

#include <algorithm>
#include <cmath>
#include <limits>

#include "Triangulate.h"

namespace pd {

static const float NaNf = std::numeric_limits<float>::quiet_NaN();

// Si les coordonnées sont sur un pas régulier (fichier déjà maillé), renvoie
// ce pas en degrés ; 0 sinon.
static double LatticeStep(const std::vector<int32_t>& q) {
  std::vector<int64_t> v(q.begin(), q.end());
  return LatticeStepUnits(v) * PointCloud::kQ;
}
static constexpr double kMPerDegLat = 110540.0, kMPerDegLonEq = 111320.0;

float GridLevel::sampleBilinear(double lon, double lat) const {
  double fx = (lon - lonW) / dlon - 0.5, fy = (latN - lat) / dlat - 0.5;
  int i = (int)std::floor(fx), j = (int)std::floor(fy);
  double tx = fx - i, ty = fy - j;
  double acc = 0, w = 0;
  for (int dj = 0; dj <= 1; ++dj)
    for (int di = 0; di <= 1; ++di) {
      int ii = i + di, jj = j + dj;
      if (ii < 0 || jj < 0 || ii >= nx || jj >= ny) continue;
      float v = at(ii, jj);
      if (std::isnan(v)) continue;
      double ww = (di ? tx : 1 - tx) * (dj ? ty : 1 - ty);
      acc += ww * v; w += ww;
    }
  return w > 1e-9 ? (float)(acc / w) : NaNf;
}

size_t BathyGrid::memoryBytes() const {
  size_t s = 0;
  for (auto& l : levels) s += l.z.size() * 4 + l.shade.size();
  return s;
}

// Interpolation bilinéaire de la grille source au centre de chaque cellule.
// La projection est calculée exactement tous les 8 cellules et interpolée
// linéairement entre deux (erreur négligeable à cette échelle).
void BathyGrid::resampleLattice(GridLevel& g, const LatticeSource& S) {
  const int step = 8;
  std::vector<double> E(g.nx), Nn(g.nx);
  for (int j = 0; j < g.ny; ++j) {
    const double lat = g.cellLat(j);
    int prev = -1;
    double pe = 0, pn = 0;
    for (int i = 0; i < g.nx; i += step) {
      int ie = std::min(i + step, g.nx - 1);
      double e0, n0, e1, n1;
      if (prev != i) {
        if (!S.toSrc(g.cellLon(i), lat, e0, n0)) { e0 = n0 = NAN; }
      } else { e0 = pe; n0 = pn; }
      if (!S.toSrc(g.cellLon(ie), lat, e1, n1)) { e1 = n1 = NAN; }
      for (int k = i; k <= ie; ++k) {
        const double t = ie > i ? double(k - i) / (ie - i) : 0;
        E[k] = e0 + (e1 - e0) * t;
        Nn[k] = n0 + (n1 - n0) * t;
      }
      prev = ie; pe = e1; pn = n1;
    }
    for (int i = 0; i < g.nx; ++i) {
      if (std::isnan(E[i])) continue;
      const double fx = (E[i] - S.e0) / S.dx, fy = (Nn[i] - S.n0) / S.dy;
      const int ni = (int)std::lround(fx), nj = (int)std::lround(fy);
      // nœud le plus proche absent : hors des données, on n'invente rien
      if (ni < 0 || nj < 0 || ni >= S.nx || nj >= S.ny || std::isnan(S.at(ni, nj))) continue;
      const int i0 = (int)std::floor(fx), j0 = (int)std::floor(fy);
      const double tx = fx - i0, ty = fy - j0;
      double acc = 0, w = 0;
      for (int dj = 0; dj <= 1; ++dj)
        for (int di = 0; di <= 1; ++di) {
          const int ii = i0 + di, jj = j0 + dj;
          if (ii < 0 || jj < 0 || ii >= S.nx || jj >= S.ny) continue;
          const float v = S.at(ii, jj);
          if (std::isnan(v)) continue;
          const double ww = (di ? tx : 1 - tx) * (dj ? ty : 1 - ty);
          acc += ww * v; w += ww;
        }
      if (w > 1e-12) g.z[(size_t)j * g.nx + i] = (float)(acc / w);
    }
  }
}

// Comble les cellules vides isolées encadrées par deux cellules pleines
// (repliement entre l'espacement des sondes et la taille de cellule).
void BathyGrid::closeGaps(GridLevel& g) {
  const bool mean = (mode == GridMode::Mean);
  std::vector<float> src(g.z);
  auto at = [&](int i, int j) {
    return (i < 0 || j < 0 || i >= g.nx || j >= g.ny) ? NaNf : src[(size_t)j * g.nx + i];
  };
  for (int j = 0; j < g.ny; ++j)
    for (int i = 0; i < g.nx; ++i) {
      size_t c = (size_t)j * g.nx + i;
      if (!std::isnan(src[c])) continue;
      float l = at(i - 1, j), r = at(i + 1, j), u = at(i, j - 1), d = at(i, j + 1);
      float v = NaNf;
      auto pair = [&](float a, float b) {
        if (std::isnan(a) || std::isnan(b)) return;
        float x = mean ? 0.5f * (a + b) : std::min(a, b);
        if (std::isnan(v) || x < v) v = x;
      };
      pair(l, r);
      pair(u, d);
      if (!std::isnan(v)) { g.z[c] = v; ++filledGaps; }
    }
}

bool BathyGrid::build(const PointCloud& pc, const GridParams& p,
                      const std::vector<uint32_t>* tris, std::string& err,
                      const std::vector<uint8_t>* pinterp) {
  if (pinterp && (pinterp->size() != pc.size() ||
                  std::find(pinterp->begin(), pinterp->end(), 1) == pinterp->end()))
    pinterp = nullptr;  // aucune sonde interpolée
  levels.clear();
  nativeLattice = false;
  filledGaps = 0;
  mode = p.mode;
  if (pc.empty()) { err = pd::tr("Nuage vide"); return false; }
  const double midLat = 0.5 * (pc.latMin + pc.latMax);
  const double cosL = std::max(0.01, std::cos(midLat * M_PI / 180.0));
  const double spanLon = std::max(pc.lonMax - pc.lonMin, 1e-9);
  const double spanLat = std::max(pc.latMax - pc.latMin, 1e-9);

  double dlon, dlat;
  double latStep = 0, lonStep = 0;
  if (p.cellMeters <= 0 && !(p.forceDLon > 0 && p.forceDLat > 0)) {
    latStep = LatticeStep(pc.qlat);
    if (latStep > 0) lonStep = LatticeStep(pc.qlon);
  }
  if (lonStep > 0 && latStep > 0) {  // fichier XYZ déjà maillé : pas natif
    dlon = lonStep; dlat = latStep;
    nativeLattice = true;
  } else if (p.forceDLon > 0 && p.forceDLat > 0) {
    dlon = p.forceDLon; dlat = p.forceDLat;
  } else {
    double cell = p.cellMeters;
    if (cell <= 0) {  // densité moyenne : une sonde par cellule en moyenne
      double area = spanLon * kMPerDegLonEq * cosL * spanLat * kMPerDegLat;
      cell = std::max(0.25, std::sqrt(area / (double)pc.size()));
      double mag = std::pow(10.0, std::floor(std::log10(cell)));  // arrondi 1/2/5
      double r = cell / mag;
      cell = mag * (r < 1.5 ? 1 : r < 3.5 ? 2 : r < 7.5 ? 5 : 10);
    }
    dlat = cell / kMPerDegLat;
    dlon = cell / (kMPerDegLonEq * cosL);
  }
  // Limite mémoire : on élargit le pas si nécessaire
  for (;;) {
    double nx = spanLon / dlon + 1, ny = spanLat / dlat + 1;
    if (nx * ny <= (double)p.maxCells) break;
    dlon *= 1.25; dlat *= 1.25;
  }

  GridLevel g;
  g.dlon = dlon; g.dlat = dlat;
  g.lonW = pc.lonMin - dlon * 0.5;
  g.latN = pc.latMax + dlat * 0.5;
  g.nx = (int)std::floor(spanLon / dlon + 1e-6) + 1;  // tolérance pas natif
  g.ny = (int)std::floor(spanLat / dlat + 1e-6) + 1;
  const size_t N = (size_t)g.nx * g.ny;
  g.z.assign(N, NaNf);

  if (p.lattice && p.mode != GridMode::Tin) {
    resampleLattice(g, *p.lattice);
  } else if (p.mode == GridMode::Mean) {
    std::vector<double> sum(N, 0.0);
    std::vector<uint32_t> cnt(N, 0);
    for (size_t k = 0; k < pc.size(); ++k) {
      int i, j;
      if (!g.cellOf(pc.lon(k), pc.lat(k), i, j)) continue;
      size_t c = (size_t)j * g.nx + i;
      sum[c] += pc.z[k]; ++cnt[c];
    }
    for (size_t c = 0; c < N; ++c) if (cnt[c]) g.z[c] = (float)(sum[c] / cnt[c]);
  } else if (pinterp) {  // mesuré avant interpolé, puis plus faible
    g.interp.assign(N, 0);
    for (size_t k = 0; k < pc.size(); ++k) {
      int i, j;
      if (!g.cellOf(pc.lon(k), pc.lat(k), i, j)) continue;
      const size_t c = (size_t)j * g.nx + i;
      const uint8_t pi = (*pinterp)[k];
      if (std::isnan(g.z[c]) || (g.interp[c] && !pi) || (g.interp[c] == pi && pc.z[k] < g.z[c])) {
        g.z[c] = pc.z[k];
        g.interp[c] = pi;
      }
    }
  } else {
    for (size_t k = 0; k < pc.size(); ++k) {
      int i, j;
      if (!g.cellOf(pc.lon(k), pc.lat(k), i, j)) continue;
      float& c = g.z[(size_t)j * g.nx + i];
      if (std::isnan(c) || pc.z[k] < c) c = pc.z[k];
    }
  }
  // Nuages irréguliers seulement : un raster ou une grille native garde ses NODATA
  const bool scattered = !nativeLattice && !(p.forceDLon > 0 && p.forceDLat > 0) &&
                         !(p.lattice && p.mode != GridMode::Tin);
  if (scattered && p.mode != GridMode::Tin && !(tris && !tris->empty())) closeGaps(g);
  if (p.mode == GridMode::Tin || (tris && !tris->empty()))
    tinFill(g, &pc, tris, p.tinMaxEdgeCells);

  zMin = pc.zMin; zMax = pc.zMax;
  levels.push_back(std::move(g));
  buildPyramid();
  return true;
}

// Rastérise un triangle (coordonnées en cellules) dans les cellules vides
static void rasterTri(GridLevel& g, const std::vector<uint8_t>& orig,
                      double x0, double y0, float z0, double x1, double y1,
                      float z1, double x2, double y2, float z2) {
  double den = (y1 - y2) * (x0 - x2) + (x2 - x1) * (y0 - y2);
  if (std::fabs(den) < 1e-12) return;
  int i0 = std::max(0, (int)std::floor(std::min({x0, x1, x2})));
  int i1 = std::min(g.nx - 1, (int)std::ceil(std::max({x0, x1, x2})));
  int j0 = std::max(0, (int)std::floor(std::min({y0, y1, y2})));
  int j1 = std::min(g.ny - 1, (int)std::ceil(std::max({y0, y1, y2})));
  for (int j = j0; j <= j1; ++j)
    for (int i = i0; i <= i1; ++i) {
      size_t c = (size_t)j * g.nx + i;
      if (orig[c]) continue;
      double px = i + 0.5, py = j + 0.5;
      double a = ((y1 - y2) * (px - x2) + (x2 - x1) * (py - y2)) / den;
      double b = ((y2 - y0) * (px - x2) + (x0 - x2) * (py - y2)) / den;
      double cc = 1 - a - b;
      if (a < -1e-9 || b < -1e-9 || cc < -1e-9) continue;
      float v = (float)(a * z0 + b * z1 + cc * z2);
      if (std::isnan(g.z[c]) || v < g.z[c]) g.z[c] = v;
    }
}

void BathyGrid::tinFill(GridLevel& g, const PointCloud* pc,
                        const std::vector<uint32_t>* tris, int maxEdge) {
  std::vector<uint8_t> orig(g.z.size());
  for (size_t c = 0; c < g.z.size(); ++c) orig[c] = !std::isnan(g.z[c]);

  if (tris && !tris->empty() && pc) {  // TIN fourni (LandXML)
    auto cx = [&](uint32_t k) { return (pc->lon(k) - g.lonW) / g.dlon; };
    auto cy = [&](uint32_t k) { return (g.latN - pc->lat(k)) / g.dlat; };
    for (size_t t = 0; t + 2 < tris->size(); t += 3) {
      uint32_t a = (*tris)[t], b = (*tris)[t + 1], c = (*tris)[t + 2];
      rasterTri(g, orig, cx(a), cy(a), pc->z[a], cx(b), cy(b), pc->z[b], cx(c),
                cy(c), pc->z[c]);
    }
    return;
  }
  // TIN calculé : seules les cellules pleines bordant un vide comptent
  std::vector<double> coords;
  std::vector<float> vals;
  for (int j = 0; j < g.ny; ++j)
    for (int i = 0; i < g.nx; ++i) {
      if (!orig[(size_t)j * g.nx + i]) continue;
      bool border = false;
      for (int dj = -1; dj <= 1 && !border; ++dj)
        for (int di = -1; di <= 1; ++di) {
          int ii = i + di, jj = j + dj;
          if (ii < 0 || jj < 0 || ii >= g.nx || jj >= g.ny) continue;
          if (!orig[(size_t)jj * g.nx + ii]) { border = true; break; }
        }
      if (!border) continue;
      coords.push_back(i + 0.5); coords.push_back(j + 0.5);
      vals.push_back(g.at(i, j));
    }
  if (vals.size() < 3) return;
  const std::vector<size_t> tris_ = Triangulate(coords);
  const double m2 = (double)maxEdge * maxEdge;
  for (size_t t = 0; t + 2 < tris_.size(); t += 3) {
    size_t a = tris_[t], b = tris_[t + 1], c = tris_[t + 2];
    double ax = coords[2 * a], ay = coords[2 * a + 1], bx = coords[2 * b],
           by = coords[2 * b + 1], cx = coords[2 * c], cy = coords[2 * c + 1];
    auto e2 = [](double x0, double y0, double x1, double y1) {
      return (x0 - x1) * (x0 - x1) + (y0 - y1) * (y0 - y1);
    };
    if (e2(ax, ay, bx, by) > m2 || e2(bx, by, cx, cy) > m2 || e2(cx, cy, ax, ay) > m2)
      continue;
    rasterTri(g, orig, ax, ay, vals[a], bx, by, vals[b], cx, cy, vals[c]);
  }
}

void BathyGrid::buildPyramid() {
  const bool mean = (mode == GridMode::Mean);
  while (levels.back().nx > 256 || levels.back().ny > 256) {
    const GridLevel& s = levels.back();
    GridLevel d;
    d.nx = (s.nx + 1) / 2; d.ny = (s.ny + 1) / 2;
    d.dlon = s.dlon * 2; d.dlat = s.dlat * 2;
    d.lonW = s.lonW; d.latN = s.latN;
    d.z.assign((size_t)d.nx * d.ny, NaNf);
    const bool fl = !s.interp.empty();
    if (fl) d.interp.assign(d.z.size(), 0);
    for (int j = 0; j < d.ny; ++j)
      for (int i = 0; i < d.nx; ++i) {
        float acc = mean ? 0.f : NaNf; int n = 0;
        uint8_t ai = 1;  // interpolée seulement si aucune cellule mesurée
        for (int dj = 0; dj < 2; ++dj)
          for (int di = 0; di < 2; ++di) {
            int ii = 2 * i + di, jj = 2 * j + dj;
            if (ii >= s.nx || jj >= s.ny) continue;
            float v = s.at(ii, jj);
            if (std::isnan(v)) continue;
            const uint8_t vi = fl ? s.interp[(size_t)jj * s.nx + ii] : 0;
            if (mean) acc += v;
            else if (std::isnan(acc) || (ai && !vi) || (ai == vi && v < acc)) { acc = v; ai = vi; }
            ++n;
          }
        if (n) {
          d.z[(size_t)j * d.nx + i] = mean ? acc / n : acc;
          if (fl && !mean) d.interp[(size_t)j * d.nx + i] = ai;
        }
      }
    levels.push_back(std::move(d));
  }
}

void BathyGrid::computeHillshade(float exag, float azimuth, float altitude) {
  const double zen = (90.0 - altitude) * M_PI / 180.0;
  double azm = 360.0 - azimuth + 90.0;
  if (azm >= 360.0) azm -= 360.0;
  azm *= M_PI / 180.0;
  const double cz = std::cos(zen), sz = std::sin(zen);
  for (size_t lev = 0; lev < levels.size(); ++lev) {
    GridLevel& g = levels[lev];
    g.shade.assign(g.z.size(), 160);
    // Surface adoucie pour l'ombrage seulement (moyenne glissante en deux
    // passes, en ignorant les cellules vides) ; les profondeurs ne changent pas.
    const int r = (int)std::lround(shadeSmooth / std::pow(2.0, (double)lev));
    std::vector<float> zs;
    const std::vector<float>* zsrc = &g.z;
    if (r > 0) {
      zs = g.z;
      std::vector<float> tmp(zs.size());
      for (int pass = 0; pass < 2; ++pass)
        for (int dir = 0; dir < 2; ++dir) {
          const int n1 = dir == 0 ? g.ny : g.nx, n2 = dir == 0 ? g.nx : g.ny;
          for (int a = 0; a < n1; ++a) {
            auto idx = [&](int b) {
              return dir == 0 ? (size_t)a * g.nx + b : (size_t)b * g.nx + a;
            };
            double acc = 0; int cnt = 0;
            for (int b = -r; b < n2 + r; ++b) {  // fenêtre glissante [b-r, b+r]
              const int in = b + r, out = b - r - 1;
              if (in >= 0 && in < n2 && !std::isnan(zs[idx(in)])) { acc += zs[idx(in)]; ++cnt; }
              if (out >= 0 && out < n2 && !std::isnan(zs[idx(out)])) { acc -= zs[idx(out)]; --cnt; }
              if (b >= 0 && b < n2)
                tmp[idx(b)] = std::isnan(zs[idx(b)]) || cnt == 0 ? zs[idx(b)] : (float)(acc / cnt);
            }
          }
          zs.swap(tmp);
        }
      zsrc = &zs;
    }
    const std::vector<float>& Z = *zsrc;
    double lat = 0.5 * (g.latN + g.latS());
    double dx = g.dlon * kMPerDegLonEq * std::cos(lat * M_PI / 180.0);
    double dy = g.dlat * kMPerDegLat;
    for (int j = 0; j < g.ny; ++j)
      for (int i = 0; i < g.nx; ++i) {
        float e = g.at(i, j);
        if (std::isnan(e)) continue;
        e = Z[(size_t)j * g.nx + i];
        auto E = [&](int di, int dj) {
          int ii = std::clamp(i + di, 0, g.nx - 1), jj = std::clamp(j + dj, 0, g.ny - 1);
          float v = Z[(size_t)jj * g.nx + ii];
          return -(double)(std::isnan(v) ? e : v) * exag;  // altitude = -profondeur
        };
        double a = E(-1, -1), b = E(0, -1), c = E(1, -1), d = E(-1, 0),
               f = E(1, 0), gg = E(-1, 1), h = E(0, 1), ii = E(1, 1);
        double dzdx = ((c + 2 * f + ii) - (a + 2 * d + gg)) / (8 * dx);
        double dzdy = ((gg + 2 * h + ii) - (a + 2 * b + c)) / (8 * dy);
        double slope = std::atan(std::sqrt(dzdx * dzdx + dzdy * dzdy));
        double aspect = std::atan2(dzdy, -dzdx);
        double hs = cz * std::cos(slope) + sz * std::sin(slope) * std::cos(azm - aspect);
        double s = std::clamp(hs / cz, 0.0, 1.59);  // 1 = plat
        g.shade[(size_t)j * g.nx + i] = (uint8_t)std::lround(s * 160.0);
      }
  }
}

int BathyGrid::levelFor(double ppdLon, double maxPx) const {
  int k = 0;
  while (k + 1 < (int)levels.size() && levels[k + 1].dlon * ppdLon <= maxPx) ++k;
  return k;
}

}  // namespace pd

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

#include "Contours.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <deque>
#include <unordered_map>

namespace pd {

namespace {

struct Seg {
  uint64_t ea, eb;    // identifiants d'arêtes de grille des deux extrémités
  float xa, ya, xb, yb;  // positions en coordonnées de cellule
};

// Côtés d'un carré : 0 haut, 1 droite, 2 bas, 3 gauche
const int8_t kCases[16][4] = {
    {-1, -1, -1, -1}, {3, 2, -1, -1}, {2, 1, -1, -1}, {3, 1, -1, -1},
    {0, 1, -1, -1},   {-2, -2, -2, -2}, {0, 2, -1, -1}, {3, 0, -1, -1},
    {3, 0, -1, -1},   {0, 2, -1, -1}, {-2, -2, -2, -2}, {0, 1, -1, -1},
    {3, 1, -1, -1},   {2, 1, -1, -1}, {3, 2, -1, -1},  {-1, -1, -1, -1}};

}  // namespace

std::vector<float> RegularLevels(float zMin, float zMax, float step) {
  std::vector<float> v;
  if (!(step > 0) || !(zMax > zMin)) return v;
  double first = std::ceil(zMin / step) * step;
  for (double z = first; z <= zMax + 1e-6 && v.size() < 400; z += step) {
    float r = (float)(std::round(z * 1000.0) / 1000.0);
    if (r > zMin) v.push_back(r);
  }
  return v;
}

std::vector<Isoline> ComputeIsolines(const GridLevel& g, const std::vector<float>& lvIn) {
  std::vector<Isoline> out;
  if (g.nx < 2 || g.ny < 2 || lvIn.empty()) return out;
  std::vector<float> lv(lvIn);
  std::sort(lv.begin(), lv.end());
  lv.erase(std::unique(lv.begin(), lv.end()), lv.end());
  std::vector<std::vector<Seg>> segs(lv.size());

  const int nx = g.nx;
  auto H = [nx](int i, int j) { return (uint64_t)2 * ((uint64_t)j * nx + i); };
  auto V = [nx](int i, int j) { return (uint64_t)2 * ((uint64_t)j * nx + i) + 1; };

  for (int j = 0; j + 1 < g.ny; ++j)
    for (int i = 0; i + 1 < g.nx; ++i) {
      const float a = g.at(i, j), b = g.at(i + 1, j), c = g.at(i + 1, j + 1),
                  d = g.at(i, j + 1);
      if (std::isnan(a) || std::isnan(b) || std::isnan(c) || std::isnan(d)) continue;
      const float mn = std::min(std::min(a, b), std::min(c, d));
      const float mx = std::max(std::max(a, b), std::max(c, d));
      if (!(mx > mn)) continue;
      // niveaux L tels que mn < L <= mx
      auto it = std::upper_bound(lv.begin(), lv.end(), mn);
      for (; it != lv.end() && *it <= mx; ++it) {
        const float L = *it;
        const int k = (int)(a >= L) << 3 | (int)(b >= L) << 2 | (int)(c >= L) << 1 | (int)(d >= L);
        // point et identifiant d'arête pour chaque côté
        auto side = [&](int sd, float& x, float& y, uint64_t& e) {
          auto t = [L](float p, float q) { return (q == p) ? 0.5f : (L - p) / (q - p); };
          switch (sd) {
            case 0: x = i + t(a, b); y = (float)j; e = H(i, j); break;
            case 1: x = (float)(i + 1); y = j + t(b, c); e = V(i + 1, j); break;
            case 2: x = i + t(d, c); y = (float)(j + 1); e = H(i, j + 1); break;
            default: x = (float)i; y = j + t(a, d); e = V(i, j); break;
          }
        };
        int pairs[2][2];
        int np = 0;
        if (k == 5 || k == 10) {  // col : on tranche par la valeur centrale
          const bool centerDeep = 0.25f * (a + b + c + d) >= L;
          const bool isoAC = (k == 5) == centerDeep;  // coins a et c isolés
          if (isoAC) { pairs[0][0] = 3; pairs[0][1] = 0; pairs[1][0] = 2; pairs[1][1] = 1; }
          else { pairs[0][0] = 0; pairs[0][1] = 1; pairs[1][0] = 3; pairs[1][1] = 2; }
          np = 2;
        } else if (kCases[k][0] >= 0) {
          pairs[0][0] = kCases[k][0]; pairs[0][1] = kCases[k][1];
          np = 1;
        }
        for (int p = 0; p < np; ++p) {
          Seg s;
          side(pairs[p][0], s.xa, s.ya, s.ea);
          side(pairs[p][1], s.xb, s.yb, s.eb);
          segs[it - lv.begin()].push_back(s);
        }
      }
    }

  // Assemblage des segments en polylignes (deux segments d'un même niveau
  // partagent une extrémité si et seulement s'ils partagent une arête)
  auto toLonLat = [&g](float x, float y, std::vector<double>& v) {
    v.push_back(g.lonW + (x + 0.5) * g.dlon);
    v.push_back(g.latN - (y + 0.5) * g.dlat);
  };
  for (size_t li = 0; li < lv.size(); ++li) {
    const auto& S = segs[li];
    if (S.empty()) continue;
    std::unordered_map<uint64_t, std::pair<int, int>> byEdge;
    byEdge.reserve(S.size() * 2);
    auto addEdge = [&byEdge](uint64_t e, int s) {
      auto r = byEdge.emplace(e, std::make_pair(s, -1));
      if (!r.second) r.first->second.second = s;
    };
    for (int s = 0; s < (int)S.size(); ++s) { addEdge(S[s].ea, s); addEdge(S[s].eb, s); }
    std::vector<uint8_t> used(S.size(), 0);
    for (int s0 = 0; s0 < (int)S.size(); ++s0) {
      if (used[s0]) continue;
      used[s0] = 1;
      std::deque<std::pair<float, float>> line;
      line.emplace_back(S[s0].xa, S[s0].ya);
      line.emplace_back(S[s0].xb, S[s0].yb);
      bool closed = false;
      // dans les deux sens à partir du premier segment
      for (int dir = 0; dir < 2 && !closed; ++dir) {
        uint64_t e = dir == 0 ? S[s0].eb : S[s0].ea;
        for (;;) {
          auto f = byEdge.find(e);
          if (f == byEdge.end()) break;
          int n = f->second.first;
          if (n >= 0 && used[n]) n = f->second.second;
          if (n < 0 || used[n]) {
            if (dir == 0 && (e == S[s0].ea)) closed = true;
            break;
          }
          used[n] = 1;
          const Seg& q = S[n];
          const bool fromA = (q.ea == e);
          const float x = fromA ? q.xb : q.xa, y = fromA ? q.yb : q.ya;
          if (dir == 0) line.emplace_back(x, y); else line.emplace_front(x, y);
          e = fromA ? q.eb : q.ea;
          if (dir == 0 && e == S[s0].ea) {  // boucle refermée
            line.emplace_back(S[s0].xa, S[s0].ya);
            closed = true;
            break;
          }
        }
      }
      Isoline il;
      il.level = lv[li];
      il.closed = closed;
      il.pts.reserve(line.size() * 2);
      for (auto& p : line) toLonLat(p.first, p.second, il.pts);
      out.push_back(std::move(il));
    }
  }
  return out;
}

void SmoothIsoline(Isoline& l, int iterations) {
  for (int it = 0; it < iterations; ++it) {
    const size_t n = l.size();
    if (n < 3) return;
    std::vector<double> o;
    o.reserve(n * 4);
    auto P = [&l](size_t k, int c) { return l.pts[2 * k + c]; };
    if (!l.closed) { o.push_back(P(0, 0)); o.push_back(P(0, 1)); }
    const size_t last = l.closed ? n - 1 : n - 1;  // boucle : dernier point = premier
    for (size_t k = 0; k < last; ++k) {
      for (int c = 0; c < 2; ++c) o.push_back(0.75 * P(k, c) + 0.25 * P(k + 1, c));
      for (int c = 0; c < 2; ++c) o.push_back(0.25 * P(k, c) + 0.75 * P(k + 1, c));
    }
    if (l.closed) { o.push_back(o[0]); o.push_back(o[1]); }
    else { o.push_back(P(n - 1, 0)); o.push_back(P(n - 1, 1)); }
    l.pts.swap(o);
  }
}

}  // namespace pd

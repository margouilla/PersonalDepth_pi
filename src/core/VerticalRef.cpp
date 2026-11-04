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

#include "VerticalRef.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <sstream>

#include "Triangulate.h"

namespace pd {

namespace {
std::string lowerExt(const std::string& p) {
  size_t d = p.find_last_of('.');
  std::string e = d == std::string::npos ? "" : p.substr(d);
  std::transform(e.begin(), e.end(), e.begin(), ::tolower);
  return e;
}
bool isGridExt(const std::string& e) {
  return e == ".tac" || e == ".mnt" || e == ".asc" || e == ".gtx";
}
}  // namespace

double Grid2D::sample(double lon, double lat) const {
  if (v.empty()) return NAN;
  const double fx = (lon - lonW) / dlon, fy = (latN - lat) / dlat;
  if (fx < -1e-9 || fy < -1e-9 || fx > nx - 1 + 1e-9 || fy > ny - 1 + 1e-9) return NAN;
  int i = std::min((int)std::floor(fx), nx - 2), j = std::min((int)std::floor(fy), ny - 2);
  i = std::max(i, 0); j = std::max(j, 0);
  const double tx = fx - i, ty = fy - j;
  double acc = 0, w = 0;
  for (int dj = 0; dj <= 1; ++dj)
    for (int di = 0; di <= 1; ++di) {
      const float x = at(std::min(i + di, nx - 1), std::min(j + dj, ny - 1));
      if (std::isnan(x)) continue;
      const double ww = (di ? tx : 1 - tx) * (dj ? ty : 1 - ty);
      acc += ww * x; w += ww;
    }
  return w > 1e-9 ? acc / w : NAN;
}

// ------------------------------------------------------------- lecteurs
// Format IGN (.tac) : « lonmin lonmax latmin latmax dlon dlat … libellé »
// puis les valeurs, longitude la plus rapide, du nord vers le sud ; chaque
// nœud peut porter des valeurs annexes (code de précision) qu'on ignore.
// Grilles IGN (.tac, .mnt) — entête :
//   lonmin lonmax latmin latmax dlon dlat ordre coord nval prec trans... [texte]
// ordre 1 : latitude croissante d'abord (depuis le sud-ouest), puis longitude
//       2 : longitude croissante d'abord (depuis le nord-ouest), latitudes décroissantes
//       3 : latitude décroissante d'abord (depuis le nord-ouest), puis longitude
//       4 : longitude croissante d'abord (depuis le sud-ouest), latitudes croissantes
// coord = 1 : lon lat écrits avant les valeurs de chaque nœud ; prec = 1 : un
// code de précision suit les valeurs. Seule la première valeur est retenue.
bool LoadTacGrid(const std::string& path, Grid2D& g, std::string& err) {
  std::ifstream in(path);
  if (!in) { err = "Impossible d'ouvrir " + path; return false; }
  std::string head;
  std::getline(in, head);
  std::istringstream hs(head);
  double h[16];
  int nh = 0;
  while (nh < 16 && hs >> h[nh]) ++nh;  // s'arrête au texte libre
  if (nh < 6 || h[4] <= 0 || h[5] <= 0) {
    err = "Entête de grille IGN illisible : " + path;
    return false;
  }
  const double lo0 = h[0], lo1 = h[1], la0 = h[2], la1 = h[3], dlo = h[4], dla = h[5];
  const bool full = nh >= 10;
  const int order = full ? (int)h[6] : 2;
  const bool coords = full && h[7] != 0;
  const int nval = full ? std::max(1, (int)h[8]) : 0;
  const bool prec = full && h[9] != 0;
  const double trans = (full && nh > 10) ? h[10] : 0.0;
  if (order < 1 || order > 4) { err = "Ordre de grille IGN inconnu dans " + path; return false; }
  g.nx = (int)std::lround((lo1 - lo0) / dlo) + 1;
  g.ny = (int)std::lround((la1 - la0) / dla) + 1;
  g.lonW = lo0; g.latN = la1; g.dlon = dlo; g.dlat = dla;
  const size_t n = (size_t)g.nx * g.ny;
  std::vector<double> vals;
  vals.reserve(n * 2);
  double x;
  while (in >> x) vals.push_back(x);
  size_t stride = full ? (coords ? 2 : 0) + nval + (prec ? 1 : 0) : 0;
  if (!full) {  // entête court : nombre de valeurs par nœud déduit
    if (vals.size() < n || vals.size() % n) stride = 0;
    else stride = vals.size() / n;
  }
  if (!stride || vals.size() < n * stride) {
    err = "Nombre de valeurs incohérent avec l'entête : " + path;
    return false;
  }
  g.v.assign(n, NAN);
  const size_t first = coords ? 2 : 0;
  for (size_t s = 0; s < n; ++s) {
    const double* r = &vals[s * stride];
    long i, j;  // j : ligne depuis le nord
    if (coords) {
      i = std::lround((r[0] - lo0) / dlo);
      j = std::lround((la1 - r[1]) / dla);
    } else {
      switch (order) {
        case 1: i = (long)(s / g.ny); j = g.ny - 1 - (long)(s % g.ny); break;
        case 2: i = (long)(s % g.nx); j = (long)(s / g.nx); break;
        case 3: i = (long)(s / g.ny); j = (long)(s % g.ny); break;
        default: i = (long)(s % g.nx); j = g.ny - 1 - (long)(s / g.nx); break;
      }
    }
    if (i < 0 || j < 0 || i >= g.nx || j >= g.ny) continue;
    const double val = r[first];
    g.v[(size_t)j * g.nx + i] = std::fabs(val) > 900 ? NAN : (float)(val + trans);
  }
  return true;
}

bool LoadAscGrid(const std::string& path, Grid2D& g, std::string& err) {
  std::ifstream in(path);
  if (!in) { err = "Impossible d'ouvrir " + path; return false; }
  long ncols = 0, nrows = 0;
  double xll = 0, yll = 0, cs = 0, nodata = -9999;
  bool center = false;
  std::string key;
  for (int k = 0; k < 6; ++k) {
    std::streampos pos = in.tellg();
    if (!(in >> key)) break;
    std::string lk = key;
    std::transform(lk.begin(), lk.end(), lk.begin(), ::tolower);
    if (lk == "ncols") in >> ncols;
    else if (lk == "nrows") in >> nrows;
    else if (lk == "xllcorner") in >> xll;
    else if (lk == "xllcenter") { in >> xll; center = true; }
    else if (lk == "yllcorner") in >> yll;
    else if (lk == "yllcenter") { in >> yll; center = true; }
    else if (lk == "cellsize") in >> cs;
    else if (lk == "nodata_value") in >> nodata;
    else { in.seekg(pos); break; }
  }
  if (ncols < 2 || nrows < 2 || cs <= 0) { err = "Entête ASC invalide : " + path; return false; }
  const double off = center ? 0 : cs / 2;  // nœuds = centres de cellules
  g.nx = (int)ncols; g.ny = (int)nrows; g.dlon = g.dlat = cs;
  g.lonW = xll + off;
  g.latN = yll + off + (nrows - 1) * cs;
  g.v.resize((size_t)ncols * nrows);
  for (auto& f : g.v) {
    double x;
    if (!(in >> x)) { err = "Fichier ASC tronqué : " + path; return false; }
    f = (x == nodata) ? NAN : (float)x;
  }
  return true;
}

// .gtx : entête gros-boutiste (lat sud, lon ouest, dlat, dlon, nlignes, ncols),
// puis des float32 du sud vers le nord ; -88.8888 = absent
bool LoadGtxGrid(const std::string& path, Grid2D& g, std::string& err) {
  FILE* f = std::fopen(path.c_str(), "rb");
  if (!f) { err = "Impossible d'ouvrir " + path; return false; }
  auto be64 = [](const unsigned char* b) {
    uint64_t u = 0;
    for (int i = 0; i < 8; ++i) u = (u << 8) | b[i];
    double d; std::memcpy(&d, &u, 8); return d;
  };
  auto be32 = [](const unsigned char* b) {
    return (uint32_t)b[0] << 24 | (uint32_t)b[1] << 16 | (uint32_t)b[2] << 8 | b[3];
  };
  unsigned char h[40];
  if (std::fread(h, 1, 40, f) != 40) { std::fclose(f); err = "Fichier .gtx tronqué"; return false; }
  const double lat0 = be64(h), lon0 = be64(h + 8), dla = be64(h + 16), dlo = be64(h + 24);
  const int nr = (int)be32(h + 32), nc = (int)be32(h + 36);
  if (nr < 2 || nc < 2 || nr > 100000 || nc > 100000 || dla <= 0 || dlo <= 0) {
    std::fclose(f); err = "Entête .gtx invalide"; return false;
  }
  std::vector<unsigned char> raw((size_t)nr * nc * 4);
  const bool ok = std::fread(raw.data(), 1, raw.size(), f) == raw.size();
  std::fclose(f);
  if (!ok) { err = "Fichier .gtx tronqué"; return false; }
  g.nx = nc; g.ny = nr; g.dlon = dlo; g.dlat = dla;
  g.lonW = lon0 > 180 ? lon0 - 360 : lon0;
  g.latN = lat0 + (nr - 1) * dla;
  g.v.resize((size_t)nr * nc);
  for (int r = 0; r < nr; ++r)
    for (int c = 0; c < nc; ++c) {
      uint32_t u = be32(&raw[((size_t)r * nc + c) * 4]);
      float x; std::memcpy(&x, &u, 4);
      g.v[(size_t)(nr - 1 - r) * nc + c] = (std::fabs(x + 88.8888f) < 1e-3f) ? NAN : x;
    }
  return true;
}

bool LoadAnyGrid(const std::string& path, Grid2D& g, std::string& err) {
  const std::string e = lowerExt(path);
  if (e == ".tac" || e == ".mnt") return LoadTacGrid(path, g, err);
  if (e == ".asc") return LoadAscGrid(path, g, err);
  if (e == ".gtx") return LoadGtxGrid(path, g, err);
  err = "Format de grille non reconnu (" + e + ") : .tac, .mnt, .asc ou .gtx attendu";
  return false;
}

bool LoadPointSurface(const std::string& path, std::vector<GeoPoint>& pts, std::string& err) {
  std::ifstream in(path);
  if (!in) { err = "Impossible d'ouvrir " + path; return false; }
  std::string line;
  const size_t before = pts.size();
  while (std::getline(in, line)) {
    for (char& c : line) if (c == ',' || c == ';' || c == '\t') c = ' ';
    double a, b, c;
    if (std::sscanf(line.c_str(), "%lf %lf %lf", &a, &b, &c) != 3) continue;
    if (a < -180 || a > 360 || b < -90 || b > 90) continue;
    pts.push_back({a > 180 ? a - 360 : a, b, c});
  }
  if (pts.size() == before) { err = "Aucun point « lon lat valeur » dans " + path; return false; }
  return true;
}

// ------------------------------------------------- grille de correction
namespace {
// Surface définie par points -> valeurs aux nœuds de g (TIN + voisinage)
void RasterizePoints(const std::vector<GeoPoint>& all, Grid2D& g, double maxEdgeDeg) {
  // points utiles : emprise de la grille + marge
  const double m = 0.5;
  const double lo0 = g.lonW - m, lo1 = g.lonW + (g.nx - 1) * g.dlon + m;
  const double la1 = g.latN + m, la0 = g.latN - (g.ny - 1) * g.dlat - m;
  std::vector<GeoPoint> p;
  for (const auto& q : all)
    if (q.lon >= lo0 && q.lon <= lo1 && q.lat >= la0 && q.lat <= la1) p.push_back(q);
  if (p.empty()) return;
  if (p.size() < 3) {
    for (auto& f : g.v) f = (float)p[0].v;
    return;
  }
  std::vector<double> xy;
  xy.reserve(p.size() * 2);
  for (const auto& q : p) {  // coordonnées en nœuds de grille
    xy.push_back((q.lon - g.lonW) / g.dlon);
    xy.push_back((g.latN - q.lat) / g.dlat);
  }
  const std::vector<size_t> tris_ = Triangulate(xy);
  const double maxE = maxEdgeDeg / g.dlon;
  for (size_t t = 0; t + 2 < tris_.size(); t += 3) {
    const size_t a = tris_[t], b = tris_[t + 1], c = tris_[t + 2];
    const double ax = xy[2 * a], ay = xy[2 * a + 1], bx = xy[2 * b], by = xy[2 * b + 1],
                 cx = xy[2 * c], cy = xy[2 * c + 1];
    auto len = [](double x0, double y0, double x1, double y1) { return std::hypot(x1 - x0, y1 - y0); };
    if (len(ax, ay, bx, by) > maxE || len(bx, by, cx, cy) > maxE || len(cx, cy, ax, ay) > maxE) continue;
    const double den = (by - cy) * (ax - cx) + (cx - bx) * (ay - cy);
    if (std::fabs(den) < 1e-12) continue;
    const int i0 = std::max(0, (int)std::ceil(std::min({ax, bx, cx})));
    const int i1 = std::min(g.nx - 1, (int)std::floor(std::max({ax, bx, cx})));
    const int j0 = std::max(0, (int)std::ceil(std::min({ay, by, cy})));
    const int j1 = std::min(g.ny - 1, (int)std::floor(std::max({ay, by, cy})));
    for (int j = j0; j <= j1; ++j)
      for (int i = i0; i <= i1; ++i) {
        const double l1 = ((by - cy) * (i - cx) + (cx - bx) * (j - cy)) / den;
        const double l2 = ((cy - ay) * (i - cx) + (ax - cx) * (j - cy)) / den;
        const double l3 = 1 - l1 - l2;
        if (l1 < -1e-9 || l2 < -1e-9 || l3 < -1e-9) continue;
        g.v[(size_t)j * g.nx + i] = (float)(l1 * p[a].v + l2 * p[b].v + l3 * p[c].v);
      }
  }
}

// Prolonge les valeurs aux nœuds vides voisins (au plus `passes` nœuds)
void Extend(Grid2D& g, int passes) {
  std::vector<float> nv;
  for (int p = 0; p < passes; ++p) {
    nv = g.v;
    bool any = false;
    for (int j = 0; j < g.ny; ++j)
      for (int i = 0; i < g.nx; ++i) {
        if (!std::isnan(g.at(i, j))) continue;
        double acc = 0; int n = 0;
        for (int dj = -1; dj <= 1; ++dj)
          for (int di = -1; di <= 1; ++di) {
            const int ii = i + di, jj = j + dj;
            if (ii < 0 || jj < 0 || ii >= g.nx || jj >= g.ny) continue;
            const float x = g.at(ii, jj);
            if (!std::isnan(x)) { acc += x; ++n; }
          }
        if (n) { nv[(size_t)j * g.nx + i] = (float)(acc / n); any = true; }
      }
    g.v.swap(nv);
    if (!any) break;
  }
}

// Surface (fichiers points ou grilles) échantillonnée aux nœuds de `target`
bool SurfaceOnGrid(const std::vector<std::string>& files, Grid2D& target, std::string& err) {
  std::fill(target.v.begin(), target.v.end(), NAN);
  std::vector<GeoPoint> pts;
  for (const auto& f : files) {
    if (isGridExt(lowerExt(f))) {
      Grid2D src;
      if (!LoadAnyGrid(f, src, err)) return false;
      for (int j = 0; j < target.ny; ++j)
        for (int i = 0; i < target.nx; ++i) {
          float& c = target.v[(size_t)j * target.nx + i];
          if (!std::isnan(c)) continue;
          c = (float)src.sample(target.lonW + i * target.dlon, target.latN - j * target.dlat);
        }
    } else if (!LoadPointSurface(f, pts, err)) {
      return false;
    }
  }
  if (!pts.empty()) {
    Grid2D tin = target;
    std::fill(tin.v.begin(), tin.v.end(), NAN);
    RasterizePoints(pts, tin, 0.3);
    for (size_t k = 0; k < target.v.size(); ++k)
      if (std::isnan(target.v[k])) target.v[k] = tin.v[k];
  }
  // au-delà de l'enveloppe des points (côte, estuaires) : prolongement ~20 km
  Extend(target, (int)std::lround(0.2 / target.dlat));
  return true;
}
}  // namespace

bool SampleSurfaces(const std::vector<std::string>& files, Grid2D& target, std::string& err) {
  return SurfaceOnGrid(files, target, err);
}

bool VerticalCorrection::build(const VerticalRefParams& p, double lonMin, double lonMax,
                               double latMin, double latMax, std::string& err) {
  grid_ = Grid2D();
  if (!p.active()) return true;
  const double step = 0.005, m = 0.05;  // nœuds tous les ~500 m, marge ~5 km
  Grid2D g;
  g.dlon = g.dlat = step;
  g.lonW = std::floor((lonMin - m) / step) * step;
  g.latN = std::ceil((latMax + m) / step) * step;
  g.nx = (int)std::ceil((lonMax + m - g.lonW) / step) + 1;
  g.ny = (int)std::ceil((g.latN - (latMin - m)) / step) + 1;
  if ((double)g.nx * g.ny > 4e7) { err = "Emprise trop grande pour la référence verticale"; return false; }
  g.v.assign((size_t)g.nx * g.ny, NAN);

  if (p.mode == VerticalRefParams::Mode::Surfaces) {
    if (p.zhFiles.empty()) {  // sans grille de référence : hauteurs ellipsoïdales (N = 0)
      err = "Référence verticale : surface ZH requise";
      return false;
    }
    if (!SurfaceOnGrid(p.zhFiles, g, err)) return false;
    // Grille(s) de référence, séparées par ';' : la première qui couvre un
    // nœud l'emporte (ex. RAC23 pour la Corse, puis RAF20 pour le continent)
    Grid2D N = g;
    std::fill(N.v.begin(), N.v.end(), NAN);
    std::stringstream rs(p.refGrid);
    std::string rf;
    while (std::getline(rs, rf, ';')) {
      if (rf.empty()) continue;
      Grid2D ref;
      if (!LoadAnyGrid(rf, ref, err)) return false;
      for (int j = 0; j < g.ny; ++j)
        for (int i = 0; i < g.nx; ++i) {
          float& n = N.v[(size_t)j * g.nx + i];
          if (std::isnan(n)) n = (float)ref.sample(g.lonW + i * step, g.latN - j * step);
        }
    }
    // géoïde lisse : prolongement limité (~10 km) au bord des grilles
    Extend(N, (int)std::lround(0.1 / step));
    if (p.refGrid.empty()) std::fill(N.v.begin(), N.v.end(), 0.f);  // Z ellipsoïdaux
    size_t valid = 0;
    for (size_t k = 0; k < g.v.size(); ++k) {
      float& c = g.v[k];
      c = (std::isnan(c) || std::isnan(N.v[k])) ? NAN : c - N.v[k];  // C = h_ZH - N
      valid += !std::isnan(c);
    }
    if (!valid) { err = "Le levé est hors de la couverture des surfaces de référence"; return false; }
  } else {
    if (p.directFile.empty()) { err = "Référence verticale : grille d'écart requise"; return false; }
    if (!SurfaceOnGrid({p.directFile}, g, err)) return false;
    if (p.directInvert)
      for (auto& c : g.v) c = -c;
  }
  grid_ = std::move(g);
  return true;
}

size_t VerticalCorrection::apply(PointCloud& pc, float& cMin, float& cMax) const {
  size_t miss = 0;
  cMin = 1e30f; cMax = -1e30f;
  for (size_t k = 0; k < pc.size(); ++k) {
    const double c = at(pc.lon(k), pc.lat(k));
    if (std::isnan(c)) { ++miss; continue; }
    pc.z[k] = (float)(pc.z[k] + c);
    cMin = std::min(cMin, (float)c); cMax = std::max(cMax, (float)c);
  }
  pc.computeBounds();
  return miss;
}

}  // namespace pd

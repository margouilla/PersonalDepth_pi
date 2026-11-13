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

// Grille mondiale compacte .pdgrid (voir tools/pdgrid_convert.py), lue par
// tuiles : surfaces de référence verticale, en premier lieu la surface
// moyenne océanique (MSS) CNES_CLS 2025, niveau moyen au-dessus de WGS84.
#pragma once
#include <cstdint>
#include <cstdio>
#include <list>
#include <map>
#include <mutex>
#include <string>
#include <vector>

#include "VerticalRef.h"

namespace pd {

class GeoGridFile {
 public:
  ~GeoGridFile();
  bool open(const std::string& path, std::string& err);
  void close();
  bool isOpen() const { return f_ != nullptr; }
  const std::string& path() const { return path_; }
  const std::string& quantity() const { return quantity_; }  // "MSS/WGS84"
  const std::string& model() const { return model_; }        // "MSS CNES_CLS_2025"
  const std::string& period() const { return period_; }      // "1993-2012"
  // Valeur au point (m), interpolation bilinéaire (poids renormalisés si des
  // nœuds manquent) ; NaN hors grille ou sans nœud valide
  double sample(double lon, double lat);
  // Grille de la zone (nœuds du fichier, +1 de marge), ligne 0 = nord ;
  // renvoie false si erreur de lecture ou zone trop grande
  bool loadRegion(double lonMin, double lonMax, double latMin, double latMax, Grid2D& g,
                  std::string& err);

 private:
  struct Tile {
    std::vector<int32_t> v;    // unités, T*T, ligne 0 = sud
    std::vector<uint8_t> m;    // 1 = présent
    bool empty = true;
  };
  const Tile* tile(int tj, int ti, std::string& err);  // sous mutex_
  double node(long i, long j, std::string& err);       // NaN si absent ; sous mutex_
  FILE* f_ = nullptr;
  std::string path_, quantity_, model_, period_;
  int T_ = 32, nx_ = 0, ny_ = 0, ntx_ = 0, nty_ = 0;
  double lon0_ = 0, lat0_ = 0, dlon_ = 0, dlat_ = 0, unit_ = 0.001;
  bool periodic_ = false;
  // cache des dernières tuiles lues
  std::map<long, Tile> cache_;
  std::list<long> lru_;
  static constexpr size_t kCache = 64;
  std::mutex mutex_;
};

}  // namespace pd

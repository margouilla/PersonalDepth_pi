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

// Traits de côte personnels : polygones et lignes lus dans un fichier
//   - KML / KMZ (Google Earth) : Polygon (anneaux extérieur et intérieurs),
//     LinearRing, LineString ;
//   - shapefile générique : polygones et polylignes (2D, Z, M), système du .prj ;
//   - GeoPackage (.gpkg) : toutes les tables d'entités (Polygon, LineString,
//     Multi…, collections), système de la table.
// Les coordonnées sont rendues en WGS84 (lon, lat). Un polygone donne ses
// anneaux fermés ; une ligne reste ouverte (trait de côte sans intérieur).
#pragma once
#include <memory>
#include <string>
#include <vector>

namespace pd {

struct ShoreFeature {
  std::vector<double> x, y;  // lon (-180..180), lat
  bool closed = false;       // anneau de polygone
};

bool IsShoreFile(const std::string& path);  // .kml .kmz .shp .gpkg

// Shapefile lu à la demande (gros traits de côte nationaux) : emprises des
// enregistrements en lon/lat à l'ouverture, géométries lues au besoin
class ShpSource {
 public:
  struct Box {
    double x0, y0, x1, y1;
    bool ok;
  };
  ShpSource();
  ~ShpSource();
  bool open(const std::string& path, std::string& err);
  bool polygons() const { return polygons_; }
  size_t count() const;
  const Box& box(size_t r) const { return box_[r]; }
  bool read(size_t r, std::vector<ShoreFeature>& out, std::string& err);  // ajoute les parties

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
  std::vector<Box> box_;
  bool polygons_ = false;
};
bool ReadShoreFile(const std::string& path, std::vector<ShoreFeature>& out, std::string& err);

}  // namespace pd

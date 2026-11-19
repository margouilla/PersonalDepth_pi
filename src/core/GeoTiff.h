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

// Lecteur GeoTIFF intégré (sans GDAL), pour les grilles bathymétriques et
// l'imagerie (réflectivité) :
//   - TIFF classique et BigTIFF, petit- ou gros-boutien ;
//   - bandes (strips) ou tuiles ; première bande seulement si plusieurs ;
//   - sans compression, LZW, Deflate, PackBits ; prédicteurs 2 et 3 ;
//   - entiers 8/16/32 bits signés ou non, réels 32/64 bits ; palette ;
//   - géoréférencement : ModelTiepoint + ModelPixelScale (ou matrice sans
//     rotation), GeoKeys : géographique, EPSG projeté, ou projection définie
//     par l'utilisateur (Mercator, Mercator transverse, Lambert conique) ;
//   - NoData : balise GDAL_NODATA.
// Lecture ligne par ligne (un bloc de lignes en mémoire) : un fichier de
// plusieurs Go se lit sans être chargé en entier.
#pragma once
#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace pd {

class PointCloud;
struct LoadOptions;

struct GeoTiffInfo {
  int width = 0, height = 0;
  int bits = 0, samples = 1, sampleFormat = 1;  // 1 entier non signé, 2 signé, 3 réel
  int compression = 1, predictor = 1, planar = 1, photometric = 1;
  bool tiled = false;
  int blockW = 0, blockH = 0;            // tuile, ou largeur × lignes par bande
  std::vector<uint16_t> palette;         // 3 × 2^bits (R, G, B), vide sans palette
  bool hasNoData = false;
  double noData = 0;
  // géoréférencement : coin nord-ouest du pixel (0, 0) et pas des pixels
  bool georef = false;
  double x0 = 0, y0 = 0, dx = 0, dy = 0;  // y décroît vers le bas : y = y0 - j·dy
  bool geographic = true;                 // lon/lat (degrés)
  std::string crsDef;                     // définition pour Crs::Parse (vide = WGS84 lon/lat)
  std::string crsLabel;                   // pour l'affichage (« EPSG:32630 », « Mercator 46°N »)
  std::string crsError;                   // projection non reconnue
  bool verticalDepth = false;             // entête vertical « profondeur » (indicatif seulement)
};

class GeoTiff {
 public:
  GeoTiff();
  ~GeoTiff();
  bool open(const std::string& path, std::string& err);
  const GeoTiffInfo& info() const { return info_; }
  // Ligne r (0 = haut, nord) de la première bande, en réels ; NoData et
  // valeurs non finies -> NaN. `out` reçoit width valeurs.
  bool readRow(int r, float* out, std::string& err);
  // Valeur brute (sans NoData -> NaN) : pour les images à palette
  bool readRowRaw(int r, float* out, std::string& err);

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
  GeoTiffInfo info_;
};

bool IsGeoTiffFile(const std::string& path);  // .tif .tiff

class Crs;
// Système d'une image : celui de l'utilisateur (`user`), sinon celui du
// fichier ; nullptr = lon/lat WGS84. `used` : libellé ; false + err si inconnu.
bool GeoTiffCrs(const GeoTiffInfo& g, const std::shared_ptr<const Crs>& user,
                std::shared_ptr<const Crs>& crs, std::string& used, std::string& err);
// Parcourt les lignes de l'image : `row(r, valeurs, lon, lat)` reçoit la ligne r
// (valeurs brutes si `raw`, sinon NoData -> NaN) et la position WGS84 du centre
// de chaque pixel. Arrêt si `row` renvoie false.
using GeoTiffRowFn = std::function<bool(int, const float*, const double*, const double*)>;
bool ForEachGeoTiffRow(GeoTiff& t, const std::shared_ptr<const Crs>& crs, const GeoTiffRowFn& row,
                       std::string& err, const std::atomic<bool>* cancel = nullptr, bool raw = false);

// Grille GeoTIFF -> nuage (comme LoadESRIAscii) : NoData ignoré, signe et
// unité de Z selon `opt`. Système : celui de `opt.crs` s'il est donné, sinon
// celui du fichier (GeoKeys). crsUsed : système effectivement utilisé.
// Pixel en degrés (cellLon/cellLat) si géographique ; sinon pc.srcStepX/Y en m.
bool LoadGeoTiff(const std::string& path, const LoadOptions& opt, PointCloud& pc, double& cellLon,
                 double& cellLat, std::string& crsUsed, std::string& err,
                 const std::atomic<bool>* cancel = nullptr);

}  // namespace pd

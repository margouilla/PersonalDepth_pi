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

// Imagerie du fond (réflectivité, rétrodiffusion, mosaïques de sonar latéral)
// importée dans une base .pdi : valeurs d'origine gardées (dB, niveaux de
// gris), étirement d'affichage 0 – 1 propre à chaque source (2 % et 98 % des
// valeurs), fond uniforme des mosaïques (blanc ou noir hors levé) écarté.
#pragma once
#include <atomic>
#include <cmath>
#include <memory>
#include <string>
#include <vector>

#include "BathyDb.h"

namespace pd {

class Crs;

struct ImageryOptions {
  std::shared_ptr<const Crs> crs;  // système imposé, sinon celui du fichier
  // Valeur de fond (hors levé) : NaN = détection automatique (valeur extrême
  // très présente sur le bord de l'image), sinon la valeur donnée
  double background = NAN;
  bool noBackground = false;       // aucune valeur de fond
  std::vector<double> nodata;      // valeurs absentes supplémentaires
};

struct ImageryInfo {
  std::string unit;      // « dB », « niveau » ou vide
  double lo = 0, hi = 0;  // étirement (2 % et 98 %)
  bool hasBackground = false;
  double background = 0;
  bool backgroundAuto = false;
  long long pixels = 0;  // pixels importés
  std::string crsUsed;
  double resM = 0;       // pas le plus fin (m)
  double stepX = 0, stepY = 0;  // pas est-ouest et nord-sud (m)
  double midLat = 0;
  int width = 0, height = 0;
};

// Analyse rapide (lignes échantillonnées) : unité, étirement, fond
bool AnalyzeImagery(const std::string& path, const ImageryOptions& o, ImageryInfo& info, std::string& err,
                    const std::atomic<bool>* cancel = nullptr);

// Import d'une image (GeoTIFF) dans une base d'imagerie (db.kind() == Imagery)
bool ImportImagery(BathyDb& db, const std::string& path, const ImageryOptions& o, DbSource meta,
                   DbPriority prio, std::string& err, const DbProgress& progress = nullptr,
                   const std::atomic<bool>* cancel = nullptr, int64_t* newId = nullptr,
                   ImageryInfo* info = nullptr);

}  // namespace pd

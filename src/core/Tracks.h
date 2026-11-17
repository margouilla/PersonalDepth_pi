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

// Traces de sondeur : sondes horodatées le long de la route du bateau.
//   - GPX (Garmin et autres) : points de trace, de route et waypoints portant
//     une profondeur en extension (gpxx:Depth, gpxtpx:depth… : tout élément
//     nommé « depth », en mètres) et leur heure <time> ;
//   - journaux sonar Lowrance / Simrad / B&G .sl2 et .sl3 : seule la
//     profondeur sous le transducteur de chaque ping est lue (pieds -> m),
//     avec la position et l'heure ; l'image sonar est sautée. Un seul canal
//     est gardé (primaire, sinon DownScan, sinon secondaire) et les pings
//     d'une même position GPS sont réduits à leur médiane.
// Les sondes sont en WGS84, profondeurs positives en mètres. Le décalage
// vertical de l'import (enfoncement du transducteur) et les hauteurs d'eau
// observées (correction à l'heure de chaque sonde) s'appliquent.
#pragma once
#include <atomic>
#include <cstdint>
#include <string>
#include <vector>

#include "PointCloud.h"

namespace pd {

bool IsTrackFile(const std::string& path);  // .gpx .sl2 .sl3

struct TrackSample {
  double lon, lat;
  float depth;     // m, positive vers le bas, sous le transducteur
  long long t;     // secondes UTC (0 : inconnue)
  bool hasT;
};

struct TrackInfo {
  std::string kind;       // « GPX », « SL2 », « SL3 »
  long long records = 0;  // points (GPX) ou trames (sonar) lues
  long long noDepth = 0;  // sans profondeur, ou profondeur nulle (fond non trouvé)
  int channel = -1;       // sonar : canal gardé
  long long pings = 0;    // sonar : pings du canal gardé
};

// Lecture brute (tests)
bool ReadGpxSamples(const std::string& path, std::vector<TrackSample>& out, TrackInfo& info,
                    std::string& err);
bool ReadSonarLogSamples(const std::string& path, std::vector<TrackSample>& out, TrackInfo& info,
                         std::string& err, const ProgressFn& progress = nullptr,
                         const std::atomic<bool>* cancel = nullptr);

// Nuage de sondes d'une trace (hauteurs d'eau et décalage vertical de `opt`)
bool LoadTrack(const std::string& path, const LoadOptions& opt, PointCloud& pc, std::string& err,
               const ProgressFn& progress = nullptr, const std::atomic<bool>* cancel = nullptr,
               TrackInfo* info = nullptr);

}  // namespace pd

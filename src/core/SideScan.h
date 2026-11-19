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

// Imagerie de sonar latéral des sondeurs de plaisance (Lowrance / Simrad /
// B&G, journaux .sl2 et .sl3, canal 5 « SideScan » : StructureScan,
// SideImaging…) assemblée en mosaïque géoréférencée :
//   - chaque ping est une ligne bâbord (du loin vers le centre) | tribord (du
//     centre vers le loin), portée ±R ;
//   - hauteur au-dessus du fond h lue sur l'image (premier écho du fond,
//     médiane glissante), à défaut profondeur du journal ;
//   - correction de la distance oblique : distance au sol = √(r² − h²) ; la
//     colonne d'eau (r < h) est écartée ; chaque ping est rééchantillonné au
//     sol (moyenne des échantillons couverts par chaque pas, interpolation
//     près de la verticale) ;
//   - position : points GPS du journal interpolés dans le temps entre deux
//     positions, route lissée sur ±1,5 s ; cap : direction de la route sur
//     ±5 m (cap compas ou route fond du journal à l'arrêt) ;
//   - rayures (gain d'un ping différent de ses voisins) corrigées ;
//   - parasites (traits brefs sur un seul ping : autres sondeurs) retirés par
//     une médiane sur trois pings ;
//   - égalisation facultative, glissante sur ±150 pings : chaque tranche de
//     1 m de distance au sol (par bord) est ramenée au même niveau moyen
//     (décroissance avec la distance, gain, changements de portée) ; au-delà
//     de la portée acoustique (plateau de bruit), le bord n'est pas placé ;
//   - chaque échantillon couvre son empreinte (espacement des pings, élargi
//     dans les virages, et des échantillons) : pas de trous entre les pings ;
//     les recouvrements sont moyennés, pondérés par la force du signal.
// La mosaïque est rangée en blocs créés à la demande (seule la fauchée
// occupe de la mémoire), puis versée dans la base d'imagerie.
#pragma once
#include <atomic>
#include <cstdint>
#include <functional>
#include <string>
#include <unordered_map>
#include <vector>

#include "BathyDb.h"

namespace pd {

bool IsSonarLogFile(const std::string& path);  // .sl2 .sl3

struct SideScanOptions {
  double pixel = 0;      // pas de la mosaïque (m), 0 = automatique (portée / 400, de 0,1 à 1 m) ;
                         // élargi si la fauchée parcourue dépasse 25 M pixels
  bool equalize = true;  // égalisation selon la distance au sol
  bool bedpick = true;      // hauteur au-dessus du fond lue sur l'image (sinon profondeur du journal)
  bool smoothTrack = true;  // lissage de la route (±1,5 s)
  bool destripe = true;     // rayures : gain de chaque ping ramené à celui de ses voisins
  // position de la sonde par rapport à l'antenne GPS (m) : vers l'avant
  // (négatif : sonde en arrière de l'antenne) et vers tribord
  double offsetFwd = 0, offsetStbd = 0;
  bool autoOffset = true;   // estimé sur les lignes en sens opposés (sinon les valeurs ci-dessus)
  bool estimateOnly = false; // décalage seulement, sans mosaïque
};

struct SideScanInfo {
  std::string kind;          // « SL2 », « SL3 »
  long long frames = 0;      // trames lues
  long long pings = 0;       // pings du sonar latéral
  long long used = 0;        // pings placés (position, cap et hauteur connus)
  long long picked = 0;      // pings dont la hauteur vient de l'image (bedpick)
  double offsetFwd = 0, offsetStbd = 0;  // décalage sonde / antenne appliqué (m)
  bool offsetAuto = false;   // estimé sur les données
  double offsetScore = 0;    // corrélation du recalage (0 – 1)
  double rangeM = 0;         // portée la plus fréquente, par bord (m)
  double pixel = 0;          // pas de la mosaïque (m)
  bool pixelWidened = false; // pas élargi (fauchée parcourue trop grande)
  double lengthKm = 0;       // longueur de la route
  double lo = 0, hi = 0;     // étirement (2 % et 98 %)
  long long pixels = 0;      // pixels de la mosaïque
  double lonMin = 0, lonMax = 0, latMin = 0, latMax = 0;
};

// Mosaïque en blocs de kB × kB pixels, repère local (x vers l'est, y vers le
// nord, mètres) autour de (lon0, lat0)
struct SideScanMosaic {
  static constexpr int kB = 128;
  double lon0 = 0, lat0 = 0, pixel = 0.25;
  struct Block {
    std::vector<float> sum;     // somme pondérée
    std::vector<uint16_t> w;    // somme des poids (× 1024)
  };
  std::unordered_map<int64_t, Block> blocks;
  void add(double x, double y, float v, float w = 1.f);
  // pixel (i, j) -> lon, lat du centre
  void lonLat(long long i, long long j, double& lon, double& lat) const;
  // parcourt les pixels renseignés : (lon, lat, moyenne)
  void forEach(const std::function<void(double, double, float)>& fn) const;
  static int64_t Key(long long bi, long long bj) { return (int64_t)((bi + (1LL << 30)) << 32) | (int64_t)(bj + (1LL << 30)); }
};

// Lecture du journal et assemblage de la mosaïque
bool BuildSideScanMosaic(const std::string& path, const SideScanOptions& o, SideScanMosaic& m,
                         SideScanInfo& info, std::string& err,
                         const std::function<void(float, const std::string&)>& progress = nullptr,
                         const std::atomic<bool>* cancel = nullptr);

// Décalage sonde / antenne estimé sur le sonar latéral d'un journal (lignes en
// sens opposés) ; false si pas de sonar latéral ou pas d'estimation fiable
bool EstimateSideScanOffset(const std::string& path, double& offFwd, double& offStbd, double& score,
                            const std::atomic<bool>* cancel = nullptr);

// Import dans une base d'imagerie (db.kind() == Imagery)
bool ImportSideScan(BathyDb& db, const std::string& path, const SideScanOptions& o, DbSource meta,
                    DbPriority prio, std::string& err, const DbProgress& progress = nullptr,
                    const std::atomic<bool>* cancel = nullptr, int64_t* newId = nullptr,
                    SideScanInfo* info = nullptr);

}  // namespace pd

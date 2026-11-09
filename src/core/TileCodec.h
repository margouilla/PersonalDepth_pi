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

// Tuile de profondeurs 256 × 256 (mm, entier 32 bits) et sa compression.
//   en-tête : "PT" version(2) drapeaux (bit 0 partielle, bit 1 plan de sources,
//             bits 2-3 unité : mm, cm, dm, m) zmin(int32) zmax(int32) nIds(uint16)
//             ids(int64 × nIds) tailleBrute(uint32)
//   puis flux zlib (miniz) : [masque 8192 o si tuile partielle]
//             4 plans d'octets des résidus (zigzag) du prédicteur MED (JPEG-LS)
//             [plan d'indices de source (uint8) si nIds > 1]
#pragma once
#include <climits>
#include <cstdint>
#include <string>
#include <vector>

namespace pd {

struct DepthTile {
  static constexpr int32_t kEmpty = INT32_MIN;
  static constexpr int kN = 256 * 256;
  // Au-delà de ±12 km (plus profond que les fosses, plus haut que l'Everest)
  // une valeur est une valeur de remplissage : ignorée à l'import et à la
  // lecture (bases créées avant ce contrôle).
  static constexpr int32_t kMaxAbs = 12000000;  // mm
  static bool plausible(double mm) { return mm >= -kMaxAbs && mm <= kMaxAbs; }
  std::vector<int32_t> v;        // profondeur en mm (positive vers le bas), kEmpty = vide
  std::vector<uint8_t> src;      // index dans srcIds (tuiles fusionnées), vide si une source
  std::vector<int64_t> srcIds;   // sources présentes (fusion)
  // Pixels interpolés (surface TIN entre les sondes d'un nuage) : 1 = interpolé,
  // vide = aucun. Une sonde mesurée l'emporte toujours sur une valeur interpolée.
  std::vector<uint8_t> interp;

  void reset() { v.assign(kN, kEmpty); src.clear(); srcIds.clear(); interp.clear(); }
  bool isInterp(int k) const { return !interp.empty() && interp[k]; }
  void setInterp(int k, bool on) {
    if (interp.empty()) { if (!on) return; interp.assign(kN, 0); }
    interp[k] = on ? 1 : 0;
  }
  bool allocated() const { return (int)v.size() == kN; }
  size_t count() const;
  int64_t sourceAt(int k) const {
    if (srcIds.empty()) return 0;
    return src.empty() ? srcIds[0] : srcIds[src[k]];
  }
};

bool EncodeTile(const DepthTile& t, std::vector<uint8_t>& out);
bool DecodeTile(const uint8_t* data, size_t n, DepthTile& t, std::string& err);

}  // namespace pd

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

// Rampe colorimétrique éditable à chaud + table de correspondance (LUT).
// Couleurs empaquetées R | G<<8 | B<<16 | A<<24 (octets RGBA en mémoire, LE).
#pragma once
#include <cstdint>
#include <string>
#include <vector>

#include "BathyGrid.h"

namespace pd {

inline uint32_t RGBA(int r, int g, int b, int a = 255) {
  return (uint32_t)r | ((uint32_t)g << 8) | ((uint32_t)b << 16) | ((uint32_t)a << 24);
}
inline int R_(uint32_t c) { return c & 255; }
inline int G_(uint32_t c) { return (c >> 8) & 255; }
inline int B_(uint32_t c) { return (c >> 16) & 255; }
inline int A_(uint32_t c) { return (c >> 24) & 255; }

struct ColorStop {
  float depth;
  uint32_t rgba;
};

class ColorRamp {
 public:
  enum class Mode { Continuous, Stepped };
  Mode mode = Mode::Stepped;
  std::vector<ColorStop> stops;  // triés par profondeur croissante
  // Zone de sécurité (tirant d'eau + pied de pilote) mélangée par-dessus
  bool safetyOn = false;
  float safetyDepth = 2.0f;
  uint32_t safetyColor = RGBA(230, 30, 30, 150);

  void sort();
  uint32_t colorAt(float depth) const;  // évaluation exacte
  bool operator==(const ColorRamp& o) const { return serialize() == o.serialize(); }

  std::string serialize() const;            // pour la configuration OpenCPN
  bool deserialize(const std::string& s);
  bool loadCPT(const std::string& path, std::string& err);  // palettes GMT
  bool saveCPT(const std::string& path) const;

  static std::vector<std::string> PresetNames();
  static ColorRamp Preset(const std::string& name);
  // Couleurs de profondeur des cartes ENC d'OpenCPN (S-52, jour) entre les
  // isobathes peu profonde / de sécurité / profonde (m)
  static ColorRamp EncPreset(float shallow, float safety, float deep);
  static constexpr const char* kEncName = "ENC OpenCPN (S-52)";
};

// Palette automatique : n seuils répartis entre zMin et zMax (valeurs rondes
// 1-2-2,5-5 × 10^k si `nice`), couleurs prises le long des seuils de `colors`
// (les couleurs « estran », seuils < 0, sont réservées aux profondeurs < 0).
// Le mode (paliers / continu) et la zone dangereuse de `keep` sont conservés.
std::vector<float> AutoThresholds(float zMin, float zMax, int n, bool nice);
ColorRamp AutoRamp(const ColorRamp& colors, const ColorRamp& keep, float zMin, float zMax,
                   int n, bool nice);

// LUT 16384 entrées sur [zMin, zMax] : recoloration de millions de cellules
// en quelques millisecondes. Erreur de seuil <= (zMax-zMin)/32768.
struct ColorLUT {
  float z0 = 0, scale = 1;
  std::vector<uint32_t> t;
  // offset : ajouté à la profondeur avant de lire la palette (hauteur de marée)
  void build(const ColorRamp& r, float zMin, float zMax, int n = 16384, float offset = 0.f);
  uint32_t operator()(float z) const {
    long k = (long)((z - z0) * scale);
    k = k < 0 ? 0 : (k >= (long)t.size() ? (long)t.size() - 1 : k);
    return t[(size_t)k];
  }
};

// Colorise un rectangle de cellules d'un niveau (lignes nord -> sud).
// shadeStrength 0 = pas d'ombrage. NaN -> transparent. `tide` : hauteur
// d'eau ajoutée à chaque cellule (profondeur du moment), facultative.
struct TideField;
void Colorize(const GridLevel& g, int i0, int j0, int w, int h,
              const ColorLUT& lut, float shadeStrength, uint32_t* out,
              int outStride, const TideField* tide = nullptr);

}  // namespace pd

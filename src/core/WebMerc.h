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

// Tuiles « Web Mercator » (comme les cartes web et les bases PBG de
// TimeZero) : niveau z, tuiles de 256 × 256 pixels, x vers l'est, y vers le
// sud (y = 0 au nord). Toutes les sources d'une base partagent ce maillage,
// ce qui rend la fusion pixel par pixel exacte.
#pragma once
#include <cstdint>

namespace pd {

constexpr int kTileSize = 256;
constexpr int kDbMinLevel = 10;   // niveau le plus grossier stocké (~0,35°)
constexpr int kDbMaxLevel = 22;   // ~4 cm à 48° N

// Pixel global continu (0 .. 256·2^z) ; centre du pixel (i, j) = (i + 0,5, j + 0,5)
void LonLatToPixel(int z, double lon, double lat, double& gx, double& gy);
void PixelToLonLat(int z, double gx, double gy, double& lon, double& lat);
// Taille au sol d'un pixel (m) à la latitude donnée
double PixelGroundSize(int z, double lat);
// Niveau le plus fin dont le pixel reste ≥ 0,7 × la résolution du levé
// (au plus une sonde par pixel, sans trous pour un levé maillé)
int LevelForResolution(double resM, double lat);
// Emprise (lon/lat) d'une tuile
void TileBounds(int z, int x, int y, double& lonW, double& lonE, double& latS, double& latN);

}  // namespace pd

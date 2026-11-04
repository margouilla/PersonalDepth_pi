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

#include "WebMerc.h"

#include <algorithm>
#include <cmath>

namespace pd {

static constexpr double kEarthR = 6378137.0;

void LonLatToPixel(int z, double lon, double lat, double& gx, double& gy) {
  const double n = std::ldexp((double)kTileSize, z);
  lat = std::max(-85.05112878, std::min(85.05112878, lat));
  const double phi = lat * M_PI / 180.0;
  gx = (lon + 180.0) / 360.0 * n;
  gy = (1.0 - std::log(std::tan(phi) + 1.0 / std::cos(phi)) / M_PI) / 2.0 * n;
}

void PixelToLonLat(int z, double gx, double gy, double& lon, double& lat) {
  const double n = std::ldexp((double)kTileSize, z);
  lon = gx / n * 360.0 - 180.0;
  lat = std::atan(std::sinh(M_PI * (1.0 - 2.0 * gy / n))) * 180.0 / M_PI;
}

double PixelGroundSize(int z, double lat) {
  return 2 * M_PI * kEarthR / std::ldexp((double)kTileSize, z) * std::cos(lat * M_PI / 180.0);
}

int LevelForResolution(double resM, double lat) {
  if (!(resM > 0)) return kDbMinLevel;
  const double g0 = 2 * M_PI * kEarthR / kTileSize * std::cos(lat * M_PI / 180.0);
  const int z = (int)std::floor(std::log2(g0 / (0.7 * resM)) + 1e-9);
  return std::max(kDbMinLevel, std::min(kDbMaxLevel, z));
}

void TileBounds(int z, int x, int y, double& lonW, double& lonE, double& latS, double& latN) {
  PixelToLonLat(z, (double)x * kTileSize, (double)y * kTileSize, lonW, latN);
  PixelToLonLat(z, (double)(x + 1) * kTileSize, (double)(y + 1) * kTileSize, lonE, latS);
}

}  // namespace pd

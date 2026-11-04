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

// Triangulation de Delaunay (enveloppe de delaunator.hpp, inclus une seule fois)
#pragma once
#include <cstddef>
#include <vector>

namespace pd {
// xy = x0, y0, x1, y1… ; renvoie les indices des sommets, 3 par triangle
std::vector<size_t> Triangulate(const std::vector<double>& xy);
}  // namespace pd

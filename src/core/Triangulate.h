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

// Méthodes d'interpolation entre les sondes. La zone remplie est celle des
// triangles retenus (TIN limité par D) ; la méthode ne change que les valeurs.
enum class InterpMethod { Tin = 0, NaturalNeighbour = 1, Idw = 2 };
const char* InterpMethodName(InterpMethod m);  // texte français (pd::tr)

// Valeurs aux centres (x + 0.5, y + 0.5) des pixels `pix` (x, y entiers, à
// plat : x0, y0, x1, y1…), d'après les sommets (xy, val), dans le même repère :
// - NaturalNeighbour : voisin naturel de Sibson discret (Park et al. 2006) :
//   chaque pixel q du voisinage, de sommet le plus proche s(q) à la distance
//   r(q), apporte la valeur de s(q) à tous les pixels à moins de r(q) de lui ;
// - Idw : inverse du carré de la distance, 12 sommets au plus à moins de maxDist.
// `out` reçoit une valeur par pixel ; NaN si aucune (le TIN est alors gardé).
void InterpolatePixels(InterpMethod m, const std::vector<double>& xy, const std::vector<double>& val,
                       const std::vector<int>& pix, double maxDist, std::vector<double>& out);
}  // namespace pd

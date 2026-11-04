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

// Décompression d'un flux zlib (RFC 1950/1951), sans dépendance.
// Suffisant pour les tuiles du fichier de marée .pdtide.
#pragma once
#include <cstddef>
#include <cstdint>
#include <vector>

namespace pd {

// Décompresse `src` (en-tête zlib + deflate) dans `out` ; `expected` sert de
// réservation (0 si inconnu). Renvoie false si le flux est invalide.
bool ZlibInflate(const uint8_t* src, size_t n, std::vector<uint8_t>& out, size_t expected = 0);

}  // namespace pd

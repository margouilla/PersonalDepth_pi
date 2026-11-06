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

// Fichiers aux chemins UTF-8 (cyrillique, accents…) : sous Windows, fopen et
// les flux standard interprètent un std::string dans la page de code du
// système ; on passe par les chemins larges.
#pragma once
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>

namespace pd {
inline std::filesystem::path U8Path(const std::string& utf8) { return std::filesystem::u8path(utf8); }
FILE* OpenFile(const std::string& utf8, const char* mode);
void RemoveFile(const std::string& utf8);
// date de modification (secondes depuis 1970), 0 si le fichier est absent
long long FileMTime(const std::string& utf8);
}  // namespace pd

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

// Langue de l'interface : français (textes du code), anglais ou russe (tables
// de traduction indexées par le texte français exact). La langue suit celle
// d'OpenCPN ; pd::tr() rend, à défaut de traduction russe, l'anglais, puis le
// texte d'origine.
#pragma once

namespace pd {
enum Lang { kFrench = 0, kEnglish = 1, kRussian = 2 };
void SetLanguage(int lang);
int Language();
inline void SetEnglish(bool english) { SetLanguage(english ? kEnglish : kFrench); }
inline bool IsEnglish() { return Language() == kEnglish; }
inline bool IsRussian() { return Language() == kRussian; }
// texte traduit (UTF-8) ; le pointeur reste valide (table statique ou argument)
const char* tr(const char* fr);
}  // namespace pd

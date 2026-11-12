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

// Langue de l'interface : français (textes du code), anglais (table compilée
// indexée par le texte français exact), russe (table compilée) ou toute langue
// fournie par un fichier de traduction .po (msgid = texte anglais). La langue
// suit celle d'OpenCPN ; pd::tr() rend, à défaut de traduction, l'anglais, puis
// le texte d'origine.
#pragma once

#include <string>
#include <vector>

namespace pd {
enum Lang { kFrench = 0, kEnglish = 1, kRussian = 2, kOther = 3 };
void SetLanguage(int lang);
int Language();
inline void SetEnglish(bool english) { SetLanguage(english ? kEnglish : kFrench); }
inline bool IsEnglish() { return Language() == kEnglish; }
inline bool IsRussian() { return Language() == kRussian; }
// texte traduit (UTF-8) ; le pointeur reste valide (table statique ou argument)
const char* tr(const char* fr);

// Fichiers de traduction (format gettext .po, UTF-8) : msgid = texte anglais,
// msgstr = traduction ; msgctxt facultatif = texte français exact (pour deux
// textes français de même anglais). Les fichiers sont lus dans l'ordre : une
// entrée d'un fichier suivant remplace celle d'un précédent (fichier de
// l'utilisateur après celui livré avec le plugin). Une traduction vide, ou dont
// les spécificateurs de format (%d, %s…) diffèrent de l'anglais, est ignorée.
// Rend le nombre d'entrées retenues (-1 : fichier illisible) ; « notes » reçoit
// les entrées ignorées.
int LoadTranslations(const std::vector<std::string>& paths, std::string* notes = nullptr);
void ClearTranslations();
bool HasTranslations();
// Écrit le modèle .po de la langue (anglais -> traduction compilée s'il y en a,
// sinon vide), avec le texte français en commentaire.
bool WriteTranslationTemplate(const std::string& path, int lang, std::string& err);
// Spécificateurs de format d'un texte (ex. "%d de %s" -> "ds") ; %% ignoré
std::string FormatSignature(const char* s);
}  // namespace pd

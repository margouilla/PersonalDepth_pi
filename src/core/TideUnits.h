// PersonalDepth_pi - unité des hauteurs des stations de marée d'OpenCPN
//
// L'API des plugins d'OpenCPN (GetTideHeight) rend la hauteur dans l'unité de
// la station (pieds pour la plupart des stations NOAA du fichier
// harmonics-dwf, mètres pour TICON…) sans dire laquelle. Ces fonctions la
// lisent dans les fichiers de marées : facteur vers le mètre, ou NaN si la
// station n'est pas trouvée.
//
// Copyright (C) 2026 Marcaurelio Franzetti
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <string>

namespace pd {

// « meters », « metres », « m » -> 1 ; « feet », « foot », « ft » -> 0,3048 ; sinon NaN
double TideUnitToMetres(const std::string& unit);

// Fichier .tcd (libtcd v1 / v2) : station de ce nom, à moins de 0,01° de
// (lat, lon). Station secondaire : son unité (celle de sa station de
// référence dans les fichiers connus)
double TcdStationToMetres(const std::string& tcdPath, const std::string& station, double lat, double lon);

// Fichier texte des harmoniques (format XTide 1, HARMONIC / HARMONICS_NO_US) :
// ligne du nom de la station, puis fuseau, puis « <niveau moyen> <unité> »
double HarmonicStationToMetres(const std::string& harmonicPath, const std::string& station);

}  // namespace pd

// PersonalDepth_pi - marégraphes REFMAR du Shom (fournisseur de TideGauge.h)
//
// Service public du Shom (Licence Ouverte Etalab 2.0, citer « REFMAR ») :
//   liste    https://services.data.shom.fr/maregraphie/service/tidegauges
//            [{"shom_id":"152","name":"LE_CONQUET","longitude":..,"latitude":..,
//              "state":"OK","reseau":"RONIM",...}, ...]
//   mesures  https://services.data.shom.fr/maregraphie/observation/txt/<id>
//            ?sources=1,2,3,4&dtStart=AAAA-MM-JJT00:00:00Z&dtEnd=...T23:59:59Z
// Réponse texte : entête « # Clé : valeur » (station, fuseau UTC, référence
// zero_hydrographique, unité m), puis « JJ/MM/AAAA hh:mm:ss;valeur;source ».
// Sources : 1 brutes temps réel (1 min), 2 brutes temps différé (10 min),
// 3 validées temps différé (10 min), 4 validées horaires, 5 brutes horaires,
// 6 pleines et basses mers. Le service renvoie toutes les sources demandées
// aux mêmes instants : la lecture garde, jour par jour, la meilleure source
// présente (3, puis 2, 1, 4, 5 ; 6 jamais).
//
// Copyright (C) 2026 Marcaurelio Franzetti
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <string>
#include <vector>

#include "TideGauge.h"

namespace pd {
namespace refmar {

extern const char* kStationsUrl;
extern const char* kCitation;  // mention à citer
extern const char* kLicence;

bool ParseStations(const std::string& json, std::vector<GaugeStation>& out, std::string& err);
std::string DataUrl(const std::string& id, long long t0, long long t1);

// Texte d'une réponse ou d'un fichier REFMAR ?
bool IsRefmarText(const std::string& head);
// Lecture : instants (s UTC) et hauteurs (m au-dessus du zéro hydrographique),
// meilleure source par jour. Lignes illisibles comptées dans skipped.
bool Parse(const std::string& text, std::vector<long long>& t, std::vector<double>& h, int& skipped,
           std::string& err);
// Valeur d'une clé de l'entête (« Station », « Référence verticale »…)
std::string HeaderValue(const std::string& text, const std::string& key);
// Réponse utile (entête « # Station » et au moins une mesure)
bool HasData(const std::string& text);

const GaugeProvider& Provider();

}  // namespace refmar
}  // namespace pd

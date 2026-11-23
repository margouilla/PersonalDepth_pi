// PersonalDepth_pi - marégraphes NOAA CO-OPS (États-Unis), fournisseur de
// TideGauge.h
//
// Service public de la NOAA (domaine public, œuvre du gouvernement des
// États-Unis ; citer « NOAA CO-OPS ») :
//   liste    https://api.tidesandcurrents.noaa.gov/mdapi/prod/webapi/stations.json?type=waterlevels
//            {"count":N,"stations":[{"id":"8454000","name":"Providence","lat":..,"lng":..,
//              "state":"RI","tidal":true,"greatlakes":false,...}]}
//            (stations sans marée et des Grands Lacs écartées : pas de MLLW)
//   mesures  https://api.tidesandcurrents.noaa.gov/api/prod/datagetter?product=water_level
//            &application=PersonalDepth&begin_date=AAAAMMJJ&end_date=AAAAMMJJ&datum=MLLW
//            &station=<id>&time_zone=gmt&units=metric&format=csv
//            31 jours au plus ; réponse « Date Time, Water Level, Sigma, ..., Quality »
//            puis « AAAA-MM-JJ hh:mm,valeur,sigma,...,p|v » (p provisoire, v vérifiée),
//            pas de 6 min, mètres au MLLW (zéro des cartes des États-Unis), UTC.
// La réponse ne dit ni la référence ni l'unité : le fichier enregistré
// commence par « # NOAA CO-OPS ... datum=MLLW units=metric time_zone=GMT ».
//
// Copyright (C) 2026 Marcaurelio Franzetti
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <string>
#include <vector>

#include "TideGauge.h"

namespace pd {
namespace noaa {

extern const char* kStationsUrl;
bool ParseStations(const std::string& json, std::vector<GaugeStation>& out, std::string& err);
std::string DataUrl(const std::string& id, long long t0, long long t1);
bool HasData(const std::string& text);
bool Parse(const std::string& text, std::vector<long long>& t, std::vector<double>& h, int& skipped,
           std::string& err);
const GaugeProvider& Provider();

}  // namespace noaa
}  // namespace pd

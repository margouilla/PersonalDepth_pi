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

// Systèmes de coordonnées : conversion vers WGS84 sans dépendance externe.
// Méthodes : Mercator transverse (Krüger ordre 6), Lambert conique conforme
// (1SP/2SP), stéréographique oblique et polaire, oblique suisse, Mercator,
// changement de datum par Helmert 7 paramètres (convention PROJ +towgs84).
// Définitions acceptées : code EPSG du catalogue ("EPSG:2154") ou chaîne PROJ.
#pragma once
#include <string>
#include <vector>

namespace pd {

class Crs {
 public:
  // def : "EPSG:2154", "2154" ou "+proj=lcc +lat_1=..." ; renvoie false + err
  static bool Parse(const std::string& def, Crs& out, std::string& err);

  bool isGeographic() const { return kind_ == Kind::LongLat; }
  // Vrai si les coordonnées sont déjà en WGS84 lon/lat (aucune conversion)
  bool isWGS84LonLat() const { return isGeographic() && !hasHelmert_ && pm_ == 0; }
  // x/y = Est/Nord en unités du système (ou lon/lat en degrés si géographique)
  bool toWGS84(double x, double y, double& lon, double& lat) const;
  bool fromWGS84(double lon, double lat, double& x, double& y) const;
  const std::string& definition() const { return def_; }
  double unitToMeter() const { return unit_; }

 private:
  enum class Kind { LongLat, TMerc, Lcc, StereA, StereB, Sterea, Somerc, Merc };
  Kind kind_ = Kind::LongLat;
  std::string def_;
  // ellipsoïde
  double a_ = 6378137.0, f_ = 1 / 298.257223563, e2_ = 0, e_ = 0;
  // paramètres
  double lat0_ = 0, lon0_ = 0, lat1_ = 0, lat2_ = 0, latts_ = 0, k0_ = 1;
  double x0_ = 0, y0_ = 0, pm_ = 0, unit_ = 1;
  bool south_ = false;  // pôle sud (stéréographique polaire)
  // Helmert vers WGS84 (rotations en radians, échelle en fraction)
  bool hasHelmert_ = false;
  double tw_[7] = {0, 0, 0, 0, 0, 0, 0};
  // constantes précalculées
  double c_[16] = {0};
  double alpha_[7] = {0}, beta_[7] = {0};

  void init();
  bool projInverse(double x, double y, double& lam, double& phi) const;  // radians, ellipsoïde source
  bool projForward(double lam, double phi, double& x, double& y) const;
  void datumToWGS84(double& lam, double& phi) const;
  void datumFromWGS84(double& lam, double& phi) const;
};

struct CrsEntry {
  std::string code;   // "EPSG:2154" ou identifiant interne ("UTM-WGS84")
  std::string group;  // France, Europe, Monde…
  std::string name;   // libellé affiché
  int zoneMin = 0, zoneMax = 0;  // > 0 : nécessite un fuseau UTM (et un hémisphère)
  bool zoneSouthDefault = false;
};

// Catalogue présenté dans l'interface
const std::vector<CrsEntry>& CrsCatalog();
// Définition PROJ d'une entrée (fuseau/hémisphère utilisés si l'entrée en demande)
std::string CrsDefinition(const CrsEntry& e, int zone, bool south);
// Code EPSG -> chaîne PROJ (catalogue + familles UTM) ; "" si inconnu
std::string EpsgToProj(int code);
// Lit un .prj (WKT) : code EPSG reconnu (0 si inconnu)
int EpsgFromWkt(const std::string& wkt);
// Indice sur des coordonnées hors plage lon/lat (message d'aide), "" sinon
std::string GuessCrsHint(double x, double y);

}  // namespace pd

// PersonalDepth_pi - nature d'un levé : grille ou nuage, résolution, distance D
//
// Grille : nœuds réguliers (pas dx, dy), dans le système d'origine, en lon/lat
// ou dans la zone UTM du centre (grille projetée diffusée en degrés), avec une
// part raisonnable des nœuds occupée. Sinon nuage de points.
// Statistiques d'espacement sur une fenêtre de la donnée (au plus ~300 000
// sondes) : plus proche voisin et arêtes de la triangulation de Delaunay. Un
// levé en lignes (sondeur monofaisceau, routes) a des arêtes bimodales : le
// long de la route (petites) et entre les lignes (grandes).
//
// Copyright (C) 2026 Marcaurelio Franzetti
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <cstddef>
#include <string>

namespace pd {

class PointCloud;

// Type d'instrument (choix de l'utilisateur, en termes simples) : ajuste la
// distance maximale d'interpolation D. Auto : d'après les données seules.
enum class Instrument {
  Auto = 0,
  SingleBeam,      // sondeur classique (un faisceau), routes ou profils
  MultiBeam,       // sondeur multifaisceaux / large fauchée
  Lidar,           // lidar bathymétrique
  ChartSoundings,  // sondes de carte (sélection)
  LeadLine,        // sondes au plomb, levés anciens
};

struct SurveyShape {
  enum class Kind { Cloud, Grid };
  Kind kind = Kind::Cloud;
  bool forced = false;      // nature imposée par l'utilisateur
  // Grille : pas (m) et repère où le réseau a été trouvé
  double dx = 0, dy = 0;
  std::string frame;        // "source", "lonlat", "utm<zone>"
  double occupancy = 0;     // part des nœuds de l'emprise occupés
  // Nuage
  bool lines = false;       // levé en lignes (arêtes bimodales)
  double nn50 = 0;          // plus proche voisin, médiane (m)
  double edge50 = 0, edge90 = 0, edge95 = 0;  // arêtes de Delaunay (m)
  double thinCell = 0;      // > 0 : arêtes mesurées sur un échantillon éclairci (m)
  size_t analysed = 0;      // sondes de la fenêtre d'analyse
  // Décisions
  double storeRes = 0;      // résolution de stockage proposée (m)
  // résolution de stockage avec surface interpolée : nuage clairsemé (hors
  // lignes) stocké plus finement pour que la surface entre les sondes ait du
  // détail (D / 8, au moins 2 m, au plus storeRes)
  double interpRes = 0;
  double maxEdge = 0;       // distance maximale d'interpolation D (m), 0 = aucune

  bool isGrid() const { return kind == Kind::Grid; }
  std::string describe() const;  // résumé lisible (journal, fenêtre d'import)
};

// force : 0 automatique, 1 grille, 2 nuage
SurveyShape AnalyzeSurvey(const PointCloud& pc, Instrument hint = Instrument::Auto, int force = 0);

}  // namespace pd

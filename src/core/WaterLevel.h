// PersonalDepth_pi - hauteurs d'eau observées (marégraphe, échelle de crue…)
//
// Fichiers texte « date[ heure], hauteur » (une observation par ligne,
// séparateur virgule, point-virgule, tabulation ou espaces) :
//   2026-11-01 05:00:00,0.35
//   2026-11-01,0.35
// Les lignes illisibles (entête « [TIME],[DEPTH] »…) sont ignorées. Un
// dossier : tous ses fichiers .txt / .csv / .dat sont fusionnés en une seule
// série (même station, périodes successives), triée par date.
//
// Copyright (C) 2026 Marcaurelio Franzetti
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <string>
#include <vector>

#include "PointCloud.h"

namespace pd {

class WaterLevelSeries {
 public:
  // Fichier ou dossier. Format personnalisé (colonnes date, heure, hauteur =
  // colZ, fuseau horaire) ou, s'il ne l'est pas, reconnaissance automatique
  // « date heure, hauteur ». toMeters : unité des hauteurs -> mètres.
  bool load(const std::string& fileOrDir, const TextFormat& fmt, double toMeters, std::string& err);
  // Premier fichier lu par load() pour un chemin (aperçu du format)
  static std::string FirstFile(const std::string& fileOrDir);
  // Hauteur (m) à l'instant t (secondes UTC depuis 1970, même horloge que les
  // sondes) : interpolation linéaire ; hors de la série, valeur extrême gardée
  // jusqu'à tolerance() secondes. false si t est trop loin de la série.
  bool at(long long t, float& h) const;
  long long tolerance() const { return tol_; }
  size_t size() const { return t_.size(); }
  const std::vector<long long>& times() const { return t_; }
  const std::vector<float>& heights() const { return h_; }
  long long tMin() const { return t_.empty() ? 0 : t_.front(); }
  long long tMax() const { return t_.empty() ? 0 : t_.back(); }
  float hMin() const { return hMin_; }
  float hMax() const { return hMax_; }
  // Fichiers lus, avec leur date de modification (clé de cache des sondes corrigées)
  const std::string& key() const { return key_; }
  const std::vector<std::string>& files() const { return files_; }
  int skippedLines() const { return skipped_; }
  // Lecture d'une ligne (tests) : false si illisible
  static bool ParseLine(const std::string& line, int dateOrder, long long& t, double& h);

 private:
  std::vector<long long> t_;
  std::vector<float> h_;
  long long tol_ = 3600;
  float hMin_ = 0, hMax_ = 0;
  std::string key_;
  std::vector<std::string> files_;
  int skipped_ = 0;
};

// Texte « 2026-11-01 05:00 » (UTC)
std::string FormatTime(long long t, bool seconds = false);

}  // namespace pd

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

// Marée maillée FES2022 : lecture du fichier compact .pdtide (voir
// tools/pdtide_convert.py) et prédiction de la hauteur d'eau.
//
//   h(t) = Σ f_k(t) · [Re_k cos(V_k+u_k)(t) + Im_k sin(V_k+u_k)(t)]
//   Re = A cos G, Im = A sin G (G : retard de phase de Greenwich)
//
// Arguments astronomiques et corrections nodales : formules de Schureman
// (ordre 1), mêmes conventions que la bibliothèque FES / pyfes. Les ondes
// mineures (2Q1, σ1, ρ1, M11, M12, χ1, π1, φ1, θ1, OO1, η2) sont déduites des
// ondes principales par admittance, avec les coefficients de FES / pyfes.
#pragma once
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <string>
#include <vector>

namespace pd {

// Angles astronomiques (degrés) à un instant (secondes UTC depuis 1970)
struct TideAstro {
  double t = 0, s = 0, h = 0, p = 0, n = 0, p1 = 0;     // longitudes moyennes
  double I = 0, xi = 0, nu = 0, nuprim = 0, nusec = 0, R = 0, x1ra = 1;
  void update(double unixSeconds);
};

// Description d'une onde (convention FES/pyfes)
struct TideWave {
  const char* name;
  int8_t d[7];          // τ s h p N' p1, puis phase en multiples de -90°
  uint8_t fKind;        // formule du facteur nodal
  int8_t u[5];          // coefficients de ξ ν ν' ν'' R dans u
};
const TideWave* FindTideWave(const std::string& name);

// f et (V+u) en radians pour une liste d'ondes à un instant
struct TideNodal {
  std::vector<double> f, cosArg, sinArg;
  void compute(const std::vector<const TideWave*>& waves, double unixSeconds);
};

// Constantes harmoniques d'une zone, nœuds réguliers (ligne 0 = sud)
struct TideRegion {
  int nx = 0, ny = 0;
  double lon0 = 0, lat0 = 0, dlon = 0, dlat = 0;
  std::vector<const TideWave*> waves;
  std::vector<float> re, im;     // [onde][j][i], m
  std::vector<uint8_t> valid;    // [j][i]
  bool empty() const { return valid.empty(); }
  bool covers(double lonMin, double lonMax, double latMin, double latMax) const;
  // hauteur aux nœuds (NaN hors modèle) pour les facteurs `nod`
  void heights(const TideNodal& nod, std::vector<float>& h) const;
  // interpolation bilinéaire d'un champ aux nœuds (poids renormalisés si
  // des nœuds manquent) ; NaN si aucun nœud valide
  double sample(const std::vector<float>& field, double lon, double lat) const;
  // hauteur au point (m) ; NaN hors modèle
  double heightAt(double lon, double lat, double unixSeconds) const;
};

class TideAtlas {
 public:
  ~TideAtlas();
  bool open(const std::string& path, std::string& err);
  bool isOpen() const { return f_ != nullptr; }
  const std::string& path() const { return path_; }
  const std::vector<std::string>& waveNames() const { return names_; }
  // Constantes de la zone (marge d'un nœud) ; renvoie false si erreur de lecture
  bool loadRegion(double lonMin, double lonMax, double latMin, double latMax,
                  TideRegion& r, std::string& err);
  // Ondes mineures déduites par admittance (2Q1, σ1, ρ1, M11, M12, χ1, π1,
  // φ1, θ1, OO1, η2), comme le réglage par défaut de FES / pyfes
  void setInferMinor(bool on) { inferMinor_ = on; }

 private:
  void inferMinorWaves(TideRegion& r) const;
  bool inferMinor_ = true;
  bool readTile(int w, int tj, int ti, std::vector<int16_t>& re, std::vector<int16_t>& im,
                std::vector<uint8_t>& mask, bool& any, std::string& err);
  FILE* f_ = nullptr;
  std::string path_;
  std::vector<std::string> names_;
  int W_ = 0, T_ = 32, nx_ = 0, ny_ = 0, ntx_ = 0, nty_ = 0;
  double lon0_ = 0, lat0_ = 0, dlon_ = 0, dlat_ = 0, unit_ = 0.001;
  bool periodic_ = false;
  long long indexPos_ = 0;
};

// Référence de la marée maillée : niveau moyen (NM) au-dessus du ZH, Z0.
// Par nœud, la première source disponible l'emporte :
//   1. grille fournie par l'utilisateur (Z0 en m, positif) ;
//   2. surfaces ellipsoïdales NM/ell − ZH/ell (BathyElli du Shom) ;
//   3. plus basse mer astronomique FES sur 19 ans (approximation du ZH).
struct TideZ0Params {
  std::vector<std::string> userFiles;          // grille(s) ou points « lon lat Z0 »
  std::vector<std::string> nmFiles, zhFiles;   // BathyElli NM/ell et ZH/ell
  bool latFallback = true;
};

struct TideField;

class FesTide {
 public:
  // Prépare la zone (constantes + Z0) ; calcul long (plus basse mer) :
  // à lancer hors du fil d'interface. `progress` 0..1 facultatif.
  bool prepare(TideAtlas& atlas, double lonMin, double lonMax, double latMin, double latMax,
               const TideZ0Params& z0, std::string& err,
               const std::atomic<bool>* cancel = nullptr, std::atomic<float>* progress = nullptr);
  bool ready() const { return !region_.empty(); }
  // Hauteur d'eau au-dessus du ZH aux nœuds, à l'instant t (rapide)
  void field(double unixSeconds, TideField& out) const;
  // Hauteur FES au-dessus du NM, et Z0, en un point
  double heightNM(double lon, double lat, double unixSeconds) const;
  double z0At(double lon, double lat) const { return region_.sample(z0_, lon, lat); }
  const TideRegion& region() const { return region_; }
  const std::string& info() const { return info_; }  // origine de Z0
  double bbox[4] = {0, 0, 0, 0};                      // emprise demandée

 private:
  TideRegion region_;
  std::vector<float> z0_;  // [j][i], NaN hors modèle
  std::string info_;
};

// Plus basse mer astronomique (m, négative, sous le NM) aux nœuds `nodes`
// de la zone, sur 19 ans à pas horaire puis affinée à 5 min.
void LowestAstronomicalTide(const TideRegion& r, const std::vector<size_t>& nodes,
                            std::vector<float>& lat, const std::atomic<bool>* cancel = nullptr,
                            std::atomic<float>* progress = nullptr);

}  // namespace pd

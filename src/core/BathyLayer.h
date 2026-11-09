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

// Un jeu de données chargé (nuage + grille + index), chargé en arrière-plan.
#pragma once
#include <atomic>
#include <functional>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "BathyGrid.h"
#include "PointCloud.h"
#include "Soundings.h"
#include "VerticalRef.h"

namespace pd {

struct LayerParams {
  std::string path;
  // Système de coordonnées : code EPSG ou chaîne PROJ ("" = WGS84 lon/lat)
  std::string crsDef;
  std::string crsName;   // libellé affiché
  bool readPrj = true;   // un fichier .prj voisin, s'il existe, a priorité
  LoadOptions load;
  GridParams grid;
  bool useCache = true;       // cache .pdpc à côté du fichier source
  float exaggeration = 3.f;   // ombrage
  int shadeSmooth = 0;        // adoucissement du relief ombré (cellules)
  VerticalRefParams vref;     // conversion d'une référence terrestre vers le ZH
  // Bornes d'import : profondeurs ZH finales (m, positives vers le bas, après
  // signe, décalage et référence verticale) ; les autres points sont ignorés
  bool depthBounds = false;
  float depthMin = -200.f, depthMax = 200.f;
  bool cloudOnly = false;     // import en base : nuage converti seulement (pas de grille)
  // Couche calculée (vue de la base) : remplit le nuage aux centres de
  // cellules régulières (pas dlon × dlat) et la source de chaque sonde
  std::function<bool(PointCloud&, double& dlon, double& dlat, std::vector<int64_t>& src,
                     std::vector<uint8_t>& interp, std::string& err)> custom;
  std::string label;          // nom affiché d'une couche calculée
  const std::atomic<bool>* stop = nullptr;  // arrêt externe (import en base)
  // Import en base : date du levé imposée (sinon déduite du nom du fichier),
  // classe imposée, réglages d'import sérialisés (gardés avec la source)
  std::string surveyDate, group, importPrefs;
  // Import en base : résolution de stockage imposée (m), 0 = estimée
  double storeRes = 0;
  // Import dans la base : type d'instrument (Instrument), nature (0 auto,
  // 1 grille, 2 nuage), surface interpolée entre les sondes d'un nuage
  int instrument = 0, nature = 0;
  bool interpolate = true;
};

class BathyLayer {
 public:
  enum State { Idle, Loading, Ready, Failed };

  explicit BathyLayer(const LayerParams& p) : params(p) {}
  ~BathyLayer();
  BathyLayer(const BathyLayer&) = delete;
  BathyLayer& operator=(const BathyLayer&) = delete;

  void startLoad();  // asynchrone
  void cancel() { cancel_ = true; }
  void loadSync() { cancel_ = false; state_ = Loading; run(); }  // dans le fil appelant
  // Très gros fichiers XYZ texte (import dans la base) : lecture par blocs
  // d'environ `blockN` sondes, converties (système, signe, hauteurs d'eau,
  // référence verticale, bornes) puis remises à `sink` ; rien n'est gardé.
  // `sink` renvoie false pour arrêter. Faux si erreur (ou aucune sonde).
  static bool IsStreamable(const LayerParams& p);
  bool streamXYZ(size_t blockN, const std::function<bool(PointCloud&)>& sink, std::string& err);
  State state() const { return state_.load(std::memory_order_acquire); }
  float progress() const { return progress_.load(); }
  const std::string& error() const { return error_; }  // valide si Failed
  std::string summary() const;  // texte pour l'interface
  const std::string& crsUsed() const { return crsUsed_; }  // système effectif

  LayerParams params;
  bool visible = true;
  // Accessibles en lecture seulement quand state() == Ready
  PointCloud cloud;
  BathyGrid grid;
  SoundingIndex index;
  std::vector<int64_t> pointSource;  // source de chaque sonde (vue de la base), sinon vide
  std::vector<uint8_t> pointInterp;  // 1 = valeur interpolée (vue de la base), sinon vide

  // Valeur sous le curseur : grille pleine résolution + sonde réelle proche
  bool probe(double lon, double lat, double radiusDeg, float& gridZ,
             float& soundZ, double& soundDistM, int64_t* sourceId = nullptr,
             bool* gridInterp = nullptr) const;

 private:
  void run();
  void setupCrs(LoadOptions& lo);  // .prj voisin ou choix de l'utilisateur (exception si invalide)
  bool buildLattice(const LoadOptions& lo, LatticeSource& L);
  std::thread th_;
  std::atomic<State> state_{Idle};
  std::atomic<float> progress_{0.f};
  std::atomic<bool> cancel_{false};
  bool stopped() const { return cancel_ || (params.stop && *params.stop); }
  const std::atomic<bool>* stopFlag() { return params.stop ? params.stop : &cancel_; }
  std::string error_;
  std::string crsUsed_;
  std::string vrefInfo_;  // résumé de la conversion verticale appliquée
};

using LayerPtr = std::shared_ptr<BathyLayer>;

}  // namespace pd

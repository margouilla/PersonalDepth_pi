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

// PersonalDepth_pi — bathymétrie personnelle dynamique pour OpenCPN
// (équivalent fonctionnel du module PBG de TimeZero)
#pragma once
#include <wx/wx.h>
#include <wx/timer.h>

#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <set>
#include <thread>
#include <vector>

#include "ocpn_plugin.h"
#include "core/BathyDb.h"
#include "core/BathyLayer.h"
#include "core/ColorRamp.h"
#include "core/TideFES.h"
#include "core/TideField.h"

#ifndef OVERLAY_CHARTS  // absent des en-têtes API 1.18, présent dans les plus récents
#define OVERLAY_CHARTS 256
#endif
#ifndef OVERLAY_OVER_UI
#define OVERLAY_OVER_UI 128
#endif

#define PLUGIN_VERSION_MAJOR 0
#define PLUGIN_VERSION_MINOR 1
#define PLUGIN_VERSION_PATCH 0

class BathyDialog;
class TideChartDialog;
class BathyRenderer;

// Réglages d'affichage : tout ceci est appliqué à chaud, sans recalcul des grilles
struct DisplaySettings {
  pd::ColorRamp ramp = pd::ColorRamp::Preset("TimeZero PBG (bleus)");
  int opacity = 85;           // %
  bool hillshade = true;
  float shadeStrength = 0.6f; // 0..1
  float exaggeration = 3.f;   // nécessite un recalcul de l'ombrage (rapide)
  int shadeSmooth = 1;        // adoucissement du relief (cellules), idem
  bool soundings = true;
  int decimals = 1;
  int spacingPx = 34;         // densité des sondes chiffrées
  int fontSize = 9;
  bool smooth = true;         // profondeur et ombrage interpolés entre cellules
  bool legend = true;         // lecture au curseur
  bool banner = true;         // bandeau de palette sur la carte
  int bannerPos = 0;          // 0 bas centré, 1 haut centré, 2 bas gauche
  unsigned version = 1;       // incrémenté à chaque changement de couleurs
  // Isobathes (calculées à la volée depuis la grille, mises en cache)
  bool contours = true;
  int contourMode = 0;        // 0 = intervalle régulier, 1 = seuils de la palette
  float contourStep = 1.f;    // m
  float contourMajor = 5.f;   // isobathe maîtresse tous les ... m (0 = aucune)
  bool contourLabels = true;
  bool contourSmooth = true;
  uint32_t contourColor = pd::RGBA(25, 45, 95);
  unsigned contourVersion = 1;  // incrémenté à chaque changement de réglage
  // Dernier système de coordonnées choisi pour l'import
  std::string crsCode = "EPSG:4326";  // code du catalogue, ou "CUSTOM"
  int crsZone = 31;
  bool crsSouth = false;
  std::string crsCustom;
  bool readPrj = true;
  // Référence verticale proposée à l'import
  int vrefMode = 0;                 // 0 ZH, 1 IGN69/IGN78C France, 2 ellipsoïde, 3 surfaces, 4 grille d'écart
  // Lecture des fichiers à l'import
  int importOrder = 0;              // LoadOptions::Order
  int importSign = 0;               // LoadOptions::ZSign
  float importOffset = 0.f;         // décalage vertical (m)
  bool importZFeet = false;         // unité verticale des fichiers importés : pieds (sinon m)
  std::string importFormat;         // format personnalisé des fichiers texte (TextFormat), "" = X Y Z
  float importRes = 0.f;            // résolution de stockage imposée (m), 0 = automatique
  bool importCache = true;          // cache binaire .pdpc
  bool importNodataOn = false;      // NODATA supplémentaires (Z brut du fichier)
  std::string importNodata = "-9999 9999 -99999 99999";
  bool importBoundsOn = false;      // bornes d'import (profondeur ZH finale)
  float boundMin = -200.f, boundMax = 200.f;  // communes à l'import et à « Nettoyer »
  std::string vrefFranceDir;        // dossier des fichiers BathyElli v2.1 + RAF20
  std::string vrefZh, vrefGeoid, vrefDirect;  // fichiers (mode 2 et 3) ; ';' entre fichiers ZH
  bool vrefInvert = false;
  // Marée (stations OpenCPN) : profondeur affichée = profondeur ZH + hauteur d'eau
  int tideMode = 0;                 // 0 aucune, 1 maintenant, 2 date choisie
  long long tideTime = 0;           // date choisie (secondes UTC)
  int tideShiftMin = 0;             // décalage du curseur horaire (minutes)
  int tideSource = 0;               // 0 stations OpenCPN, 1 FES2022 maillé
  std::string tideAtlas;            // fichier .pdtide (tools/pdtide_convert.py)
  std::string tideZ0Files;          // grille(s) « NM au-dessus du ZH » (';'), facultatif
  bool tideUseBathyElli = true;     // Z0 = BathyElli NM/ell − ZH/ell (dossier France)
  // Palette automatique : seuils recalculés d'après les profondeurs présentes
  bool autoPalette = false;
  int autoSource = 0;               // 0 toutes les données (base + couches), 1 zone affichée
  int autoCount = 10;               // nombre de seuils
  bool autoNice = true;             // valeurs rondes (1-2-2,5-5)
  std::string autoBase;             // couleurs de référence (rampe sérialisée)
  bool autoBaseFresh = false;       // (non sauvegardé) autoBase = couleurs affichées
  bool autoMinOn = false, autoMaxOn = false;  // bornes imposées (m)
  float autoMin = 0.f, autoMax = 50.f;
  // Base de bathymétrie (sources fusionnées)
  std::string dbFolder;             // dossier des fichiers PD_*.pdb
  int dbPriority = 0;               // pd::DbPriority
  std::set<std::string> dbHidden;   // fichiers .pdb masqués à l'affichage
  // --- calculés (non sauvegardés)
  // Unités d'affichage d'OpenCPN (Options › Affichage › Unités) : les valeurs
  // restent en mètres partout, multipliées par ces facteurs à l'affichage.
  // Les brasses ne sont pas gérées : affichées en pieds.
  float depthK = 1.f;               // mètres -> unité de profondeur affichée
  wxString depthUnit = "m";
  float heightK = 1.f;              // mètres -> unité de hauteur (marée)
  wxString heightUnit = "m";
  float tideH = 0.f;                // hauteur d'eau appliquée à l'affichage (m)
  bool tideOk = false;
  wxString tideInfo;                // texte du panneau
  wxString tideLegend;              // ligne ASCII de la légende
  std::shared_ptr<const pd::TideField> tide;  // hauteur d'eau au-dessus du ZH (affichage)
};

// Niveaux d'isobathes effectifs (avec l'isobathe de sécurité si active)
std::vector<float> ContourLevels(const DisplaySettings& s, float zMin, float zMax);

// Réglages d'import d'une source (système, référence verticale, lecture…)
// sérialisés « clé=valeur » et gardés dans la base, pour relire le fichier
// d'origine avec d'autres paramètres
// Point de marée tracé dans la courbe de marée
struct TideChartPoint {
  wxString name;
  double lon = 0, lat = 0;
};

std::string SerializeImportPrefs(const DisplaySettings& s);
bool ApplyImportPrefs(const std::string& text, DisplaySettings& s);

class PersonalDepth_pi : public opencpn_plugin_118 {
 public:
  explicit PersonalDepth_pi(void* ppimgr);
  ~PersonalDepth_pi() override;

  int Init() override;
  bool DeInit() override;
  int GetAPIVersionMajor() override { return 1; }
  int GetAPIVersionMinor() override { return 18; }
  int GetPlugInVersionMajor() override { return PLUGIN_VERSION_MAJOR; }
  int GetPlugInVersionMinor() override { return PLUGIN_VERSION_MINOR; }
  int GetPlugInVersionPatch() override { return PLUGIN_VERSION_PATCH; }
  wxBitmap* GetPlugInBitmap() override { return &m_icon; }
  wxString GetCommonName() override { return "PersonalDepth"; }
  wxString GetShortDescription() override;
  wxString GetLongDescription() override;

  int GetToolbarToolCount() override { return 1; }
  void OnToolbarToolCallback(int id) override;
  void ShowPreferencesDialog(wxWindow* parent) override;
  void SetCursorLatLon(double lat, double lon) override;

  using opencpn_plugin_118::RenderOverlayMultiCanvas;
  using opencpn_plugin_118::RenderGLOverlayMultiCanvas;
  bool RenderOverlayMultiCanvas(wxDC& dc, PlugIn_ViewPort* vp, int canvasIndex,
                                int priority) override;
  bool RenderGLOverlayMultiCanvas(wxGLContext* ctx, PlugIn_ViewPort* vp,
                                  int canvasIndex, int priority) override;

  // --- API interne pour le dialogue
  std::vector<pd::LayerPtr>& Layers() { return m_layers; }
  // Base de bathymétrie
  const std::vector<pd::DbSource>& DbSources() const { return m_dbSources; }
  bool DbBusy() const { return m_dbBusy; }
  wxString DbStatus();
  void DbOpen();                                         // (re)lit la liste des sources
  const std::vector<pd::DbCellInfo>& DbCells() const { return m_dbCells; }
  double DbCellDeg() const { return m_dbCellDeg; }
  // `replace[i]` : sources remplacées par le fichier i (réimport), vide sinon
  void DbImport(const std::vector<pd::LayerParams>& files, const std::string& group,
                const std::vector<std::vector<int64_t>>& replace = {});
  float DbProgressValue() const { return m_dbBusy ? m_dbProgress.load() : -1.f; }
  void DbStop() { m_dbCancel = true; }
  // Courbe de marée : points utilisés (station OpenCPN, ou 9 points FES sur
  // l'emprise des données), hauteurs au-dessus du ZH (m), instant choisi
  std::vector<TideChartPoint> TideChartPoints(wxString& source, wxString& why);
  void TideChartSeries(const std::vector<TideChartPoint>& pts, const std::vector<time_t>& t,
                       std::vector<std::vector<double>>& h);
  time_t TideChartTime() const;
  void ShowTideChart();
  // date du levé (AAAA-MM-JJ) et/ou classe, sans relire les fichiers (vide = inchangé) ;
  // params : réglages d'import déclarés pour les sources qui n'en ont pas encore
  void DbEditMeta(const std::vector<int64_t>& ids, const std::string& date, const std::string& group,
                  const std::string& params = std::string());                    // arrêt de l'opération en cours
  void DbSetCellsVisible(const std::vector<std::string>& names, bool visible);
  void DbZoomToCells(const std::vector<std::string>& names);
  void DbResplit(double deg);
  void DbRemove(const std::vector<int64_t>& ids);
  void DbClean(const std::vector<int64_t>& ids, float zMin, float zMax);
  void DbSetVisible(const std::vector<int64_t>& ids, bool visible);
  void DbMove(const std::vector<int64_t>& ids, int delta);   // rang manuel (bloc)
  void DbSetGroup(const std::vector<int64_t>& ids, const std::string& group);
  void DbZoomTo(const std::vector<int64_t>& ids);
  void DbSetPriority(int prio);
  // Isobathes peu profonde / de sécurité / profonde des cartes vectorielles
  // d'OpenCPN (Options › Cartes › Cartes vectorielles), en mètres
  void EncContours(float& shallow, float& safety, float& deep);
  void AutoPaletteChanged();                             // réglages de la palette automatique
  void SetAutoPalette(bool on);                          // bascule A / M (panneau ou bandeau)
  void ApplyPreset(const std::string& name);             // préréglage de couleurs
  // Bandeau de palette : clics sur la carte
  bool MouseEventHook(wxMouseEvent& event) override;
  wxString AutoPaletteInfo() const { return m_autoInfo; }
  DisplaySettings& Settings() { return m_set; }
  void AddLayer(const pd::LayerParams& p);
  void RemoveLayer(size_t i);
  void ZoomToLayer(size_t i);
  void ColorsChanged();            // palette, seuils, opacité, ombrage…
  void ExaggerationChanged();      // recalcul de l'ombrage
  void ViewChanged();              // affichage des sondes, légende…
  void OnDialogClose();
  void OnTick();                   // suivi des chargements
  void SaveConfig();
  void TideChanged();              // réglage de marée modifié dans le panneau
  void UpdateTide(bool findStation);
  static wxString DefaultReferenceDir();

 private:
  void LoadConfig();
  bool ShouldDraw(int priority);
  wxString CursorTextFor(int priority) const;
  std::vector<pd::LayerPtr> DrawLayers() const;          // vue de la base + couches fichiers
  void StartDbJob(const std::string& title,
                  std::function<bool(pd::BathyDb&, std::string&, const pd::DbProgress&)> job);
  void UpdateDbView();
  void UpdateAutoPalette(bool force);
  void BannerClick(int part, wxWindow* win, wxPoint pos);
  void BannerRange(bool isMin, wxWindow* win);
  void UpdateCursorText();
  bool UpdateUnits();  // unités d'affichage d'OpenCPN ; true si elles ont changé
  bool UpdateLanguage();  // langue d'OpenCPN (français / anglais) ; true si elle a changé
  int m_lang = -1;        // 0 français, 1 anglais

  wxBitmap m_icon;
  int m_toolId = -1;
  BathyDialog* m_dlg = nullptr;
  TideChartDialog* m_chart = nullptr;
  double m_tideBox[4] = {0, 0, 0, 0};  // emprise des données (lon0 lon1 lat0 lat1)
  bool m_tideBoxOk = false;
  std::unique_ptr<BathyRenderer> m_renderer;
  std::vector<pd::LayerPtr> m_layers;
  std::vector<int> m_lastState;
  DisplaySettings m_set;
  bool m_hostHasChartsPriority = false;
  double m_curLat = 0, m_curLon = 0, m_lastPpd = 1e5;
  wxString m_cursorText;
  class Ticker;
  std::unique_ptr<Ticker> m_timer;
  class CursorRefresh;
  std::unique_ptr<CursorRefresh> m_cursorRefresh;
  wxLongLong m_lastCursorRefresh = 0;
  // Marée
  int m_tideStation = -1;
  wxString m_tideStationName;
  double m_tideStationDistKm = 0, m_tideStationLat = 0, m_tideStationLon = 0;
  double m_tideRefLat = 1e9, m_tideRefLon = 1e9;
  long long m_lastTideMinute = 0;
  unsigned m_tideVersion = 0;
  // Marée maillée FES : zone préparée en tâche de fond (plus basse mer…)
  void StartFes(const std::string& key, const std::string& atlas, const double bbox[4],
                const pd::TideZ0Params& z0);
  void StopFes();
  std::shared_ptr<pd::FesTide> m_fes, m_fesNext;
  std::string m_fesKey, m_fesNextKey, m_fesErr, m_fesThreadErr;
  std::thread m_fesThread;
  std::atomic<bool> m_fesCancel{false}, m_fesBusy{false}, m_fesOk{false};
  std::atomic<float> m_fesProgress{0.f};
  double m_fieldT = -1;
  // Base : opérations en tâche de fond, une à la fois
  std::vector<pd::DbSource> m_dbSources, m_dbSourcesNew;
  std::vector<pd::DbCellInfo> m_dbCells, m_dbCellsNew;
  double m_dbCellDeg = pd::kDefaultCellDeg, m_dbCellDegNew = pd::kDefaultCellDeg;
  std::thread m_dbThread;
  std::atomic<bool> m_dbBusy{false}, m_dbCancel{false};
  std::atomic<float> m_dbProgress{0.f};
  std::mutex m_dbMx;
  std::string m_dbTitle, m_dbStep, m_dbErr, m_dbLast;
  unsigned m_dbEpoch = 1, m_dbViewEpoch = 0;
  // Vue de la base : couche calculée pour la zone affichée
  pd::LayerPtr m_dbView, m_dbViewNext;
  double m_dbViewBox[4] = {0, 0, 0, 0};
  int m_dbViewTarget = -1;
  double m_vpBox[4] = {0, 0, 0, 0};
  bool m_vpValid = false;
  std::string m_autoKey;
  wxString m_autoInfo;
  const pd::FesTide* m_fieldFes = nullptr;
};

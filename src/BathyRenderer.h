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

// Rendu OpenGL (tuiles texturées recolorées à la volée) et wxDC (image cache).
#pragma once
#include <wx/wx.h>

#include <functional>
#include <map>
#include <memory>
#include <set>
#include <tuple>
#include <vector>

#include "ocpn_plugin.h"
#include "core/BathyLayer.h"
#include "core/ColorRamp.h"
#include "core/Contours.h"
#include "core/Soundings.h"

struct DisplaySettings;

// Bandeau de palette posé sur la carte : zones cliquables (pixels du canevas,
// tels que les donne le point de vue d'OpenCPN)
struct BannerHit {
  // parties : bathymétrie (Min… Bar), imagerie (ImgMin… ImgBar), communes
  enum Part { NoPart = 0, Min, Mode, Source, Max, Bar, Gear, Orient, Body,
              ImgMin, ImgMode, ImgSource, ImgMax, ImgBar,
              // ligne isobathes / sondes
              CtOn, CtStep, CtMajor, SdOn, SdDec, SdSize };
  struct Zones {
    bool on = false;
    wxRect minR, modeR, srcR, maxR, barR;
  };
  bool valid = false;
  wxRect box, gearR, orientR;
  Zones bathy, img;
  std::vector<std::pair<wxRect, Part>> items;  // ligne isobathes / sondes
  int pixW = 0, pixH = 0;  // taille du point de vue (rapport avec les coordonnées souris)
  Part PartAt(const wxPoint& p) const {
    if (!valid || !box.Contains(p)) return NoPart;
    if (gearR.Contains(p)) return Gear;
    if (orientR.Contains(p)) return Orient;
    for (int k = 0; k < 2; ++k) {
      const Zones& z = k ? img : bathy;
      if (!z.on) continue;
      if (z.minR.Contains(p)) return k ? ImgMin : Min;
      if (z.modeR.Contains(p)) return k ? ImgMode : Mode;
      if (!z.srcR.IsEmpty() && z.srcR.Contains(p)) return k ? ImgSource : Source;
      if (z.maxR.Contains(p)) return k ? ImgMax : Max;
      if (z.barR.Contains(p)) return k ? ImgBar : Bar;
    }
    for (const auto& it : items)
      if (it.first.Contains(p)) return it.second;
    return Body;
  }
};

class BathyRenderer {
 public:
  BathyRenderer();
  ~BathyRenderer();

  // parts : tuiles colorées (kTiles), isobathes, sondes, bandeau et curseur
  // (kOverlays) ; deux passes quand une autre couche (imagerie) s'intercale
  enum Parts { kTiles = 1, kOverlays = 2, kAll = 3 };
  void RenderGL(PlugIn_ViewPort* vp, const std::vector<pd::LayerPtr>& layers,
                const DisplaySettings& s, const wxString& cursorText, int parts = kAll);
  void RenderDC(wxDC& dc, PlugIn_ViewPort* vp, const std::vector<pd::LayerPtr>& layers,
                const DisplaySettings& s, const wxString& cursorText, int parts = kAll);
  // Étiquette du curseur seule (passe d'interface, jamais mise en cache)
  void RenderCursorGL(PlugIn_ViewPort* vp, const wxString& text, int fontSize);
  // Bandeau seul (imagerie affichée sans bathymétrie)
  void RenderBannerGL(PlugIn_ViewPort* vp, const DisplaySettings& s);
  void RenderBannerDC(wxDC& dc, PlugIn_ViewPort* vp, const DisplaySettings& s);
  void RenderCursorDC(wxDC& dc, PlugIn_ViewPort* vp, const wxString& text, int fontSize);
  // Position du curseur pour l'étiquette de lecture (on=false : masquée)
  void SetCursor(bool on, double lat, double lon) { m_curOn = on; m_curLat = lat; m_curLon = lon; }
  // couches ajoutées / retirées ; la vue adoptée (AdoptPending) garde ses textures et isobathes
  void Invalidate() {
    ++m_epoch;
    for (auto it = m_contours.begin(); it != m_contours.end();)
      it = (it->first.first && it->first.first == m_keepLayer) ? std::next(it) : m_contours.erase(it);
    m_labelKey.clear();
  }
  // Nouvelle vue de la base préparée en tâche de fond (OpenGL) avant d'être affichée
  void SetPending(const pd::LayerPtr& l) { m_pending = l; m_pendingDone = !l; }
  bool PendingDone() const { return !m_pending || m_pendingDone; }
  const pd::LayerPtr& Pending() const { return m_pending; }
  void ClearPending() { m_pending.reset(); m_pendingDone = true; }
  // couleurs de profondeur masquées (isobathes et sondes seules) : la vue en
  // attente n'a que ses isobathes à préparer
  void SetTilesHidden(bool h) { m_tilesHidden = h; }
  void AdoptPending() {  // la vue en attente devient la vue affichée
    m_keepLayer = m_pending.get();
    m_pending.reset();
    m_pendingDone = true;
    Invalidate();
  }
  // Sondes et valeurs : décalées pendant un déplacement, recalculées à l'arrêt
  bool LabelsShifted() const { return m_labelsShifted; }
  void RefreshLabels() { m_labelKey.clear(); m_labelStatic.clear(); }
  // Mesures de temps (journal PerfLog) : cumulées depuis la dernière remise à zéro
  struct Perf {
    int frames = 0, uploads = 0, labelRuns = 0;
    double totalMs = 0, maxMs = 0, uploadMs = 0, contourMs = 0, labelMs = 0, tileMs = 0;
  };
  Perf m_perf;
  void ReleaseGL();                 // à appeler avec un contexte GL courant
  double LastPixelsPerDegLon() const { return m_ppd; }
  const BannerHit& Banner() const { return m_banner; }
  void HideBanner() { m_banner.valid = false; }

 private:
  struct Tile;
  struct Glyphs;
  struct SmoothGL;  // rendu lissé par shader (profondeur interpolée sur la carte graphique)
  bool InitSmoothGL();
  void UploadDepthTile(Tile& t, const pd::GridLevel& g, const pd::BathyLayer& l, int ti, int tj);
  bool EnsureTile(Tile& t, const pd::GridLevel& g, const pd::BathyLayer& l, int ti, int tj, bool shader,
                  const DisplaySettings& s, std::vector<uint32_t>& buf);
  void PrewarmPending(PlugIn_ViewPort* vp, const DisplaySettings& s, bool shader, std::vector<uint32_t>& buf);
  pd::LayerPtr m_pending;
  bool m_pendingDone = true;
  bool m_tilesHidden = false;
  const void* m_keepLayer = nullptr;
  // sondes : position géographique de chaque étiquette, pour les décaler
  std::vector<double> m_labelLon, m_labelLat;
  wxString m_labelStatic;  // clé des étiquettes sans le centre de la vue
  bool m_labelsShifted = false;
  bool PrepareLUT(const std::vector<pd::LayerPtr>& layers, const DisplaySettings& s);
  bool FitMapper(PlugIn_ViewPort* vp, pd::ScreenMapper& sm);
  std::vector<pd::Label> CollectLabels(PlugIn_ViewPort* vp, const pd::ScreenMapper& sm,
                                       const std::vector<pd::LayerPtr>& layers,
                                       const DisplaySettings& s);
  static void FormatDepth(float z, int decimals, wxString& ip, wxString& dp);
  void DrawGLText(float x, float y, const wxString& t, float scale, bool center);
  float GLTextWidth(const wxString& t, float scale);
  void EnsureGlyphs(const wxString& t);
  // Bandeau : une seule mise en page, dessinée en OpenGL ou en wxDC
  void DrawBanner(struct BannerPainter& p, PlugIn_ViewPort* vp, const DisplaySettings& s);
  void DrawBannerGL(PlugIn_ViewPort* vp, const DisplaySettings& s);
  void DrawBannerDC(wxDC& dc, PlugIn_ViewPort* vp, const DisplaySettings& s);
  void BuildGlyphs(int fontSize);
  void DrawCursorGL(PlugIn_ViewPort* vp, const wxString& text);
  void DrawCursorDC(wxDC& dc, PlugIn_ViewPort* vp, const wxString& text);
  bool CursorBox(PlugIn_ViewPort* vp, int w, int h, int& x, int& y) const;
  const std::vector<pd::Isoline>& Isolines(const pd::BathyLayer& l, int lev,
                                            const DisplaySettings& s);
  int ContourLevelIndex(const pd::BathyLayer& l) const;
  void AddContourLabels(PlugIn_ViewPort* vp, const pd::ScreenMapper& sm,
                        const std::vector<pd::LayerPtr>& layers, const DisplaySettings& s,
                        std::vector<pd::Label>& out);
  void DrawContoursGL(PlugIn_ViewPort* vp, const pd::ScreenMapper& sm,
                      const std::vector<pd::LayerPtr>& layers, const DisplaySettings& s);
  void DrawContoursDC(wxDC& dc, PlugIn_ViewPort* vp, const pd::ScreenMapper& sm,
                      const std::vector<pd::LayerPtr>& layers, const DisplaySettings& s);
  wxString LabelKey(PlugIn_ViewPort* vp, const std::vector<pd::LayerPtr>& layers,
                    const DisplaySettings& s, char mode, bool center = true) const;

  pd::ColorLUT m_lut;
  unsigned m_lutVersion = 0, m_lutStamp = 0, m_tideVersion = ~0u;
  float m_lutMin = 0, m_lutMax = 0, m_lutOffset = 0;
  unsigned m_epoch = 1, m_purgedEpoch = 1;
  double m_ppd = 1e5;
  bool m_curOn = false;
  BannerHit m_banner;
  bool m_glLight = false;  // texte GL clair (sur fond sombre)
  bool m_glPlain = false;  // texte GL sans halo, teinté par glColor
  // Sondes déjà sélectionnées et désencombrées pour la vue courante
  std::vector<pd::Label> m_labels;
  // Isobathes d'une couche ; projetées une fois en (lon, y Mercator) relatifs à
  // (ox, oy) : à chaque image, une seule transformation affine (matrice GL)
  // au lieu d'un log/tan par point (déplacement et zoom fluides)
  struct ContourRun {
    uint32_t first, count;
    float x0, y0, x1, y1;  // emprise (coordonnées relatives)
    float level;
  };
  struct ContourCache {
    wxString key;
    std::vector<pd::Isoline> lines;
    double ox = 0, oy = 0;
    std::vector<float> uv;
    std::vector<ContourRun> runs;
  };
  const ContourCache& Contours(const pd::BathyLayer& l, const DisplaySettings& s);
  std::map<std::pair<const void*, int>, ContourCache> m_contours;
  wxString m_labelKey;
  double m_curLat = 0, m_curLon = 0;

  // GL
  std::map<std::tuple<const void*, int, int, int>, std::unique_ptr<Tile>> m_tiles;
  unsigned long m_frame = 0;
  std::unique_ptr<Glyphs> m_glyphs;
  std::set<uint32_t> m_glyphExtra;  // caractères ajoutés à l'atlas à la demande
  std::unique_ptr<SmoothGL> m_sgl;

  // DC
  wxBitmap m_dcBitmap;
  wxString m_dcKey;
};

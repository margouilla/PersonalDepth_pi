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
  enum Part { None = 0, Min, Mode, Source, Max, Bar, Gear, Orient, Body };
  bool valid = false;
  wxRect box, minR, modeR, srcR, maxR, barR, gearR, orientR;
  int pixW = 0, pixH = 0;  // taille du point de vue (rapport avec les coordonnées souris)
  Part PartAt(const wxPoint& p) const {
    if (!valid || !box.Contains(p)) return None;
    if (minR.Contains(p)) return Min;
    if (modeR.Contains(p)) return Mode;
    if (!srcR.IsEmpty() && srcR.Contains(p)) return Source;
    if (maxR.Contains(p)) return Max;
    if (gearR.Contains(p)) return Gear;
    if (orientR.Contains(p)) return Orient;
    if (barR.Contains(p)) return Bar;
    return Body;
  }
};

class BathyRenderer {
 public:
  BathyRenderer();
  ~BathyRenderer();

  void RenderGL(PlugIn_ViewPort* vp, const std::vector<pd::LayerPtr>& layers,
                const DisplaySettings& s, const wxString& cursorText);
  void RenderDC(wxDC& dc, PlugIn_ViewPort* vp, const std::vector<pd::LayerPtr>& layers,
                const DisplaySettings& s, const wxString& cursorText);
  // Étiquette du curseur seule (passe d'interface, jamais mise en cache)
  void RenderCursorGL(PlugIn_ViewPort* vp, const wxString& text, int fontSize);
  void RenderCursorDC(wxDC& dc, PlugIn_ViewPort* vp, const wxString& text, int fontSize);
  // Position du curseur pour l'étiquette de lecture (on=false : masquée)
  void SetCursor(bool on, double lat, double lon) { m_curOn = on; m_curLat = lat; m_curLon = lon; }
  void Invalidate() { ++m_epoch; m_contours.clear(); m_labelKey.clear(); }  // couches ajoutées / retirées
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
  bool PrepareLUT(const std::vector<pd::LayerPtr>& layers, const DisplaySettings& s);
  bool FitMapper(PlugIn_ViewPort* vp, pd::ScreenMapper& sm);
  std::vector<pd::Label> CollectLabels(PlugIn_ViewPort* vp, const pd::ScreenMapper& sm,
                                       const std::vector<pd::LayerPtr>& layers,
                                       const DisplaySettings& s);
  static void FormatDepth(float z, int decimals, wxString& ip, wxString& dp);
  void DrawGLText(float x, float y, const wxString& t, float scale, bool center);
  float GLTextWidth(const wxString& t, float scale) const;
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
                    const DisplaySettings& s, char mode) const;

  pd::ColorLUT m_lut;
  unsigned m_lutVersion = 0, m_lutStamp = 0, m_tideVersion = ~0u;
  float m_lutMin = 0, m_lutMax = 0, m_lutOffset = 0;
  unsigned m_epoch = 1, m_purgedEpoch = 1;
  double m_ppd = 1e5;
  bool m_curOn = false;
  BannerHit m_banner;
  bool m_glLight = false;  // texte GL clair (sur fond sombre)
  // Sondes déjà sélectionnées et désencombrées pour la vue courante
  std::vector<pd::Label> m_labels;
  struct ContourCache {
    wxString key;
    std::vector<pd::Isoline> lines;
  };
  std::map<std::pair<const void*, int>, ContourCache> m_contours;
  wxString m_labelKey;
  double m_curLat = 0, m_curLon = 0;

  // GL
  std::map<std::tuple<const void*, int, int, int>, std::unique_ptr<Tile>> m_tiles;
  unsigned long m_frame = 0;
  std::unique_ptr<Glyphs> m_glyphs;
  std::unique_ptr<SmoothGL> m_sgl;

  // DC
  wxBitmap m_dcBitmap;
  wxString m_dcKey;
};

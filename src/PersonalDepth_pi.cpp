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

#include "PersonalDepth_pi.h"

#include <wx/dir.h>
#include <wx/fileconf.h>
#include <wx/filename.h>
#include <wx/time.h>
#include <wx/tokenzr.h>
#include <wx/utils.h>

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <set>
#include <cstdio>
#include <ctime>

#include "BathyDialog.h"
#include "BathyRenderer.h"
#include "core/WebMerc.h"

#define U8(s) wxString::FromUTF8(s)

using namespace pd;

extern "C" DECL_EXP opencpn_plugin* create_pi(void* ppimgr) {
  return new PersonalDepth_pi(ppimgr);
}
extern "C" DECL_EXP void destroy_pi(opencpn_plugin* p) { delete p; }

class PersonalDepth_pi::Ticker : public wxTimer {
 public:
  explicit Ticker(PersonalDepth_pi* pi) : m_pi(pi) {}
  void Notify() override { m_pi->OnTick(); }
 private:
  PersonalDepth_pi* m_pi;
};

// Redessin différé pour l'étiquette du curseur (au plus ~25 par seconde)
class PersonalDepth_pi::CursorRefresh : public wxTimer {
 public:
  void Notify() override { RequestRefresh(GetOCPNCanvasWindow()); }
};

static wxBitmap MakeIcon() {  // petite rampe bleue, sans fichier image
  wxImage img(32, 32);
  for (int y = 0; y < 32; ++y)
    for (int x = 0; x < 32; ++x) {
      double d = (x + y) / 62.0;
      bool dot = ((x / 6 + y / 6) % 2 == 0) && x % 6 == 3 && y % 6 == 3;
      img.SetRGB(x, y, dot ? 10 : (int)(190 - 180 * d), dot ? 20 : (int)(235 - 185 * d),
                 dot ? 50 : (int)(255 - 125 * d));
    }
  return wxBitmap(img);
}

PersonalDepth_pi::PersonalDepth_pi(void* ppimgr)
    : opencpn_plugin_118(ppimgr), m_icon(MakeIcon()) {}

PersonalDepth_pi::~PersonalDepth_pi() = default;

wxString PersonalDepth_pi::GetShortDescription() {
  return U8("Bathymétrie personnelle dynamique (nuages XYZ, grilles, TIN)");
}

wxString PersonalDepth_pi::GetLongDescription() {
  return U8("Affiche de volumineux levés bathymétriques personnels (XYZ, ESRI ASCII, "
            "LandXML TIN, GeoTIFF/BAG avec GDAL) sous forme de surface colorée ombrée "
            "et de sondes chiffrées exactes. Palette, seuils de profondeur, zone "
            "dangereuse et ombrage se modifient instantanément, sans régénérer les "
            "données — à la manière du module PBG de TimeZero.");
}

int PersonalDepth_pi::Init() {
  m_renderer = std::make_unique<BathyRenderer>();
  LoadConfig();
  if (m_set.vrefFranceDir.empty()) m_set.vrefFranceDir = DefaultReferenceDir().ToUTF8().data();
  DbOpen();

  wxString svg = GetPluginDataDir("PersonalDepth_pi") + wxFileName::GetPathSeparator() +
                 "data" + wxFileName::GetPathSeparator() + "personaldepth.svg";
  if (wxFileExists(svg))
    m_toolId = InsertPlugInToolSVG("PersonalDepth", svg, svg, svg, wxITEM_CHECK,
                                   U8("Bathymétrie personnelle"), "", nullptr, -1, 0, this);
  else
    m_toolId = InsertPlugInTool("PersonalDepth", &m_icon, &m_icon, wxITEM_CHECK,
                                U8("Bathymétrie personnelle"), "", nullptr, -1, 0, this);

  m_timer = std::make_unique<Ticker>(this);
  m_timer->Start(250);
  m_cursorRefresh = std::make_unique<CursorRefresh>();

  return WANTS_OVERLAY_CALLBACK | WANTS_OPENGL_OVERLAY_CALLBACK | WANTS_CURSOR_LATLON |
         WANTS_TOOLBAR_CALLBACK | INSTALLS_TOOLBAR_TOOL | WANTS_CONFIG | WANTS_PREFERENCES;
}

bool PersonalDepth_pi::DeInit() {
  SaveConfig();
  if (m_timer) m_timer->Stop();
  m_timer.reset();
  if (m_cursorRefresh) m_cursorRefresh->Stop();
  m_cursorRefresh.reset();
  StopFes();
  m_dbCancel = true;
  if (m_dbThread.joinable()) m_dbThread.join();
  if (m_dbViewNext) m_dbViewNext->cancel();
  m_dbViewNext.reset();
  m_dbView.reset();
  if (m_dlg) { m_dlg->Destroy(); m_dlg = nullptr; }
  for (auto& l : m_layers) l->cancel();
  m_layers.clear();  // attend la fin des threads de chargement
  m_renderer.reset();
  return true;
}

// ------------------------------------------------------------- interface
void PersonalDepth_pi::OnToolbarToolCallback(int) {
  if (!m_dlg) m_dlg = new BathyDialog(GetOCPNCanvasWindow(), this);
  bool show = !m_dlg->IsShown();
  m_dlg->Show(show);
  if (show) m_dlg->Raise();
  SetToolbarItemState(m_toolId, show);
}

void PersonalDepth_pi::ShowPreferencesDialog(wxWindow*) {
  if (!m_dlg) m_dlg = new BathyDialog(GetOCPNCanvasWindow(), this);
  m_dlg->Show();
  m_dlg->Raise();
  SetToolbarItemState(m_toolId, true);
}

void PersonalDepth_pi::OnDialogClose() {
  SetToolbarItemState(m_toolId, false);
  SaveConfig();
}

void PersonalDepth_pi::AddLayer(const LayerParams& p) {
  auto l = std::make_shared<BathyLayer>(p);
  l->startLoad();
  m_layers.push_back(l);
  m_lastState.push_back(-1);
  SaveConfig();
}

void PersonalDepth_pi::RemoveLayer(size_t i) {
  if (i >= m_layers.size()) return;
  m_layers[i]->cancel();
  m_layers.erase(m_layers.begin() + i);
  m_lastState.erase(m_lastState.begin() + i);
  m_renderer->Invalidate();
  ColorsChanged();
}

void PersonalDepth_pi::ZoomToLayer(size_t i) {
  if (i >= m_layers.size() || m_layers[i]->state() != BathyLayer::Ready) return;
  const PointCloud& c = m_layers[i]->cloud;
  double lat = 0.5 * (c.latMin + c.latMax), lon = 0.5 * (c.lonMin + c.lonMax);
  double wm = (c.lonMax - c.lonMin) * 111320.0 * std::cos(lat * M_PI / 180.0);
  double hm = (c.latMax - c.latMin) * 110540.0;
  wxWindow* cv = GetOCPNCanvasWindow();
  wxSize sz = cv ? cv->GetClientSize() : wxSize(1000, 800);
  double ppm = 0.9 * std::min(sz.x / std::max(wm, 1.0), sz.y / std::max(hm, 1.0));
  JumpToPosition(lat, lon, ppm);
}

void PersonalDepth_pi::ColorsChanged() {
  ++m_set.version;  // la LUT et les tuiles visibles seront recolorées au prochain rendu
  SaveConfig();
  RequestRefresh(GetOCPNCanvasWindow());
}

void PersonalDepth_pi::ExaggerationChanged() {
  wxBusyCursor busy;
  for (auto& l : DrawLayers())
    if (l->state() == BathyLayer::Ready) {
      l->params.exaggeration = m_set.exaggeration;
      l->params.shadeSmooth = m_set.shadeSmooth;
      l->grid.shadeSmooth = m_set.shadeSmooth;
      l->grid.computeHillshade(m_set.exaggeration);
    }
  m_renderer->Invalidate();  // l'ombrage est stocké dans les textures lissées
  ColorsChanged();
}

void PersonalDepth_pi::ViewChanged() {
  SaveConfig();
  RequestRefresh(GetOCPNCanvasWindow());
}

void PersonalDepth_pi::OnTick() {
  bool changed = false, loading = false;
  for (size_t i = 0; i < m_layers.size(); ++i) {
    int st = (int)m_layers[i]->state();
    if (st == BathyLayer::Loading) loading = true;
    if (st != m_lastState[i]) {
      m_lastState[i] = st;
      changed = true;
      if (st == BathyLayer::Ready) {
        ColorsChanged();  // plage de profondeur élargie
        if (m_set.tideMode) UpdateTide(true);
      }
    }
  }
  if (m_dlg && m_dlg->IsShown() && (changed || loading)) m_dlg->RefreshLayers();
  // base : fin d'une opération, puis mise à jour de la vue
  if (m_dbThread.joinable() && !m_dbBusy) {
    m_dbThread.join();
    m_dbSources = m_dbSourcesNew;
    m_dbCells = m_dbCellsNew;
    m_dbCellDeg = m_dbCellDegNew;
    ++m_dbEpoch;
    {
      std::lock_guard<std::mutex> lk(m_dbMx);
      m_dbLast = m_dbErr.empty() ? m_dbTitle + U8(" : terminé").ToStdString(wxConvUTF8)
                                 : m_dbTitle + " : " + m_dbErr;
    }
    if (m_dlg) m_dlg->RefreshDb();
    if (m_set.tideMode) UpdateTide(false);
  } else if (m_dbBusy && m_dlg) {
    m_dlg->SetDbStatus(DbStatus());
  }
  UpdateDbView();
  UpdateAutoPalette(false);
  // marée maillée : fin de préparation de zone
  if (m_fesThread.joinable() && !m_fesBusy) {
    m_fesThread.join();
    const bool cancelled = m_fesCancel;
    if (m_fesOk && !cancelled) {
      m_fes = m_fesNext;
      m_fesKey = m_fesNextKey;
      m_fesErr.clear();
    } else if (!cancelled) {
      m_fesErr = m_fesThreadErr;
    } else {
      m_fesNextKey.clear();  // annulée : relancée avec la nouvelle zone
    }
    m_fesNext.reset();
    UpdateTide(false);
  } else if (m_fesBusy && m_dlg && m_dlg->IsShown() && !m_fes) {
    m_dlg->SetTideInfo(wxString::Format(
        U8("Préparation de la marée FES2022 (niveau de référence)… %d %%"), (int)(100 * m_fesProgress)));
  }
  // marée « maintenant » : mise à jour à chaque nouvelle minute
  if (m_set.tideMode == 1) {
    const long long minute = (long long)std::time(nullptr) / 60;
    if (minute != m_lastTideMinute) { m_lastTideMinute = minute; UpdateTide(false); }
  }
}

// -------------------------------------------------------------- marée
// Dossier des références verticales livrées avec le plugin
wxString PersonalDepth_pi::DefaultReferenceDir() {
  const wxString sep = wxFileName::GetPathSeparator();
  wxString a = GetPluginDataDir("PersonalDepth_pi") + sep + "references";
  if (wxDirExists(a)) return a;
  wxString local;
  if (wxGetEnv("LOCALAPPDATA", &local))
    return local + sep + "opencpn" + sep + "plugins" + sep + "PersonalDepth_pi" + sep + "references";
  return a;
}

static wxString AsciiOnly(const wxString& in) {  // atlas OpenGL : ASCII seulement
  wxString o;
  for (wxUniChar c : in) {
    const int v = (int)c.GetValue();
    if (v >= 32 && v < 127) o += c;
    else if (v == 0xE9 || v == 0xE8 || v == 0xEA || v == 0xEB) o += 'e';
    else if (v == 0xE0 || v == 0xE2) o += 'a';
    else if (v == 0xC9 || v == 0xC8) o += 'E';
    else o += '?';
  }
  return o;
}

void PersonalDepth_pi::TideChanged() {
  UpdateTide(true);
  SaveConfig();
}

// Prépare en tâche de fond la marée maillée de la zone des couches : lecture
// des constantes FES et calcul de Z0 (niveau moyen au-dessus du ZH).
void PersonalDepth_pi::StartFes(const std::string& key, const std::string& atlas,
                                const double bbox[4], const TideZ0Params& z0) {
  if (m_fesThread.joinable()) return;  // une préparation à la fois
  m_fesCancel = false;
  m_fesOk = false;
  m_fesProgress = 0.f;
  m_fesNextKey = key;
  m_fesThreadErr.clear();
  auto next = std::make_shared<FesTide>();
  m_fesNext = next;
  m_fesBusy = true;
  const double b0 = bbox[0], b1 = bbox[1], b2 = bbox[2], b3 = bbox[3];
  m_fesThread = std::thread([this, next, atlas, b0, b1, b2, b3, z0]() {
    TideAtlas at;
    std::string err;
    const bool ok = at.open(atlas, err) &&
                    next->prepare(at, b0, b1, b2, b3, z0, err, &m_fesCancel, &m_fesProgress);
    m_fesThreadErr = ok ? std::string() : err;
    m_fesOk = ok;
    m_fesBusy = false;  // publié après les résultats
  });
}

void PersonalDepth_pi::StopFes() {
  m_fesCancel = true;
  if (m_fesThread.joinable()) m_fesThread.join();
  m_fesNext.reset();
}

// Hauteur d'eau ajoutée à toutes les profondeurs affichées (couleurs, sondes,
// isobathes) ; les données restent au zéro hydrographique.
//  - stations OpenCPN : hauteur uniforme, station la plus proche du centre des couches ;
//  - FES2022 maillé : hauteur en chaque nœud de l'atlas (1/30°) + Z0 local.
void PersonalDepth_pi::UpdateTide(bool findStation) {
  DisplaySettings& s = m_set;
  const float oldH = s.tideH;
  const bool oldOk = s.tideOk;
  const auto oldTide = s.tide;
  s.tideOk = false;
  s.tideH = 0.f;
  s.tideLegend.clear();
  auto setUniform = [&](float h) {
    if (oldTide && !oldTide->gridded() && oldTide->h0 == h) { s.tide = oldTide; return; }
    auto f = std::make_shared<TideField>();
    f->h0 = h;
    f->finish();
    f->version = ++m_tideVersion;
    s.tide = f;
  };
  auto compute = [&]() -> wxString {
    if (s.tideMode == 0) return U8("Profondeurs au zéro hydrographique (sans marée)");
    double la0 = 1e9, la1 = -1e9, lo0 = 1e9, lo1 = -1e9;
    for (auto& l : m_layers)
      if (l->visible && l->state() == BathyLayer::Ready) {
        la0 = std::min(la0, l->cloud.latMin); la1 = std::max(la1, l->cloud.latMax);
        lo0 = std::min(lo0, l->cloud.lonMin); lo1 = std::max(lo1, l->cloud.lonMax);
      }
    for (const auto& d : m_dbSources)  // base : emprise des sources visibles
      if (d.visible) {
        la0 = std::min(la0, d.latMin); la1 = std::max(la1, d.latMax);
        lo0 = std::min(lo0, d.lonMin); lo1 = std::max(lo1, d.lonMax);
      }
    if (la0 > la1) return U8("En attente d'une couche chargée");
    const double clat = 0.5 * (la0 + la1), clon = 0.5 * (lo0 + lo1);
    const time_t t = s.tideMode == 1 ? std::time(nullptr)
                                     : (time_t)(s.tideTime + (long long)s.tideShiftMin * 60);
    const wxString when = wxDateTime((time_t)t).Format("%d/%m %H:%M");

    if (s.tideSource == 1) {  // ------------------------------- FES2022 maillé
      if (s.tideAtlas.empty() || !wxFileExists(U8(s.tideAtlas.c_str())))
        return U8("Choisir le fichier de marée FES2022 (.pdtide)");
      // emprise arrondie au 1/50° : pas de recalcul pour un simple décalage
      const double bb[4] = {std::floor(lo0 * 50) / 50, std::ceil(lo1 * 50) / 50,
                            std::floor(la0 * 50) / 50, std::ceil(la1 * 50) / 50};
      TideZ0Params z0;
      wxStringTokenizer tk(U8(s.tideZ0Files.c_str()), ";");
      while (tk.HasMoreTokens()) {
        const wxString f = tk.GetNextToken().Trim().Trim(false);
        if (!f.empty()) z0.userFiles.push_back(f.ToUTF8().data());
      }
      const wxString dir = U8(s.vrefFranceDir.c_str());
      if (s.tideUseBathyElli && wxDirExists(dir)) {
        wxArrayString nm, zh;
        wxDir::GetAllFiles(dir, &nm, "*NM*.glhi");
        wxDir::GetAllFiles(dir, &zh, "*ZH*.glhi");
        if (!nm.empty() && !zh.empty()) {
          for (auto& f : nm) z0.nmFiles.push_back(f.ToUTF8().data());
          for (auto& f : zh) z0.zhFiles.push_back(f.ToUTF8().data());
        }
      }
      std::string key = s.tideAtlas + "|" + s.tideZ0Files + "|" +
                        std::to_string(z0.nmFiles.size()) + "|" + s.vrefFranceDir;
      char kb[96];
      std::snprintf(kb, sizeof kb, "|%.2f %.2f %.2f %.2f", bb[0], bb[1], bb[2], bb[3]);
      key += kb;
      if (key != m_fesKey) {
        if (m_fesBusy && m_fesNextKey != key) m_fesCancel = true;  // relancée à la fin
        else if (!m_fesThread.joinable() && key != m_fesNextKey) StartFes(key, s.tideAtlas, bb, z0);
        else if (!m_fesThread.joinable() && !m_fesErr.empty() && key == m_fesNextKey)
          return U8("Marée FES2022 : ") + U8(m_fesErr.c_str());
      }
      if (!m_fes) {
        if (m_fesBusy)
          return wxString::Format(U8("Préparation de la marée FES2022 (niveau de référence)… %d %%"),
                                  (int)(100 * m_fesProgress));
        return m_fesErr.empty() ? U8("Marée FES2022 en attente") : U8("Marée FES2022 : ") + U8(m_fesErr.c_str());
      }
      // champ du moment (rapide), recalculé seulement si l'instant ou la zone change
      if (!(m_fieldT == (double)t && m_fieldFes == m_fes.get() && oldTide && oldTide->gridded())) {
        auto f = std::make_shared<TideField>();
        m_fes->field((double)t, *f);
        f->version = ++m_tideVersion;
        s.tide = f;
        m_fieldT = (double)t;
        m_fieldFes = m_fes.get();
      } else {
        s.tide = oldTide;
      }
      const TideField& F = *s.tide;
      const float hc = F.at(clon, clat);
      const double nm = m_fes->heightNM(clon, clat, (double)t), z = m_fes->z0At(clon, clat);
      s.tideH = hc;
      s.tideOk = true;
      s.tideLegend = AsciiOnly(wxString::Format("Maree FES %+.2f m (%+.2f a %+.2f) %s", hc,
                                                F.hMin, F.hMax, when));
      wxString info = wxString::Format(
          U8("FES2022 le %s : hauteur d'eau %+.2f m au centre (FES %+.2f m sur le NM, "
             "Z0 %.2f m), de %+.2f à %+.2f m sur la zone\n%s"),
          when, hc, nm, z, F.hMin, F.hMax, U8(m_fes->info().c_str()));
      if (m_fesBusy) info += U8(" — nouvelle zone en préparation");
      return info;
    }

    // ------------------------------------------------------- station OpenCPN
    std::unique_ptr<HostApi> api;
    try { api = GetHostApi(); } catch (...) {}
    HostApi121* a = dynamic_cast<HostApi121*>(api.get());
    if (!a) return U8("Marée indisponible : OpenCPN 5.14 ou plus récent requis");
    if (findStation || m_tideStation < 0 ||
        std::fabs(clat - m_tideRefLat) > 0.01 || std::fabs(clon - m_tideRefLon) > 0.01) {
      PlugIn_TideStation st;
      if (!a->GetNearestTideStation(clat, clon, &st)) {
        m_tideStation = -1;
        return U8("Aucune station de marée : activer les données de marée d'OpenCPN "
                  "(Options › Cartes › Marées et courants)");
      }
      m_tideStation = st.index;
      m_tideStationName = wxString(st.name, wxConvUTF8);
      if (m_tideStationName.empty()) m_tideStationName = wxString(st.name, wxConvISO8859_1);
      m_tideStationName.Trim().Trim(false);
      const double dy = (st.lat - clat) * 111.32, dx = (st.lon - clon) * 111.32 * std::cos(clat * M_PI / 180);
      m_tideStationDistKm = std::hypot(dx, dy);
      m_tideRefLat = clat; m_tideRefLon = clon;
    }
    float h = 0;
    if (!a->GetTideHeight(m_tideStation, t, &h) || !std::isfinite(h))
      return U8("Hauteur de marée indisponible pour ") + m_tideStationName;
    s.tideH = h;
    s.tideOk = true;
    setUniform(h);
    s.tideLegend = AsciiOnly(wxString::Format("Maree %+.2f m %s %s", h, when, m_tideStationName));
    wxString info = wxString::Format(U8("Station %s (%.1f km) : hauteur d'eau %+.2f m le %s"),
                                     m_tideStationName, m_tideStationDistKm, h, when);
    if (m_tideStationDistKm > 30) info += U8(" — station éloignée, à vérifier");
    return info;
  };
  s.tideInfo = compute();
  if (!s.tideOk) s.tide.reset();
  if (m_dlg) m_dlg->SetTideInfo(s.tideInfo);
  if (s.tideH != oldH || s.tideOk != oldOk || s.tide != oldTide)
    RequestRefresh(GetOCPNCanvasWindow());
}

// ------------------------------------------------------------------ rendu
bool PersonalDepth_pi::ShouldDraw(int priority) {
  if (priority == OVERLAY_CHARTS) {  // sous les navires, routes et AIS
    m_hostHasChartsPriority = true;
    return true;
  }
  // Hôtes plus anciens : pas de passe OVERLAY_CHARTS, on dessine en LEGACY
  return (priority == OVERLAY_LEGACY || priority == -1) && !m_hostHasChartsPriority;
}

// Passe d'interface : l'étiquette du curseur, redessinée à chaque image.
// La passe OVERLAY_CHARTS est mise en cache par OpenCPN avec les cartes.
wxString PersonalDepth_pi::CursorTextFor(int priority) const {
  if (!m_set.legend) return wxString();
  if (m_hostHasChartsPriority) return priority == OVERLAY_OVER_UI ? m_cursorText : wxString();
  return m_cursorText;  // hôte ancien : tout est dessiné dans la passe LEGACY
}

bool PersonalDepth_pi::RenderOverlayMultiCanvas(wxDC& dc, PlugIn_ViewPort* vp, int,
                                                int priority) {
  if (!vp) return false;
  m_vpBox[0] = vp->lon_min; m_vpBox[1] = vp->lon_max; m_vpBox[2] = vp->lat_min; m_vpBox[3] = vp->lat_max;
  m_vpValid = true;
  const auto layers = DrawLayers();
  if (layers.empty()) return false;
  if (priority == OVERLAY_OVER_UI && m_hostHasChartsPriority) {
    m_renderer->RenderCursorDC(dc, vp, CursorTextFor(priority), m_set.fontSize);
    return true;
  }
  if (!ShouldDraw(priority)) return false;
  m_renderer->RenderDC(dc, vp, layers, m_set, CursorTextFor(priority));
  m_lastPpd = m_renderer->LastPixelsPerDegLon();
  return true;
}

bool PersonalDepth_pi::RenderGLOverlayMultiCanvas(wxGLContext*, PlugIn_ViewPort* vp, int,
                                                  int priority) {
  if (!vp) return false;
  m_vpBox[0] = vp->lon_min; m_vpBox[1] = vp->lon_max; m_vpBox[2] = vp->lat_min; m_vpBox[3] = vp->lat_max;
  m_vpValid = true;
  const auto layers = DrawLayers();
  if (layers.empty()) return false;
  if (priority == OVERLAY_OVER_UI && m_hostHasChartsPriority) {
    m_renderer->RenderCursorGL(vp, CursorTextFor(priority), m_set.fontSize);
    return true;
  }
  if (!ShouldDraw(priority)) return false;
  m_renderer->RenderGL(vp, layers, m_set, CursorTextFor(priority));
  m_lastPpd = m_renderer->LastPixelsPerDegLon();
  return true;
}

void PersonalDepth_pi::SetCursorLatLon(double lat, double lon) {
  m_curLat = lat; m_curLon = lon;
  if (!m_set.legend || DrawLayers().empty()) {
    if (!m_cursorText.empty()) {
      m_cursorText.clear();
      m_renderer->SetCursor(false, lat, lon);
      if (m_dlg) m_dlg->SetCursorText(wxString());
      RequestRefresh(GetOCPNCanvasWindow());
    }
    return;
  }
  const bool wasShown = !m_cursorText.empty();
  wxString before = m_cursorText;
  // Glisser de la carte : OpenCPN redessine déjà, l'étiquette est masquée
  if (wxGetMouseState().LeftIsDown()) {
    m_cursorText.clear();
    m_renderer->SetCursor(false, lat, lon);
    return;
  }
  UpdateCursorText();
  const bool shown = !m_cursorText.empty();
  m_renderer->SetCursor(shown, lat, lon);
  if (m_dlg && m_cursorText != before) m_dlg->SetCursorText(m_cursorText);
  // L'étiquette suit la souris : redessin immédiat, au plus toutes les 40 ms ;
  // un redessin différé rattrape la dernière position.
  if (!(shown || wasShown)) return;
  const wxLongLong now = wxGetLocalTimeMillis();
  if (now - m_lastCursorRefresh >= 40) {
    m_lastCursorRefresh = now;
    RequestRefresh(GetOCPNCanvasWindow());
  } else if (m_cursorRefresh && !m_cursorRefresh->IsRunning()) {
    m_cursorRefresh->StartOnce(40);
  }
}

void PersonalDepth_pi::UpdateCursorText() {
  m_cursorText.clear();
  const double radiusDeg = 12.0 / std::max(m_lastPpd, 1e-3);  // ~12 px autour du curseur
  const auto layers = DrawLayers();
  for (auto it = layers.rbegin(); it != layers.rend(); ++it) {
    if (!(*it)->visible) continue;
    float gz, sz; double dist;
    int64_t sid = 0;
    if (!(*it)->probe(m_curLon, m_curLat, radiusDeg, gz, sz, dist, &sid)) continue;
    // Texte ASCII (atlas de glyphes OpenGL)
    const float v = !std::isnan(sz) ? sz : gz;
    wxString base = !std::isnan(sz) ? wxString::Format("Sonde %.2f m", sz)
                                    : wxString::Format("Grille %.2f m", gz);
    if (m_set.tideOk && m_set.tide) {  // profondeur au ZH puis profondeur du moment
      const float th = m_set.tide->at(m_curLon, m_curLat);
      base = (!std::isnan(sz) ? "Sonde " : "Grille ") +
             wxString::Format("%.2f m ZH -> %.2f m (maree %+.2f)", v, v + th, th);
    }
    if (!std::isnan(sz)) base += wxString::Format("  (a %.1f m)", dist);
    if (sid)  // vue de la base : source retenue pour ce pixel
      for (const auto& d : m_dbSources)
        if (d.id == sid) {
          wxString n = AsciiOnly(wxString::FromUTF8(d.name.c_str()));
          if (n.size() > 28) n = n.Left(26) + "..";
          base += "  [" + n + " " + wxString(d.date.substr(0, 4)) + "]";
        }
    m_cursorText = base;
    return;
  }
}

// ---------------------------------------------------------- configuration
void PersonalDepth_pi::SaveConfig() {
  wxFileConfig* c = GetOCPNConfigObject();
  if (!c) return;
  c->DeleteGroup("/PlugIns/PersonalDepth");
  c->SetPath("/PlugIns/PersonalDepth");
  c->Write("Ramp", U8(m_set.ramp.serialize().c_str()));
  c->Write("Opacity", m_set.opacity);
  c->Write("Hillshade", m_set.hillshade);
  c->Write("ShadeStrength", (double)m_set.shadeStrength);
  c->Write("Exaggeration", (double)m_set.exaggeration);
  c->Write("ShadeSmooth", m_set.shadeSmooth);
  c->Write("Soundings", m_set.soundings);
  c->Write("Decimals", m_set.decimals);
  c->Write("Spacing", m_set.spacingPx);
  c->Write("FontSize", m_set.fontSize);
  c->Write("Smooth", m_set.smooth);
  c->Write("Legend", m_set.legend);
  c->Write("Contours", m_set.contours);
  c->Write("ContourMode", m_set.contourMode);
  c->Write("ContourStep", (double)m_set.contourStep);
  c->Write("ContourMajor", (double)m_set.contourMajor);
  c->Write("ContourLabels", m_set.contourLabels);
  c->Write("ContourSmooth", m_set.contourSmooth);
  c->Write("ContourColor", (long)m_set.contourColor);
  c->Write("CrsCode", U8(m_set.crsCode.c_str()));
  c->Write("CrsZone", m_set.crsZone);
  c->Write("CrsSouth", m_set.crsSouth);
  c->Write("CrsCustom", U8(m_set.crsCustom.c_str()));
  c->Write("ReadPrj", m_set.readPrj);
  c->Write("VrefChoice", m_set.vrefMode);
  c->Write("ImportOrder", m_set.importOrder);
  c->Write("ImportSign", m_set.importSign);
  c->Write("ImportOffset", (double)m_set.importOffset);
  c->Write("ImportCache", m_set.importCache);
  c->Write("ImportNodataOn", m_set.importNodataOn);
  c->Write("ImportNodata", U8(m_set.importNodata.c_str()));
  c->Write("ImportBoundsOn", m_set.importBoundsOn);
  c->Write("BoundMin", (double)m_set.boundMin);
  c->Write("BoundMax", (double)m_set.boundMax);
  c->Write("VrefFranceDir", U8(m_set.vrefFranceDir.c_str()));
  c->Write("VrefZh", U8(m_set.vrefZh.c_str()));
  c->Write("VrefGeoid", U8(m_set.vrefGeoid.c_str()));
  c->Write("VrefDirect", U8(m_set.vrefDirect.c_str()));
  c->Write("VrefInvert", m_set.vrefInvert);
  c->Write("TideMode", m_set.tideMode);
  c->Write("TideTime", wxString::Format("%lld", m_set.tideTime));
  c->Write("TideShift", m_set.tideShiftMin);
  c->Write("TideSource", m_set.tideSource);
  c->Write("TideAtlas", U8(m_set.tideAtlas.c_str()));
  c->Write("TideZ0", U8(m_set.tideZ0Files.c_str()));
  c->Write("TideBathyElli", m_set.tideUseBathyElli);
  c->Write("DbFolder", U8(m_set.dbFolder.c_str()));
  c->Write("AutoPalette", m_set.autoPalette);
  c->Write("AutoSource", m_set.autoSource);
  c->Write("AutoCount", m_set.autoCount);
  c->Write("AutoNice", m_set.autoNice);
  c->Write("AutoBase", U8(m_set.autoBase.c_str()));
  c->Write("AutoMinOn", m_set.autoMinOn);
  c->Write("AutoMaxOn", m_set.autoMaxOn);
  c->Write("AutoMin", (double)m_set.autoMin);
  c->Write("AutoMax", (double)m_set.autoMax);
  c->Write("DbPriority", m_set.dbPriority);
  {
    std::string h;
    for (const auto& n : m_set.dbHidden) h += (h.empty() ? "" : ";") + n;
    c->Write("DbHidden", U8(h.c_str()));
  }
  c->Write("LayerCount", 0);  // couches « fichiers » supprimées : tout passe par la base
  c->SetPath("/");
}

void PersonalDepth_pi::LoadConfig() {
  wxFileConfig* c = GetOCPNConfigObject();
  if (!c) return;
  c->SetPath("/PlugIns/PersonalDepth");
  wxString ramp;
  if (c->Read("Ramp", &ramp)) m_set.ramp.deserialize(ramp.ToUTF8().data());
  double d;
  c->Read("Opacity", &m_set.opacity, m_set.opacity);
  c->Read("Hillshade", &m_set.hillshade, m_set.hillshade);
  if (c->Read("ShadeStrength", &d)) m_set.shadeStrength = (float)d;
  if (c->Read("Exaggeration", &d)) m_set.exaggeration = (float)d;
  c->Read("ShadeSmooth", &m_set.shadeSmooth, m_set.shadeSmooth);
  c->Read("Soundings", &m_set.soundings, m_set.soundings);
  c->Read("Decimals", &m_set.decimals, m_set.decimals);
  c->Read("Spacing", &m_set.spacingPx, m_set.spacingPx);
  c->Read("FontSize", &m_set.fontSize, m_set.fontSize);
  c->Read("Smooth", &m_set.smooth, m_set.smooth);
  c->Read("Legend", &m_set.legend, m_set.legend);
  c->Read("Contours", &m_set.contours, m_set.contours);
  c->Read("ContourMode", &m_set.contourMode, m_set.contourMode);
  if (c->Read("ContourStep", &d) && d > 0) m_set.contourStep = (float)d;
  if (c->Read("ContourMajor", &d) && d >= 0) m_set.contourMajor = (float)d;
  c->Read("ContourLabels", &m_set.contourLabels, m_set.contourLabels);
  c->Read("ContourSmooth", &m_set.contourSmooth, m_set.contourSmooth);
  long col;
  if (c->Read("ContourColor", &col)) m_set.contourColor = (uint32_t)col;
  wxString ws;
  if (c->Read("CrsCode", &ws)) m_set.crsCode = ws.ToUTF8().data();
  c->Read("CrsZone", &m_set.crsZone, m_set.crsZone);
  c->Read("CrsSouth", &m_set.crsSouth, m_set.crsSouth);
  if (c->Read("CrsCustom", &ws)) m_set.crsCustom = ws.ToUTF8().data();
  c->Read("ReadPrj", &m_set.readPrj, m_set.readPrj);
  c->Read("VrefChoice", &m_set.vrefMode, m_set.vrefMode);
  c->Read("ImportOrder", &m_set.importOrder, m_set.importOrder);
  c->Read("ImportSign", &m_set.importSign, m_set.importSign);
  {
    double v;
    if (c->Read("ImportOffset", &v)) m_set.importOffset = (float)v;
  }
  c->Read("ImportCache", &m_set.importCache, m_set.importCache);
  c->Read("ImportNodataOn", &m_set.importNodataOn, m_set.importNodataOn);
  if (c->Read("ImportNodata", &ws)) m_set.importNodata = ws.ToUTF8().data();
  c->Read("ImportBoundsOn", &m_set.importBoundsOn, m_set.importBoundsOn);
  {
    double v;
    if (c->Read("BoundMin", &v)) m_set.boundMin = (float)v;
    if (c->Read("BoundMax", &v)) m_set.boundMax = (float)v;
  }
  if (c->Read("VrefFranceDir", &ws)) m_set.vrefFranceDir = ws.ToUTF8().data();
  if (c->Read("VrefZh", &ws)) m_set.vrefZh = ws.ToUTF8().data();
  if (c->Read("VrefGeoid", &ws)) m_set.vrefGeoid = ws.ToUTF8().data();
  if (c->Read("VrefDirect", &ws)) m_set.vrefDirect = ws.ToUTF8().data();
  c->Read("VrefInvert", &m_set.vrefInvert, m_set.vrefInvert);
  c->Read("TideMode", &m_set.tideMode, m_set.tideMode);
  if (c->Read("TideTime", &ws)) ws.ToLongLong(&m_set.tideTime);
  c->Read("TideShift", &m_set.tideShiftMin, m_set.tideShiftMin);
  c->Read("TideSource", &m_set.tideSource, m_set.tideSource);
  if (c->Read("TideAtlas", &ws)) m_set.tideAtlas = ws.ToUTF8().data();
  if (c->Read("TideZ0", &ws)) m_set.tideZ0Files = ws.ToUTF8().data();
  c->Read("TideBathyElli", &m_set.tideUseBathyElli, m_set.tideUseBathyElli);
  if (c->Read("DbFolder", &ws)) m_set.dbFolder = ws.ToUTF8().data();
  c->Read("AutoPalette", &m_set.autoPalette, m_set.autoPalette);
  c->Read("AutoSource", &m_set.autoSource, m_set.autoSource);
  c->Read("AutoCount", &m_set.autoCount, m_set.autoCount);
  c->Read("AutoNice", &m_set.autoNice, m_set.autoNice);
  if (c->Read("AutoBase", &ws)) m_set.autoBase = ws.ToUTF8().data();
  c->Read("AutoMinOn", &m_set.autoMinOn, m_set.autoMinOn);
  c->Read("AutoMaxOn", &m_set.autoMaxOn, m_set.autoMaxOn);
  {
    double v;
    if (c->Read("AutoMin", &v)) m_set.autoMin = (float)v;
    if (c->Read("AutoMax", &v)) m_set.autoMax = (float)v;
  }
  c->Read("DbPriority", &m_set.dbPriority, m_set.dbPriority);
  if (c->Read("DbHidden", &ws)) {
    m_set.dbHidden.clear();
    wxStringTokenizer tk(ws, ";");
    while (tk.HasMoreTokens()) {
      const wxString t = tk.GetNextToken().Trim().Trim(false);
      if (!t.empty()) m_set.dbHidden.insert(t.ToUTF8().data());
    }
  }
  c->SetPath("/");
}

// ================================================================ base
std::vector<LayerPtr> PersonalDepth_pi::DrawLayers() const {
  std::vector<LayerPtr> v;
  if (m_dbView && m_dbView->state() == BathyLayer::Ready) v.push_back(m_dbView);  // dessous
  v.insert(v.end(), m_layers.begin(), m_layers.end());
  return v;
}

wxString PersonalDepth_pi::DbStatus() {
  std::lock_guard<std::mutex> lk(m_dbMx);
  if (m_dbBusy)
    return U8((m_dbTitle + " — " + m_dbStep).c_str()) +
           wxString::Format(" %d %%", (int)(100 * m_dbProgress));
  return U8(m_dbLast.c_str());
}

void PersonalDepth_pi::DbOpen() {
  m_dbSources.clear();
  m_dbCells.clear();
  if (m_set.dbFolder.empty() || m_dbBusy) return;
  BathyDb db;
  std::string err;
  const bool opened = db.open(m_set.dbFolder, err);
  m_dbCells.clear();
  if (opened) {
    m_dbSources = db.sources();
    m_dbCells = db.cellInfo();
    m_dbCellDeg = db.cellDeg();
  }
  {
    std::lock_guard<std::mutex> lk(m_dbMx);
    m_dbLast = err.empty() ? std::string() : err;
  }
  ++m_dbEpoch;
  // base antérieure au filtre des valeurs de remplissage : profondeurs
  // min/max des sources recalculées en tâche de fond (une seule fois)
  if (opened && db.needsRepair())
    StartDbJob(U8("Mise à jour des statistiques des sources").ToStdString(wxConvUTF8),
               [](BathyDb& b, std::string& e, const DbProgress&) { return b.repairStats(e); });
}

void PersonalDepth_pi::StartDbJob(
    const std::string& title,
    std::function<bool(BathyDb&, std::string&, const DbProgress&)> job) {
  if (m_dbBusy || m_dbThread.joinable() || m_set.dbFolder.empty()) return;
  {
    std::lock_guard<std::mutex> lk(m_dbMx);
    m_dbTitle = title;
    m_dbStep.clear();
    m_dbErr.clear();
  }
  m_dbProgress = 0.f;
  m_dbCancel = false;
  m_dbBusy = true;
  const std::string folder = m_set.dbFolder;
  m_dbThread = std::thread([this, folder, job]() {
    BathyDb db;
    std::string err;
    DbProgress prog = [this](float f, const std::string& step) {
      m_dbProgress = f;
      std::lock_guard<std::mutex> lk(m_dbMx);
      m_dbStep = step;
    };
    bool ok = db.open(folder, err) && job(db, err, prog);
    if (!ok && err.empty()) err = "échec";
    std::string e2;
    db.reloadSources(e2);
    {
      std::lock_guard<std::mutex> lk(m_dbMx);
      m_dbErr = ok ? std::string() : (err == "Annulé" ? std::string("arrêté") : err);
      m_dbSourcesNew = db.sources();
      m_dbCellsNew = db.cellInfo();
      m_dbCellDegNew = db.cellDeg();
    }
    m_dbBusy = false;
  });
  if (m_dlg) m_dlg->SetDbStatus(DbStatus());
}

void PersonalDepth_pi::DbImport(const std::vector<LayerParams>& files, const std::string& group,
                                const std::vector<std::vector<int64_t>>& replace) {
  const DbPriority prio = (DbPriority)m_set.dbPriority;
  const std::atomic<bool>* cancel = &m_dbCancel;
  StartDbJob(U8("Import").ToStdString(wxConvUTF8) + " (" + std::to_string(files.size()) + " fichier" +
                 (files.size() > 1 ? "s)" : ")"),
             [files, prio, cancel, group, replace](BathyDb& db, std::string& err, const DbProgress& prog) {
               std::string errors;
               size_t done = 0;
               auto stopped = [&]() {
                 err = U8("arrêté — ").ToStdString(wxConvUTF8) + std::to_string(done) + "/" +
                       std::to_string(files.size()) + U8(" fichier(s) importé(s) ; le fichier en cours "
                                                         "est annulé").ToStdString(wxConvUTF8);
                 return false;
               };
               for (size_t i = 0; i < files.size(); ++i) {
                 if (*cancel) return stopped();
                 LayerParams p = files[i];
                 p.cloudOnly = true;
                 p.stop = cancel;
                 const std::string name = std::filesystem::u8path(p.path).filename().u8string();
                 const float f0 = (float)i / files.size(), f1 = (float)(i + 1) / files.size();
                 const std::string tag = "[" + std::to_string(i + 1) + "/" + std::to_string(files.size()) + "] ";
                 prog(f0, tag + name + " : lecture");
                 BathyLayer L(p);
                 L.loadSync();
                 if (*cancel) return stopped();
                 if (L.state() != BathyLayer::Ready || L.cloud.empty()) {
                   errors += name + " : " + (L.error().empty() ? "lecture impossible" : L.error()) + "\n";
                   continue;
                 }
                 bool gridded = false;
                 DbSource m;
                 m.name = name;
                 m.path = p.path;
                 m.res = EstimateResolution(L.cloud, gridded);
                 m.date = GuessSurveyDate(p.path);
                 m.crs = L.crsUsed().empty() ? "WGS84" : L.crsUsed();
                 m.vref = p.vref.active() ? (p.vref.label.empty() ? "converti" : p.vref.label) : "ZH";
                 m.group = group.empty() ? GuessSourceGroup(p.path) : group;
                 std::string e;
                 const bool rep = i < replace.size() && !replace[i].empty();
                 auto sub = [&](float f, const std::string& step) {
                   prog(f0 + (f1 - f0) * (rep ? 0.7f * f : f), tag + name + " : " + step);
                 };
                 if (!db.importCloud(L.cloud, m, gridded, prio, e, sub, cancel)) {
                   if (*cancel || e == "Annulé") return stopped();
                   errors += name + " : " + e + "\n";
                   continue;
                 }
                 // réimport : l'ancienne version de ce fichier est retirée de la base
                 if (rep) {
                   prog(f0 + (f1 - f0) * 0.7f, tag + name + U8(" : retrait de l'ancienne version").ToStdString(wxConvUTF8));
                   if (!db.removeSources(replace[i], prio, e)) errors += name + " (ancienne version) : " + e + "\n";
                 }
                 ++done;
               }
               if (!errors.empty()) {
                 err = std::to_string(done) + "/" + std::to_string(files.size()) + U8(" importé(s)\n").ToStdString(wxConvUTF8) + errors;
                 return false;
               }
               return true;
             });
}

void PersonalDepth_pi::DbSetCellsVisible(const std::vector<std::string>& names, bool visible) {
  bool changed = false;
  for (const auto& n : names)
    changed |= visible ? m_set.dbHidden.erase(n) > 0 : m_set.dbHidden.insert(n).second;
  if (!changed) return;
  SaveConfig();
  ++m_dbEpoch;  // vue reconstruite sans (ou avec) ces fichiers
  UpdateDbView();
}

void PersonalDepth_pi::DbZoomToCells(const std::vector<std::string>& names) {
  double lo0 = 1e9, lo1 = -1e9, la0 = 1e9, la1 = -1e9;
  for (const auto& c : m_dbCells)
    if (std::find(names.begin(), names.end(), c.name) != names.end()) {
      lo0 = std::min(lo0, c.lonW); lo1 = std::max(lo1, c.lonW + c.deg);
      la0 = std::min(la0, c.latS); la1 = std::max(la1, c.latS + c.deg);
    }
  if (lo0 > lo1) return;
  const double lat = 0.5 * (la0 + la1), lon = 0.5 * (lo0 + lo1);
  const double wm = (lo1 - lo0) * 111320.0 * std::cos(lat * M_PI / 180.0), hm = (la1 - la0) * 110540.0;
  wxWindow* cv = GetOCPNCanvasWindow();
  const wxSize sz = cv ? cv->GetClientSize() : wxSize(1000, 800);
  JumpToPosition(lat, lon, 0.9 * std::min(sz.x / std::max(wm, 1.0), sz.y / std::max(hm, 1.0)));
}

void PersonalDepth_pi::DbResplit(double deg) {
  const std::atomic<bool>* cancel = &m_dbCancel;
  char t[96];
  std::snprintf(t, sizeof t, "%s %g°", U8("Redécoupage des fichiers en cellules de").ToStdString(wxConvUTF8).c_str(), deg);
  m_set.dbHidden.clear();  // les noms de fichiers changent
  SaveConfig();
  StartDbJob(t, [deg, cancel](BathyDb& db, std::string& err, const DbProgress& prog) {
    return db.resplit(deg, err, prog, cancel);
  });
}

void PersonalDepth_pi::DbRemove(const std::vector<int64_t>& ids) {
  if (ids.empty()) return;
  const DbPriority prio = (DbPriority)m_set.dbPriority;
  StartDbJob("Retrait de " + std::to_string(ids.size()) + " source" + (ids.size() > 1 ? "s" : ""),
             [ids, prio](BathyDb& db, std::string& err, const DbProgress& prog) {
               return db.removeSources(ids, prio, err, prog);
             });
}

void PersonalDepth_pi::DbClean(const std::vector<int64_t>& ids, float zMin, float zMax) {
  if (ids.empty() || !(zMax > zMin)) return;
  m_set.boundMin = zMin;  // mêmes bornes que l'import
  m_set.boundMax = zMax;
  SaveConfig();
  const DbPriority prio = (DbPriority)m_set.dbPriority;
  const std::atomic<bool>* cancel = &m_dbCancel;
  StartDbJob("Nettoyage de " + std::to_string(ids.size()) + " source" + (ids.size() > 1 ? "s" : ""),
             [ids, zMin, zMax, prio, cancel](BathyDb& db, std::string& err, const DbProgress& prog) {
               return db.cleanSources(ids, zMin, zMax, prio, err, prog, cancel);
             });
}

void PersonalDepth_pi::DbSetVisible(const std::vector<int64_t>& ids, bool visible) {
  if (ids.empty()) return;
  const DbPriority prio = (DbPriority)m_set.dbPriority;
  StartDbJob(std::string(visible ? "Affichage" : "Masquage") + " de " + std::to_string(ids.size()) +
                 " source" + (ids.size() > 1 ? "s" : ""),
             [ids, visible, prio](BathyDb& db, std::string& err, const DbProgress& prog) {
               return db.setVisible(ids, visible, prio, err, prog);
             });
}

// Déplace le bloc sélectionné d'un cran dans l'ordre des rangs
void PersonalDepth_pi::DbMove(const std::vector<int64_t>& ids, int delta) {
  std::vector<int64_t> order;
  for (const auto& d : m_dbSources) order.push_back(d.id);  // déjà triées par rang
  if (order.size() < 2) return;
  std::set<int64_t> sel(ids.begin(), ids.end());
  bool moved = false;
  if (delta < 0) {
    for (size_t k = 1; k < order.size(); ++k)
      if (sel.count(order[k]) && !sel.count(order[k - 1])) { std::swap(order[k], order[k - 1]); moved = true; }
  } else {
    for (size_t k = order.size() - 1; k-- > 0;)
      if (sel.count(order[k]) && !sel.count(order[k + 1])) { std::swap(order[k], order[k + 1]); moved = true; }
  }
  if (!moved) return;
  const DbPriority prio = (DbPriority)m_set.dbPriority;
  StartDbJob(U8("Classement des sources").ToStdString(wxConvUTF8),
             [order, prio](BathyDb& db, std::string& err, const DbProgress& prog) {
               return db.setRanks(order, prio, err, prog);
             });
}

void PersonalDepth_pi::DbSetGroup(const std::vector<int64_t>& ids, const std::string& group) {
  if (ids.empty() || group.empty()) return;
  StartDbJob("Classe " + group,
             [ids, group](BathyDb& db, std::string& err, const DbProgress&) {
               return db.setGroup(ids, group, err);
             });
}

void PersonalDepth_pi::DbSetPriority(int prio) {
  if (prio == m_set.dbPriority) return;
  m_set.dbPriority = prio;
  SaveConfig();
  StartDbJob(U8("Nouvelle règle de priorité : fusion").ToStdString(wxConvUTF8),
             [prio](BathyDb& db, std::string& err, const DbProgress& prog) {
               return db.remergeAll((DbPriority)prio, err, prog);
             });
}

// Vue de la base : reconstruite en tâche de fond quand la carte sort de la
// zone déjà préparée (marge de 50 %) ou quand le niveau de détail change
void PersonalDepth_pi::UpdateDbView() {
  if (m_set.dbFolder.empty() || m_dbSources.empty()) {
    if (m_dbView || m_dbViewNext) {
      if (m_dbViewNext) m_dbViewNext->cancel();
      m_dbViewNext.reset();
      m_dbView.reset();
      m_renderer->Invalidate();
      RequestRefresh(GetOCPNCanvasWindow());
    }
    return;
  }
  if (m_dbViewNext) {
    const auto st = m_dbViewNext->state();
    if (st == BathyLayer::Loading) return;
    const bool first = !m_dbView;
    m_dbView = st == BathyLayer::Ready ? m_dbViewNext : nullptr;
    m_dbViewNext.reset();
    m_renderer->Invalidate();
    ++m_set.version;
    if (first && m_dbView && m_set.tideMode && !m_set.tideOk) UpdateTide(true);
    if (m_dlg && m_dlg->IsShown()) m_dlg->RefreshLayers();
    RequestRefresh(GetOCPNCanvasWindow());
  }
  if (!m_vpValid) return;
  // niveau : un pixel de la base ≈ un pixel d'écran
  const double ppd = std::max(1e-6, m_lastPpd);
  const int target = std::max(kDbMinLevel, std::min(kDbMaxLevel,
                              (int)std::lround(std::log2(ppd * 360.0 / kTileSize))));
  const double* v = m_vpBox;
  const bool inside = v[0] >= m_dbViewBox[0] && v[1] <= m_dbViewBox[1] && v[2] >= m_dbViewBox[2] &&
                      v[3] <= m_dbViewBox[3];
  if (inside && target == m_dbViewTarget && m_dbViewEpoch == m_dbEpoch) return;
  if (m_dbBusy) return;  // la base est en cours de modification
  const double w = v[1] - v[0], h = v[3] - v[2];
  const double box[4] = {v[0] - 0.5 * w, v[1] + 0.5 * w, v[2] - 0.5 * h, v[3] + 0.5 * h};
  LayerParams p;
  p.label = U8("Base de bathymétrie").ToStdString(wxConvUTF8);
  p.useCache = false;
  p.exaggeration = m_set.exaggeration;
  p.shadeSmooth = m_set.shadeSmooth;
  const std::string folder = m_set.dbFolder;
  const std::set<std::string> hidden = m_set.dbHidden;
  const double b0 = box[0], b1 = box[1], b2 = box[2], b3 = box[3];
  p.custom = [folder, hidden, b0, b1, b2, b3, target](PointCloud& pc, double& dl, double& dt,
                                                     std::vector<int64_t>& src, std::string& err) {
    BathyDb db;
    int used = 0;
    if (!db.open(folder, err)) return false;
    db.setHiddenCells(hidden);
    return db.buildView(b0, b1, b2, b3, target, 4000000, pc, dl, dt, used, err, nullptr, &src);
  };
  for (int k = 0; k < 4; ++k) m_dbViewBox[k] = box[k];
  m_dbViewTarget = target;
  m_dbViewEpoch = m_dbEpoch;
  m_dbViewNext = std::make_shared<BathyLayer>(p);
  m_dbViewNext->startLoad();
}

// ======================================================= palette automatique
void PersonalDepth_pi::AutoPaletteChanged() {
  SaveConfig();
  UpdateAutoPalette(true);
}

// Seuils recalculés d'après les profondeurs du moment (sonde ZH + marée) :
// toutes les données (base + couches) ou la seule zone affichée
void PersonalDepth_pi::UpdateAutoPalette(bool force) {
  DisplaySettings& s = m_set;
  if (!s.autoPalette) { m_autoInfo.clear(); m_autoKey.clear(); return; }
  float lo = 1e30f, hi = -1e30f;
  const auto layers = DrawLayers();
  if (s.autoSource == 1) {
    if (!m_vpValid) return;
    const double* v = m_vpBox;
    for (const auto& l : layers) {
      if (!l->visible || l->state() != BathyLayer::Ready) continue;
      // niveau grossier : ~4 px d'écran par cellule suffisent pour min / max
      const GridLevel& g = l->grid.levels[l->grid.levelFor(m_lastPpd, 4.0)];
      const int i0 = std::max(0, (int)std::floor((v[0] - g.lonW) / g.dlon));
      const int i1 = std::min(g.nx - 1, (int)std::floor((v[1] - g.lonW) / g.dlon));
      const int j0 = std::max(0, (int)std::floor((g.latN - v[3]) / g.dlat));
      const int j1 = std::min(g.ny - 1, (int)std::floor((g.latN - v[2]) / g.dlat));
      for (int j = j0; j <= j1; ++j)
        for (int i = i0; i <= i1; ++i) {
          const float z = g.at(i, j);
          if (std::isnan(z)) continue;
          lo = std::min(lo, z);
          hi = std::max(hi, z);
        }
    }
  } else {
    for (const auto& l : m_layers)
      if (l->visible && l->state() == BathyLayer::Ready) {
        lo = std::min(lo, l->grid.zMin);
        hi = std::max(hi, l->grid.zMax);
      }
    for (const auto& d : m_dbSources)
      if (d.visible) { lo = std::min(lo, d.zmin); hi = std::max(hi, d.zmax); }
  }
  if (lo > hi) return;  // rien à afficher pour l'instant
  if (s.tideOk && s.tide) {  // profondeurs du moment
    lo += s.tide->gridded() ? s.tide->hMin : s.tide->h0;
    hi += s.tide->gridded() ? s.tide->hMax : s.tide->h0;
  }
  // arrondi au décimètre : pas de recalcul pour un simple déplacement
  lo = std::floor(lo * 10.f) / 10.f;
  hi = std::ceil(hi * 10.f) / 10.f;
  const float dataLo = lo;
  // bornes imposées : la palette ne couvre que [min, max] ; au-delà, couleur
  // du premier ou du dernier seuil (ex. min = 0 : palette sur la seule bathymétrie)
  if (s.autoMinOn) lo = s.autoMin;
  if (s.autoMaxOn) hi = s.autoMax;
  if (!(hi > lo)) hi = lo + 1.f;
  char key[256];
  std::snprintf(key, sizeof key, "%.1f %.1f %.1f %d %d %d %zu", lo, hi, dataLo, s.autoCount,
                s.autoNice ? 1 : 0, s.autoSource, std::hash<std::string>()(s.autoBase));
  if (!force && key == m_autoKey) return;
  m_autoKey = key;
  ColorRamp base;
  if (s.autoBase.empty() || !base.deserialize(s.autoBase)) base = ColorRamp::Preset("TimeZero PBG (bleus)");
  ColorRamp r = AutoRamp(base, s.ramp, lo, hi, s.autoCount, s.autoNice);
  // estran sous la borne minimale : couleur « terre » de la palette de référence
  // (celle la plus proche de 0), si elle en a une
  const ColorStop* land = nullptr;
  for (const auto& c : base.stops)
    if (c.depth < 0 && (!land || c.depth > land->depth)) land = &c;
  if (land && !r.stops.empty() && r.stops.front().depth >= 0 && r.stops.front().depth <= 0.5f &&
      dataLo < r.stops.front().depth) {
    r.stops.insert(r.stops.begin(), ColorStop{r.stops.front().depth - 0.01f, land->rgba});
  }
  m_autoInfo = wxString::Format(U8("Automatique : %d seuils de %.1f à %.1f m (%s%s)"),
                                (int)r.stops.size(), lo, hi,
                                s.autoSource == 1 ? U8("zone affichée") : U8("toutes les données"),
                                s.autoMinOn || s.autoMaxOn ? U8(", bornes imposées") : wxString());
  if (r.stops.size() >= 2 && !(r == s.ramp)) {
    s.ramp = r;
    ++s.version;
    RequestRefresh(GetOCPNCanvasWindow());
  }
  if (m_dlg) m_dlg->RefreshPalette();
}

// Centre la carte sur l'emprise des sources choisies
void PersonalDepth_pi::DbZoomTo(const std::vector<int64_t>& ids) {
  double lo0 = 1e9, lo1 = -1e9, la0 = 1e9, la1 = -1e9;
  for (const auto& d : m_dbSources)
    if (std::find(ids.begin(), ids.end(), d.id) != ids.end()) {
      lo0 = std::min(lo0, d.lonMin); lo1 = std::max(lo1, d.lonMax);
      la0 = std::min(la0, d.latMin); la1 = std::max(la1, d.latMax);
    }
  if (lo0 > lo1) return;
  const double lat = 0.5 * (la0 + la1), lon = 0.5 * (lo0 + lo1);
  const double wm = (lo1 - lo0) * 111320.0 * std::cos(lat * M_PI / 180.0), hm = (la1 - la0) * 110540.0;
  wxWindow* cv = GetOCPNCanvasWindow();
  const wxSize sz = cv ? cv->GetClientSize() : wxSize(1000, 800);
  JumpToPosition(lat, lon, 0.9 * std::min(sz.x / std::max(wm, 1.0), sz.y / std::max(hm, 1.0)));
}

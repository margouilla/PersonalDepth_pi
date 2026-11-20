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

#include "core/Lang.h"
#include "core/WaterLevel.h"
#include "PersonalDepth_pi.h"

#include <wx/dir.h>
#include <wx/file.h>
#include <wx/fileconf.h>
#include <wx/filename.h>
#include <wx/time.h>
#include <wx/tokenzr.h>

#include <sstream>
#include <wx/utils.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <filesystem>
#include <set>
#include <cstdio>
#include <ctime>

#include "BathyDialog.h"
#include "TideChart.h"
#include "BathyRenderer.h"
#include "core/WebMerc.h"
#include "core/Soundings.h"
#include "core/SurveyShape.h"
#include "core/Triangulate.h"
#include "core/GeoGrid.h"
#include "core/Imagery.h"
#include "core/SideScan.h"

#define U8(s) wxString::FromUTF8(pd::tr(s))
#define W8(s) wxString::FromUTF8(s)  // données : pas de traduction

// libellé de tâche traduit avec un nombre (« Retrait de %d source(s) »…)
static std::string JobLabel(const char* fmt, size_t n) {
  return wxString::Format(U8(fmt), (int)n).ToStdString(wxConvUTF8);
}

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
            "LandXML TIN, GeoTIFF, BAG avec GDAL) sous forme de surface colorée ombrée "
            "et de sondes chiffrées exactes. Palette, seuils de profondeur, zone "
            "dangereuse et ombrage se modifient instantanément, sans régénérer les "
            "données — à la manière du module PBG de TimeZero.");
}

int PersonalDepth_pi::Init() {
  m_renderer = std::make_unique<BathyRenderer>();
  m_imgRenderer = std::make_unique<BathyRenderer>();
  LoadConfig();
  UpdateLanguage();  // langue de l'interface d'OpenCPN
  UpdateUnits();  // unités d'affichage d'OpenCPN (m / ft)
  if (m_set.vrefFranceDir.empty()) m_set.vrefFranceDir = DefaultReferenceDir().ToUTF8().data();
  if (m_set.paletteDir.empty()) m_set.paletteDir = DefaultPaletteDir().ToUTF8().data();
  WriteBuiltinPalettes(false);  // dossier de palettes unique : intégrées comprises
  DbOpen();

  wxString svg = GetPluginDataDir("PersonalDepth_pi") + wxFileName::GetPathSeparator() +
                 "data" + wxFileName::GetPathSeparator() + "personaldepth.svg";
  if (wxFileExists(svg))
    m_toolId = InsertPlugInToolSVG("PersonalDepth", svg, svg, svg, wxITEM_CHECK,
                                   U8("Bathymétrie personnelle"), "", nullptr, -1, 0, this);
  else
    m_toolId = InsertPlugInTool("PersonalDepth", &m_icon, &m_icon, wxITEM_CHECK,
                                U8("Bathymétrie personnelle"), "", nullptr, -1, 0, this);

  SetToolbarItemState(m_toolId, m_set.toolbarMode == 1 && m_set.shown);
  // menu contextuel de la carte : accès au panneau, utile quand l'icône
  // bascule l'affichage
  m_ctxMenuId = AddCanvasContextMenuItem(new wxMenuItem(nullptr, wxID_ANY, U8("PersonalDepth…")), this);

  m_timer = std::make_unique<Ticker>(this);
  m_timer->Start(250);
  m_cursorRefresh = std::make_unique<CursorRefresh>();

  return WANTS_OVERLAY_CALLBACK | WANTS_OPENGL_OVERLAY_CALLBACK | WANTS_CURSOR_LATLON |
         WANTS_TOOLBAR_CALLBACK | INSTALLS_TOOLBAR_TOOL | WANTS_CONFIG | WANTS_PREFERENCES |
         WANTS_MOUSE_EVENTS | INSTALLS_CONTEXTMENU_ITEMS;
}

bool PersonalDepth_pi::DeInit() {
  SaveConfig();
  if (m_ctxMenuId >= 0) RemoveCanvasContextMenuItem(m_ctxMenuId);
  if (m_timer) m_timer->Stop();
  m_timer.reset();
  if (m_cursorRefresh) m_cursorRefresh->Stop();
  m_cursorRefresh.reset();
  StopFes();
  m_dbCancel = true;
  if (m_dbThread.joinable()) m_dbThread.join();
  ResetView(m_vd, m_renderer.get());
  ResetView(m_vi, m_imgRenderer.get());
  // Fenêtres supprimées tout de suite : Destroy() les laisse à wxWidgets, qui
  // les supprime plus tard, quand OpenCPN a déjà déchargé le plugin (mise à
  // jour, désinstallation) -> appel de code disparu et plantage.
  m_events.reset();
  if (m_chart) { m_chart->Hide(); delete m_chart; m_chart = nullptr; }
  if (m_dlg) { m_dlg->Hide(); delete m_dlg; m_dlg = nullptr; }
  for (auto& l : m_layers) l->cancel();
  m_layers.clear();  // attend la fin des threads de chargement
  m_renderer.reset();
  m_imgRenderer.reset();
  return true;
}

// ------------------------------------------------------------- interface
// Icône de la barre d'outils : ouvre / ferme le panneau, ou (réglage) affiche
// / masque tout le plugin ; le panneau reste accessible par le menu du clic
// droit sur la carte, la roue du bandeau et Options › Plugins › Préférences
void PersonalDepth_pi::OnToolbarToolCallback(int) {
  if (m_set.toolbarMode == 1) {
    SetShown(!m_set.shown);
    return;
  }
  if (!m_dlg) m_dlg = new BathyDialog(GetOCPNCanvasWindow(), this);
  bool show = !m_dlg->IsShown();
  m_dlg->Show(show);
  if (show) m_dlg->Raise();
  SetToolbarItemState(m_toolId, show);
}

void PersonalDepth_pi::ShowPanel(int tab) {
  if (!m_dlg) m_dlg = new BathyDialog(GetOCPNCanvasWindow(), this);
  if (tab >= 0) m_dlg->SelectTab(tab);
  m_dlg->Show();
  m_dlg->Raise();
  if (m_set.toolbarMode == 0) SetToolbarItemState(m_toolId, true);
}

void PersonalDepth_pi::ShowPreferencesDialog(wxWindow*) { ShowPanel(); }

void PersonalDepth_pi::OnContextMenuItemCallback(int id) {
  if (id == m_ctxMenuId) ShowPanel();
}

void PersonalDepth_pi::SetShown(bool on) {
  m_set.shown = on;
  if (m_set.toolbarMode == 1) SetToolbarItemState(m_toolId, on);
  if (!on) {
    m_renderer->HideBanner();
    m_imgRenderer->HideBanner();
  }
  UpdateDbView();
  SaveConfig();
  RequestRefresh(GetOCPNCanvasWindow());
}

void PersonalDepth_pi::ToolbarModeChanged() {
  // icône enfoncée : panneau ouvert (mode 0) ou plugin affiché (mode 1)
  if (m_set.toolbarMode == 1) SetToolbarItemState(m_toolId, m_set.shown);
  else {
    if (!m_set.shown) SetShown(true);  // plus de bascule : ne pas laisser le plugin masqué
    SetToolbarItemState(m_toolId, m_dlg && m_dlg->IsShown());
  }
  SaveConfig();
}

void PersonalDepth_pi::OnDialogClose() {
  if (m_set.toolbarMode == 0) SetToolbarItemState(m_toolId, false);
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
  if (!m_set.bathyShow) UpdateDbView();
  RequestRefresh(GetOCPNCanvasWindow());
}

void PersonalDepth_pi::InterpDisplayChanged() {
  ++m_dbEpoch;  // vue de la base reconstruite (zones interpolées omises ou non)
  ColorsChanged();
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
  if (!m_set.bathyShow) UpdateDbView();  // isobathes / sondes sans les couleurs : vue à charger ou libérer
  RequestRefresh(GetOCPNCanvasWindow());
}

// Unités choisies dans Options › Affichage › Unités d'OpenCPN. Les brasses ne
// sont pas gérées : une profondeur en brasses est affichée en pieds.
bool PersonalDepth_pi::UpdateUnits() {
  const double kd = toUsrDepth_Plugin(1.0), kh = toUsrHeight_Plugin(1.0);
  const bool depthFeet = std::fabs(kd - 1.0) > 1e-3, heightFeet = std::fabs(kh - 1.0) > 1e-3;
  const float dK = depthFeet ? 1.f / 0.3048f : 1.f, hK = heightFeet ? 1.f / 0.3048f : 1.f;
  if (dK == m_set.depthK && hK == m_set.heightK) return false;
  m_set.depthK = dK;
  m_set.depthUnit = depthFeet ? "ft" : "m";
  m_set.heightK = hK;
  m_set.heightUnit = heightFeet ? "ft" : "m";
  return true;
}

void PersonalDepth_pi::EncContours(float& shallow, float& safety, float& deep) {
  double sh = 2, sa = 3, de = 6;  // valeurs par défaut d'OpenCPN
  if (wxFileConfig* c = GetOCPNConfigObject()) {
    const wxString old = c->GetPath();
    c->SetPath("/Settings/GlobalState");
    c->Read("S52_MAR_SHALLOW_CONTOUR", &sh, sh);
    c->Read("S52_MAR_SAFETY_CONTOUR", &sa, sa);
    c->Read("S52_MAR_DEEP_CONTOUR", &de, de);
    c->SetPath(old);
  }
  shallow = (float)sh;
  safety = (float)sa;
  deep = (float)de;
}

// Langue choisie dans Options › Utilisateur › Options générales › Langue
// d'OpenCPN : français si la locale commence par « fr », anglais sinon.
// Langue de l'interface d'OpenCPN : français, anglais, russe, ou toute langue
// dont un fichier de traduction PersonalDepth_<code>.po est présent (sinon
// anglais). Fichiers lus : celui livré avec le plugin (data/lang), puis la copie
// de l'utilisateur (dossier lang du dossier utilisateur du plugin), prioritaire.
bool PersonalDepth_pi::UpdateLanguage() {
  const wxString loc = GetLocaleCanonicalName().Lower();
  const wxString code = loc.Left(2);
  if (code == m_langCode && m_lang >= 0) return false;
  m_langCode = code;
  int lang = (loc.empty() || code == "fr") ? pd::kFrench : code == "en" ? pd::kEnglish : pd::kOther;
  pd::ClearTranslations();
  if (lang == pd::kOther) {
    const wxString sep = wxFileName::GetPathSeparator();
    const wxString name = "PersonalDepth_" + code + ".po";
    const wxString files[] = {
        GetPluginDataDir("PersonalDepth_pi") + sep + "data" + sep + "lang" + sep + name,
        wxFileName(DefaultPaletteDir()).GetPath() + sep + "lang" + sep + name};
    std::vector<std::string> paths;
    for (const wxString& f : files)
      if (wxFileExists(f)) paths.push_back(f.ToUTF8().data());
    std::string notes;
    if (!paths.empty() && pd::LoadTranslations(paths, &notes) > 0) {
      if (!notes.empty()) wxLogMessage("PersonalDepth_pi: %s", wxString::FromUTF8(notes.c_str()));
    } else {
      pd::ClearTranslations();
    }
    // russe : table compilée en repli ; autre langue sans fichier : anglais
    lang = code == "ru" ? pd::kRussian : pd::HasTranslations() ? pd::kOther : pd::kEnglish;
  }
  m_lang = lang;
  pd::SetLanguage(lang);
  return true;
}

// Journal des temps (PerfLog=1 dans opencpn.ini, section [PlugIns/PersonalDepth]) :
// une ligne toutes les 3 s d'activité dans le journal d'OpenCPN
void PersonalDepth_pi::PerfLogTick() {
  if (!m_set.perfLog || !m_renderer) return;
  const wxLongLong now = wxGetLocalTimeMillis();
  if (m_perfLast == 0) m_perfLast = now;
  const double span = (now - m_perfLast).ToDouble();
  if (span < 3000) return;
  BathyRenderer::Perf& p = m_renderer->m_perf;
  if (p.frames || m_perfViewsStarted) {
    const double n = std::max(1, p.frames);
    wxLogMessage(
        "PersonalDepth_pi perf: %d images en %.1f s (%.1f/s) ; plugin moy %.1f ms, max %.1f ms ; "
        "fond %.1f ms/image dont envoi de %d tuiles %.0f ms ; isobathes %.1f ms/image ; "
        "sondes et valeurs recalculées %d fois, %.0f ms ; vues : %d lancées, %d arrêtées, %d prêtes "
        "(calcul moy %.0f ms, max %.0f ms) ; niveau visé %d",
        p.frames, span / 1000.0, p.frames * 1000.0 / span, p.totalMs / n, p.maxMs, p.tileMs / n, p.uploads,
        p.uploadMs, p.contourMs / n, p.labelRuns, p.labelMs, m_perfViewsStarted, m_perfViewsCancelled,
        m_perfViewsReady, m_perfViewsReady ? m_perfViewMs / m_perfViewsReady : 0.0, m_perfViewMaxMs,
        m_vd.target);
  }
  p = BathyRenderer::Perf();
  m_perfViewsStarted = m_perfViewsCancelled = m_perfViewsReady = 0;
  m_perfViewMs = m_perfViewMaxMs = 0;
  m_perfLast = now;
}

void PersonalDepth_pi::OnTick() {
  PerfLogTick();
  // sondes et valeurs décalées pendant le déplacement : nouvelle sélection à l'arrêt
  if (m_renderer && m_renderer->LabelsShifted() && (wxGetLocalTimeMillis() - m_vpMoveTime).ToDouble() > 300) {
    m_renderer->RefreshLabels();
    RequestRefresh(GetOCPNCanvasWindow());
  }
  if (UpdateLanguage()) {  // langue changée : fenêtres reconstruites dans la nouvelle langue
    const bool dlgShown = m_dlg && m_dlg->IsShown(), chartShown = m_chart && m_chart->IsShown();
    wxPoint pos = m_dlg ? m_dlg->GetPosition() : wxDefaultPosition;
    if (m_chart) { m_chart->Destroy(); m_chart = nullptr; }
    if (m_dlg) { m_dlg->Destroy(); m_dlg = nullptr; }
    ++m_set.version;
    m_renderer->Invalidate();
    m_autoKey.clear();  // palette automatique recalculée dès que les données sont prêtes
    UpdateAutoPalette(true);
    UpdateTide(false);
    UpdateCursorText();
    if (dlgShown) {
      m_dlg = new BathyDialog(GetOCPNCanvasWindow(), this);
      m_dlg->Move(pos);
      m_dlg->Show();
    }
    if (chartShown) ShowTideChart();
    RequestRefresh(GetOCPNCanvasWindow());
  }
  if (UpdateUnits()) {  // unités changées dans les options d'OpenCPN
    ++m_set.version;
    m_renderer->Invalidate();
    m_autoKey.clear();  // palette automatique recalculée dès que les données sont prêtes
    UpdateAutoPalette(true);
    UpdateTide(false);
    UpdateCursorText();
    if (m_dlg) m_dlg->RefreshUnits();
    RequestRefresh(GetOCPNCanvasWindow());
  }
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
    if (m_dbJobKind == DbKind::Imagery) {
      m_imgSources = m_imgSourcesNew;
      ++m_imgEpoch;
    } else {
      m_dbSources = m_dbSourcesNew;
      m_dbCells = m_dbCellsNew;
      m_dbCellDeg = m_dbCellDegNew;
      ++m_dbEpoch;
    }
    {
      std::lock_guard<std::mutex> lk(m_dbMx);
      m_dbLast = m_dbErr.empty() ? m_dbTitle + U8(" : terminé").ToStdString(wxConvUTF8)
                                 : m_dbTitle + " : " + m_dbErr;
    }
    bool estAny = false;
    {  // décalage sonde / antenne estimé : affiché dans les réglages
      std::lock_guard<std::mutex> lk(m_dbMx);
      bool& any = estAny;
      if (m_estTracks.set) {
        m_set.importAntEstOk = m_estTracks.ok;
        m_set.importAntEstFwd = (float)m_estTracks.fwd;
        m_set.importAntEstStbd = (float)m_estTracks.stbd;
        m_estTracks = LeverEstimate();
        any = true;
      }
      if (m_estSonar.set) {
        m_set.imgSonarOffsetEstOk = m_estSonar.ok;
        m_set.imgSonarOffsetEstFwd = m_estSonar.fwd;
        m_set.imgSonarOffsetEstStbd = m_estSonar.stbd;
        m_estSonar = LeverEstimate();
        any = true;
      }
    }
    if (estAny) SaveConfig();
    if (m_dlg) m_dlg->RefreshDb();
    if (m_dlg) m_dlg->RefreshImageryDisplay();
    if (m_set.tideMode) UpdateTide(false);
  } else if (m_dbBusy && m_dlg) {
    m_dlg->SetDbStatus(DbStatus());
  }
  UpdateDbView();
  UpdateAutoPalette(false);
  UpdateImgAuto(false);
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

// Dossier des palettes de l'utilisateur (créé au premier enregistrement)
wxString PersonalDepth_pi::DefaultPaletteDir() {
  const wxString sep = wxFileName::GetPathSeparator();
  wxString local;
  if (wxGetEnv("LOCALAPPDATA", &local))
    return local + sep + "opencpn" + sep + "plugins" + sep + "PersonalDepth_pi" + sep + "palettes";
  const wxString* priv = GetpPrivateApplicationDataLocation();
  if (priv && !priv->empty())
    return *priv + sep + "plugins" + sep + "PersonalDepth_pi" + sep + "palettes";
  return GetPluginDataDir("PersonalDepth_pi") + sep + "palettes";
}

// Dossier de palettes unique : les palettes intégrées y sont écrites en .cpt (on
// peut les modifier ou les supprimer) ; seule la palette ENC (S-52), calculée
// d'après les isobathes d'OpenCPN, reste à part.
bool PersonalDepth_pi::WriteBuiltinPalettes(bool restore, wxString* err) {
  const wxString dir = W8(m_set.paletteDir.c_str());
  if (dir.empty()) return false;
  const wxString sep = wxFileName::GetPathSeparator();
  const wxString marker = dir + sep + ".personaldepth-builtin";
  if (!restore && wxFileExists(marker)) return true;  // déjà fait : suppressions respectées
  if (!wxDirExists(dir) && !wxFileName::Mkdir(dir, wxS_DIR_DEFAULT, wxPATH_MKDIR_FULL)) {
    if (err) *err = U8("Impossible de créer le dossier ") + dir;
    return false;
  }
  bool ok = true;
  for (const auto& n : ColorRamp::PresetNames()) {
    if (n == ColorRamp::kEncName) continue;
    wxString name = U8(n.c_str());
    for (wxUniChar bad : wxString("\\/:*?\"<>|")) name.Replace(wxString(bad), "_");
    const wxString path = dir + sep + name + ".cpt";
    if (!restore && wxFileExists(path)) continue;
    ok = ColorRamp::Preset(n).saveCPT(path.ToUTF8().data()) && ok;
  }
  wxFile m;
  if (m.Create(marker, true)) m.Write(wxString("PersonalDepth_pi: built-in palettes written\n"));
  if (!ok && err) *err = U8("Échec de l'écriture");
  return ok;
}

std::vector<PersonalDepth_pi::PaletteEntry> PersonalDepth_pi::PaletteList() const {
  std::vector<PaletteEntry> v;
  v.push_back({U8(ColorRamp::kEncName), ColorRamp::kEncName});
  const wxString dir = W8(m_set.paletteDir.c_str());
  if (!dir.empty() && wxDirExists(dir)) {
    wxArrayString files;
    wxDir::GetAllFiles(dir, &files, "*.cpt", wxDIR_FILES);
    files.Sort();
    for (const auto& f : files)
      v.push_back({wxFileName(f).GetName(), std::string("file:") + f.ToUTF8().data()});
  }
  return v;
}

// -------------------------------------------------------------- marée
// Dossier des références verticales livrées avec le plugin
wxString PersonalDepth_pi::DefaultReferenceDir() {
  const wxString sep = wxFileName::GetPathSeparator();
  wxString a = GetPluginDataDir("PersonalDepth_pi") + sep + "references";
  if (wxDirExists(a)) return a;
  wxString local;  // Windows : dossier des plugins de l'utilisateur
  if (wxGetEnv("LOCALAPPDATA", &local))
    return local + sep + "opencpn" + sep + "plugins" + sep + "PersonalDepth_pi" + sep + "references";
  // Linux, Flatpak, macOS : dossier de données privé d'OpenCPN
  const wxString* priv = GetpPrivateApplicationDataLocation();
  if (priv && !priv->empty())
    return *priv + sep + "plugins" + sep + "PersonalDepth_pi" + sep + "references";
  return a;
}


void PersonalDepth_pi::TideChanged() {
  UpdateTide(true);
  SaveConfig();
  if (m_dlg) m_dlg->RefreshDatum();  // avec la marée : zéro des cartes + hauteur d'eau
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
// Zone de la marée : emprise des sources visibles qui touchent la carte
// affichée (pas toutes les sources de la base : levés en France et au
// Canada donnaient un centre au milieu de l'Atlantique) ; sans source à
// l'écran, la carte elle-même (arrondie au 1/20° pour limiter les calculs).
// `sig` : signature de la zone, pour la recalculer quand elle change.
bool PersonalDepth_pi::TideZone(double box[4], std::string* sig) const {
  double la0 = 1e9, la1 = -1e9, lo0 = 1e9, lo1 = -1e9;
  const double* v = m_vpBox;
  auto inView = [&](double x0, double x1, double y0, double y1) {
    return !m_vpValid || (x1 >= v[0] && x0 <= v[1] && y1 >= v[2] && y0 <= v[3]);
  };
  std::string s;
  auto add = [&](double x0, double x1, double y0, double y1) {
    la0 = std::min(la0, y0); la1 = std::max(la1, y1);
    lo0 = std::min(lo0, x0); lo1 = std::max(lo1, x1);
  };
  for (auto& l : m_layers)
    if (l->visible && l->state() == BathyLayer::Ready &&
        inView(l->cloud.lonMin, l->cloud.lonMax, l->cloud.latMin, l->cloud.latMax)) {
      add(l->cloud.lonMin, l->cloud.lonMax, l->cloud.latMin, l->cloud.latMax);
      s += "L" + std::to_string((uintptr_t)l.get()) + ",";
    }
  for (const auto& d : m_dbSources)
    if (d.visible && inView(d.lonMin, d.lonMax, d.latMin, d.latMax)) {
      add(d.lonMin, d.lonMax, d.latMin, d.latMax);
      s += std::to_string(d.id) + ",";
    }
  if (la0 > la1 && m_vpValid && (!m_dbSources.empty() || !m_layers.empty())) {
    lo0 = std::floor(v[0] * 20) / 20; lo1 = std::ceil(v[1] * 20) / 20;
    la0 = std::floor(v[2] * 20) / 20; la1 = std::ceil(v[3] * 20) / 20;
    char b[96];
    std::snprintf(b, sizeof b, "V%.2f %.2f %.2f %.2f", lo0, lo1, la0, la1);
    s = b;
  }
  if (sig) *sig = s;
  if (la0 > la1) return false;
  box[0] = lo0; box[1] = lo1; box[2] = la0; box[3] = la1;
  return true;
}

void PersonalDepth_pi::UpdateTide(bool findStation) {
  DisplaySettings& s = m_set;
  const float oldH = s.tideH;
  const bool oldOk = s.tideOk;
  const auto oldTide = s.tide;
  s.tideOk = false;
  s.tideH = 0.f;
  s.tideLegend.clear();
  s.tideWhen.clear();
  auto setUniform = [&](float h) {
    if (oldTide && !oldTide->gridded() && oldTide->h0 == h) { s.tide = oldTide; return; }
    auto f = std::make_shared<TideField>();
    f->h0 = h;
    f->finish();
    f->version = ++m_tideVersion;
    s.tide = f;
  };
  auto compute = [&]() -> wxString {
    if (s.tideMode == 0)
      return U8("Profondeurs : ") + W8(DatumName(DisplayDatumOf(s.dispDatum)).c_str()) + U8(" (sans marée)");
    double zb[4];
    if (!TideZone(zb, nullptr)) { m_tideBoxOk = false; return U8("En attente d'une couche chargée"); }
    const double lo0 = zb[0], lo1 = zb[1], la0 = zb[2], la1 = zb[3];
    m_tideBox[0] = lo0; m_tideBox[1] = lo1; m_tideBox[2] = la0; m_tideBox[3] = la1;
    m_tideBoxOk = true;
    const double clat = 0.5 * (la0 + la1), clon = 0.5 * (lo0 + lo1);
    const time_t t = TideChartTime();
    const wxString when = wxDateTime((time_t)t).Format("%d/%m %H:%M");
    // bandeau : date courte JJ/MM/AA HH:MM (français) ou JJ Mmm AA HH:MM (anglais)
    {
      static const char* kMon[] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun",
                                   "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
      const wxDateTime dt((time_t)t);
      s.tideWhen = pd::IsEnglish()
                       ? wxString::Format("%02d %s %02d %s", dt.GetDay(), kMon[(int)dt.GetMonth()],
                                          dt.GetYear() % 100, dt.Format("%H:%M"))
                   : pd::IsRussian() ? dt.Format("%d.%m.%y %H:%M")  // usage russe : JJ.MM.AA
                                     : dt.Format("%d/%m/%y %H:%M");
    }
    auto legend = [&](float h) {
      return wxString::Format("%s %+.2f %s", U8("Marée"), h * s.heightK, s.heightUnit);
    };

    if (s.tideSource == 1) {  // ------------------------------- FES2022 maillé
      if (s.tideAtlas.empty() || !wxFileExists(W8(s.tideAtlas.c_str())))
        return U8("Choisir le fichier de marée FES2022 (.pdtide)");
      // emprise arrondie au 1/50° : pas de recalcul pour un simple décalage
      const double bb[4] = {std::floor(lo0 * 50) / 50, std::ceil(lo1 * 50) / 50,
                            std::floor(la0 * 50) / 50, std::ceil(la1 * 50) / 50};
      TideZ0Params z0;
      wxStringTokenizer tk(W8(s.tideZ0Files.c_str()), ";");
      while (tk.HasMoreTokens()) {
        const wxString f = tk.GetNextToken().Trim().Trim(false);
        if (!f.empty()) z0.userFiles.push_back(f.ToUTF8().data());
      }
      const wxString dir = W8(s.vrefFranceDir.c_str());
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
      const float hk = s.heightK;
      const wxString hu = s.heightUnit;
      s.tideLegend = legend(hc);
      wxString info = wxString::Format(
          U8("FES2022 le %s : hauteur d'eau %+.2f %s au centre (FES %+.2f %s sur le NM, "
             "Z0 %.2f %s), de %+.2f à %+.2f %s sur la zone\n%s"),
          when, hc * hk, hu, nm * hk, hu, z * hk, hu, F.hMin * hk, F.hMax * hk, hu,
          U8(m_fes->info().c_str()));
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
      m_tideStationLat = st.lat; m_tideStationLon = st.lon;
      m_tideRefLat = clat; m_tideRefLon = clon;
    }
    float h = 0;
    if (!a->GetTideHeight(m_tideStation, t, &h) || !std::isfinite(h))
      return U8("Hauteur de marée indisponible pour ") + m_tideStationName;
    s.tideH = h;
    s.tideOk = true;
    setUniform(h);
    s.tideLegend = legend(h);
    wxString info = wxString::Format(U8("Station %s (%.1f km) : hauteur d'eau %+.2f %s le %s"),
                                     m_tideStationName, m_tideStationDistKm, h * s.heightK,
                                     s.heightUnit, when);
    if (m_tideStationDistKm > 30) info += U8(" — station éloignée, à vérifier");
    return info;
  };
  s.tideInfo = compute();
  if (m_chart && m_chart->IsShown()) m_chart->TideUpdated();  // marqueur, points
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

// Emprise de l'écran : rectangle lon/lat et, carte tournée, ses 4 coins
void PersonalDepth_pi::StoreViewport(PlugIn_ViewPort* vp) {
  if (vp->clat != m_vpCenter[0] || vp->clon != m_vpCenter[1]) {  // carte déplacée
    m_vpCenter[0] = vp->clat;
    m_vpCenter[1] = vp->clon;
    m_vpMoveTime = wxGetLocalTimeMillis();
  }
  m_vpBox[0] = vp->lon_min; m_vpBox[1] = vp->lon_max; m_vpBox[2] = vp->lat_min; m_vpBox[3] = vp->lat_max;
  m_vpValid = true;
  const wxPoint c[4] = {wxPoint(0, 0), wxPoint(vp->pix_width, 0),
                        wxPoint(vp->pix_width, vp->pix_height), wxPoint(0, vp->pix_height)};
  m_vpQuadOk = vp->pix_width > 0 && vp->pix_height > 0;
  for (int k = 0; k < 4 && m_vpQuadOk; ++k) {
    double lat, lon;
    GetCanvasLLPix(vp, c[k], &lat, &lon);
    if (k) {  // longitudes à moins de 180° du premier coin
      while (lon - m_vpQuad.x[0] > 180.0) lon -= 360.0;
      while (lon - m_vpQuad.x[0] < -180.0) lon += 360.0;
    }
    if (!std::isfinite(lat) || !std::isfinite(lon) || std::fabs(lat) > 85.0) m_vpQuadOk = false;
    m_vpQuad.x[k] = lon;
    m_vpQuad.y[k] = ScreenMapper::mercY(lat);
  }
  // emprise hors de [-180, 180] (antiméridien) : rectangle seul
  for (int k = 0; k < 4 && m_vpQuadOk; ++k)
    if (m_vpQuad.x[k] < -180.0 || m_vpQuad.x[k] > 180.0) m_vpQuadOk = false;
  if (m_vpQuadOk && m_vpQuad.area() <= 0) m_vpQuadOk = false;
}

// Bathymétrie et imagerie : l'imagerie dessous (par défaut) ou au-dessus des
// couleurs de profondeur ; isobathes, sondes, bandeau et curseur toujours
// au-dessus de tout
bool PersonalDepth_pi::RenderOverlayMultiCanvas(wxDC& dc, PlugIn_ViewPort* vp, int,
                                                int priority) {
  if (!vp) return false;
  StoreViewport(vp);
  const auto layers = DrawLayers();
  const auto img = ImgLayers();
  if (layers.empty() && img.empty()) return false;
  if (priority == OVERLAY_OVER_UI && m_hostHasChartsPriority) {
    m_renderer->RenderCursorDC(dc, vp, CursorTextFor(priority), m_set.fontSize);
    return true;
  }
  if (!ShouldDraw(priority)) return false;
  const wxString cur = CursorTextFor(priority);
  m_set.bannerBathy = !layers.empty() && m_set.bathyShow;  // sections du bandeau
  m_set.bannerImg = !img.empty();
  m_set.bannerChart = !layers.empty();
  // couleurs de profondeur masquées : isobathes et sondes seules
  const bool tiles = m_set.bathyShow;
  m_renderer->SetTilesHidden(!tiles);
  const int bathyAll = tiles ? BathyRenderer::kAll : BathyRenderer::kOverlays;
  if (img.empty()) {
    m_renderer->RenderDC(dc, vp, layers, m_set, cur, bathyAll);
  } else if (layers.empty()) {
    m_imgRenderer->RenderDC(dc, vp, img, ImgSettings(), wxString(), BathyRenderer::kTiles);
    m_renderer->RenderBannerDC(dc, vp, m_set);
    m_renderer->RenderCursorDC(dc, vp, cur, m_set.fontSize);
  } else if (m_set.imgOnTop && tiles) {
    m_renderer->RenderDC(dc, vp, layers, m_set, cur, BathyRenderer::kTiles);
    m_imgRenderer->RenderDC(dc, vp, img, ImgSettings(), wxString(), BathyRenderer::kTiles);
    m_renderer->RenderDC(dc, vp, layers, m_set, cur, BathyRenderer::kOverlays);
  } else {  // isobathes, sondes, bandeau et curseur au-dessus de l'imagerie
    m_imgRenderer->RenderDC(dc, vp, img, ImgSettings(), wxString(), BathyRenderer::kTiles);
    m_renderer->RenderDC(dc, vp, layers, m_set, cur, bathyAll);
  }
  m_lastPpd = (layers.empty() ? m_imgRenderer : m_renderer)->LastPixelsPerDegLon();
  m_lastRenderGL = false;
  return true;
}

bool PersonalDepth_pi::RenderGLOverlayMultiCanvas(wxGLContext*, PlugIn_ViewPort* vp, int,
                                                  int priority) {
  if (!vp) return false;
  StoreViewport(vp);
  const auto layers = DrawLayers();
  const auto img = ImgLayers();
  if (layers.empty() && img.empty()) return false;
  if (priority == OVERLAY_OVER_UI && m_hostHasChartsPriority) {
    m_renderer->RenderCursorGL(vp, CursorTextFor(priority), m_set.fontSize);
    return true;
  }
  if (!ShouldDraw(priority)) return false;
  const wxString cur = CursorTextFor(priority);
  m_set.bannerBathy = !layers.empty() && m_set.bathyShow;  // sections du bandeau
  m_set.bannerImg = !img.empty();
  m_set.bannerChart = !layers.empty();
  // couleurs de profondeur masquées : isobathes et sondes seules
  const bool tiles = m_set.bathyShow;
  m_renderer->SetTilesHidden(!tiles);
  const int bathyAll = tiles ? BathyRenderer::kAll : BathyRenderer::kOverlays;
  if (img.empty()) {
    m_renderer->RenderGL(vp, layers, m_set, cur, bathyAll);
  } else if (layers.empty()) {
    m_imgRenderer->RenderGL(vp, img, ImgSettings(), wxString(), BathyRenderer::kTiles);
    m_renderer->RenderBannerGL(vp, m_set);
    m_renderer->RenderCursorGL(vp, cur, m_set.fontSize);
  } else if (m_set.imgOnTop && tiles) {
    m_renderer->RenderGL(vp, layers, m_set, cur, BathyRenderer::kTiles);
    m_imgRenderer->RenderGL(vp, img, ImgSettings(), wxString(), BathyRenderer::kTiles);
    m_renderer->RenderGL(vp, layers, m_set, cur, BathyRenderer::kOverlays);
  } else {  // isobathes, sondes, bandeau et curseur au-dessus de l'imagerie
    m_imgRenderer->RenderGL(vp, img, ImgSettings(), wxString(), BathyRenderer::kTiles);
    m_renderer->RenderGL(vp, layers, m_set, cur, bathyAll);
  }
  m_lastPpd = (layers.empty() ? m_imgRenderer : m_renderer)->LastPixelsPerDegLon();
  m_lastRenderGL = true;
  // nouvelle vue en préparation : images suivantes demandées tout de suite
  const bool pending = (m_renderer->Pending() && !m_renderer->PendingDone()) ||
                       (m_imgRenderer->Pending() && !m_imgRenderer->PendingDone());
  if (pending && m_cursorRefresh && !m_cursorRefresh->IsRunning()) m_cursorRefresh->StartOnce(20);
  return true;
}

void PersonalDepth_pi::SetCursorLatLon(double lat, double lon) {
  m_curLat = lat; m_curLon = lon;
  if (!m_set.legend || (DrawLayers().empty() && ImgLayers().empty())) {
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
    bool gi = false;
    if (!(*it)->probe(m_curLon, m_curLat, radiusDeg, gz, sz, dist, &sid, &gi)) continue;
    const float v = !std::isnan(sz) ? sz : gz;
    const float dk = m_set.depthK;
    const wxString du = m_set.depthUnit;
    wxString base = U8(!std::isnan(sz) ? "Sonde " : "Grille ") + wxString::Format("%.2f %s ", v * dk, du) +
                    DatumShort();
    if (m_set.tideOk && m_set.tide) {  // profondeur au ZH puis profondeur du moment
      const float th = m_set.tide->at(m_curLon, m_curLat);
      base = U8(!std::isnan(sz) ? "Sonde " : "Grille ") +
             wxString::Format(U8("%.2f %s ZH → %.2f %s (marée %+.2f %s)"), v * dk, du, (v + th) * dk, du,
                              th * m_set.heightK, m_set.heightUnit);
    }
    if (!std::isnan(sz)) base += wxString::Format(U8("  (a %.1f m)"), dist);
    else if (gi) base += U8("  (interpolé)");  // valeur interpolée entre les sondes
    if (sid)  // vue de la base : source retenue pour ce pixel
      for (const auto& d : m_dbSources)
        if (d.id == sid) {
          wxString n = wxString::FromUTF8(d.name.c_str());
          if (n.size() > 28) n = n.Left(26) + "..";
          base += "  [" + n + " " + wxString(d.date.substr(0, 4)) + "]";
        }
    m_cursorText = base;
    break;
  }
  const wxString im = ImgCursorText();  // imagerie sous le curseur (valeur d'origine)
  if (!im.empty()) m_cursorText = m_cursorText.empty() ? im : m_cursorText + "  |  " + im;
}

// ---------------------------------------------------------- configuration
void PersonalDepth_pi::SaveConfig() {
  wxFileConfig* c = GetOCPNConfigObject();
  if (!c) return;
  c->DeleteGroup("/PlugIns/PersonalDepth");
  c->SetPath("/PlugIns/PersonalDepth");
  c->Write("Ramp", W8(m_set.ramp.serialize().c_str()));
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
  if (m_set.perfLog) c->Write("PerfLog", m_set.perfLog);
  c->Write("Banner", m_set.banner);
  c->Write("BannerPos", m_set.bannerPos);
  c->Write("BannerFx", (double)m_set.bannerFx);
  c->Write("BannerFy", (double)m_set.bannerFy);
  c->Write("BannerLock", m_set.bannerLock);
  c->Write("Shown", m_set.shown);
  c->Write("ToolbarMode", m_set.toolbarMode);
  c->Write("PanelTab", m_set.panelTab);
  c->Write("BannerVertical", m_set.bannerVertical);
  c->Write("BathyShow", m_set.bathyShow);
  c->Write("ImgShow", m_set.imgShow);
  c->Write("ImgOpacity", m_set.imgOpacity);
  c->Write("ImgOnTop", m_set.imgOnTop);
  c->Write("ImgPalette", m_set.imgPalette);
  c->Write("ImgInvert", m_set.imgInvert);
  c->Write("ImgMin", m_set.imgMin);
  c->Write("ImgMax", m_set.imgMax);
  c->Write("ImgGamma", (double)m_set.imgGamma);
  c->Write("ImgSmooth", m_set.imgSmooth);
  c->Write("ImgBgMode", m_set.imgBgMode);
  c->Write("ImgBgValue", m_set.imgBgValue);
  c->Write("ImgAuto", m_set.imgAuto);
  c->Write("ImgAutoSource", m_set.imgAutoSource);
  c->Write("ImgSonarPixel", m_set.imgSonarPixel);
  c->Write("ImgSonarEqualize", m_set.imgSonarEqualize);
  c->Write("ImgSonarOffsetAuto", m_set.imgSonarOffsetAuto);
  c->Write("ImgSonarOffsetFwd", m_set.imgSonarOffsetFwd);
  c->Write("ImgSonarOffsetStbd", m_set.imgSonarOffsetStbd);
  c->Write("ImgSonarOffsetEstOk", m_set.imgSonarOffsetEstOk);
  c->Write("ImgSonarOffsetEstFwd", m_set.imgSonarOffsetEstFwd);
  c->Write("ImgSonarOffsetEstStbd", m_set.imgSonarOffsetEstStbd);
  c->Write("Contours", m_set.contours);
  c->Write("ContourMode", m_set.contourMode);
  c->Write("ContourStep", (double)m_set.contourStep);
  c->Write("ContourMajor", (double)m_set.contourMajor);
  c->Write("ContourLabels", m_set.contourLabels);
  c->Write("ContourSmooth", m_set.contourSmooth);
  c->Write("ContourColor", (long)m_set.contourColor);
  c->Write("CrsCode", W8(m_set.crsCode.c_str()));
  c->Write("CrsZone", m_set.crsZone);
  c->Write("CrsSouth", m_set.crsSouth);
  c->Write("CrsCustom", W8(m_set.crsCustom.c_str()));
  c->Write("ReadPrj", m_set.readPrj);
  c->Write("VrefChoice2", m_set.vrefMode);  // (« VrefChoice » : ancienne numérotation)
  c->Write("DisplayDatum", m_set.dispDatum);
  c->Write("MssGrid", W8(m_set.mssGrid.c_str()));
  c->Write("NationalDir", W8(m_set.nationalDir.c_str()));
  c->Write("LandMask", W8(m_set.landMask.c_str()));
  c->Write("LandMaskOn", m_set.landMaskOn);
  c->Write("ShoreFiles", W8(m_set.shoreFiles.c_str()));
  c->Write("ShoreZero", m_set.shoreZero);
  c->Write("ImportOrder", m_set.importOrder);
  c->Write("ImportSign", m_set.importSign);
  c->Write("ImportOffset", (double)m_set.importOffset);
  c->Write("ImportAntAuto", m_set.importAntAuto);
  c->Write("ImportAntFwd", (double)m_set.importAntFwd);
  c->Write("ImportAntStbd", (double)m_set.importAntStbd);
  c->Write("ImportAntEstOk", m_set.importAntEstOk);
  c->Write("ImportAntEstFwd", (double)m_set.importAntEstFwd);
  c->Write("ImportAntEstStbd", (double)m_set.importAntEstStbd);
  c->Write("ImportZFeet", m_set.importZFeet);
  c->Write("ImportRes", (double)m_set.importRes);
  c->Write("ImportInstrument", m_set.importInstrument);
  c->Write("ImportNature", m_set.importNature);
  c->Write("ImportInterp", m_set.importInterp);
  c->Write("ImportInterpD", (double)m_set.importInterpD);
  c->Write("ImportInterpMode", m_set.importInterpMethod);
  c->Write("InterpShow", m_set.interpShow);
  c->Write("InterpDistinct", m_set.interpDistinct);
  c->Write("ImportFormat", W8(m_set.importFormat.c_str()));
  c->Write("ImportWaterOn", m_set.importWaterOn);
  c->Write("ImportWater", W8(m_set.importWater.c_str()));
  c->Write("ImportWaterSign", m_set.importWaterSign);
  c->Write("ImportWaterFormat", W8(m_set.importWaterFormat.c_str()));
  c->Write("ImportWaterFeet", m_set.importWaterFeet);
  c->Write("ImportCache", m_set.importCache);
  c->Write("ImportNodataOn", m_set.importNodataOn);
  c->Write("ImportNodata", W8(m_set.importNodata.c_str()));
  c->Write("ImportBoundsOn", m_set.importBoundsOn);
  c->Write("BoundMin", (double)m_set.boundMin);
  c->Write("BoundMax", (double)m_set.boundMax);
  c->Write("VrefFranceDir", W8(m_set.vrefFranceDir.c_str()));
  c->Write("PaletteDir", W8(m_set.paletteDir.c_str()));
  c->Write("VrefZh", W8(m_set.vrefZh.c_str()));
  c->Write("VrefGeoid", W8(m_set.vrefGeoid.c_str()));
  c->Write("VrefDirect", W8(m_set.vrefDirect.c_str()));
  c->Write("VrefInvert", m_set.vrefInvert);
  c->Write("TideMode", m_set.tideMode);
  c->Write("TideTime", wxString::Format("%lld", m_set.tideTime));
  c->Write("TideShift", m_set.tideShiftMin);
  c->Write("TideSource", m_set.tideSource);
  c->Write("TideAtlas", W8(m_set.tideAtlas.c_str()));
  c->Write("TideZ0", W8(m_set.tideZ0Files.c_str()));
  c->Write("TideBathyElli", m_set.tideUseBathyElli);
  c->Write("DbFolder", W8(m_set.dbFolder.c_str()));
  c->Write("AutoPalette", m_set.autoPalette);
  c->Write("AutoSource", m_set.autoSource);
  c->Write("AutoCount", m_set.autoCount);
  c->Write("AutoNice", m_set.autoNice);
  c->Write("AutoBase", W8(m_set.autoBase.c_str()));
  c->Write("AutoMinOn", m_set.autoMinOn);
  c->Write("AutoMaxOn", m_set.autoMaxOn);
  c->Write("AutoMin", (double)m_set.autoMin);
  c->Write("AutoMax", (double)m_set.autoMax);
  c->Write("DbPriority", m_set.dbPriority);
  {
    std::string h;
    for (const auto& n : m_set.dbHidden) h += (h.empty() ? "" : ";") + n;
    c->Write("DbHidden", W8(h.c_str()));
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
  c->Read("PerfLog", &m_set.perfLog, m_set.perfLog);
  c->Read("Banner", &m_set.banner, m_set.banner);
  c->Read("BannerPos", &m_set.bannerPos, m_set.bannerPos);
  {
    double fx = m_set.bannerFx, fy = m_set.bannerFy;
    c->Read("BannerFx", &fx, fx);
    c->Read("BannerFy", &fy, fy);
    m_set.bannerFx = (float)std::min(1.0, std::max(0.0, fx));
    m_set.bannerFy = (float)std::min(1.0, std::max(0.0, fy));
  }
  c->Read("BannerLock", &m_set.bannerLock, m_set.bannerLock);
  c->Read("Shown", &m_set.shown, m_set.shown);
  c->Read("ToolbarMode", &m_set.toolbarMode, m_set.toolbarMode);
  if (m_set.toolbarMode != 1) { m_set.toolbarMode = 0; m_set.shown = true; }
  c->Read("PanelTab", &m_set.panelTab, m_set.panelTab);
  c->Read("BannerVertical", &m_set.bannerVertical, m_set.bannerVertical);
  c->Read("BathyShow", &m_set.bathyShow, m_set.bathyShow);
  c->Read("ImgShow", &m_set.imgShow, m_set.imgShow);
  c->Read("ImgOpacity", &m_set.imgOpacity, m_set.imgOpacity);
  m_set.imgOpacity = std::max(0, std::min(100, m_set.imgOpacity));
  c->Read("ImgOnTop", &m_set.imgOnTop, m_set.imgOnTop);
  c->Read("ImgPalette", &m_set.imgPalette, m_set.imgPalette);
  m_set.imgPalette = std::max(0, std::min(2, m_set.imgPalette));
  c->Read("ImgInvert", &m_set.imgInvert, m_set.imgInvert);
  c->Read("ImgMin", &m_set.imgMin, m_set.imgMin);
  c->Read("ImgMax", &m_set.imgMax, m_set.imgMax);
  m_set.imgMin = std::max(0, std::min(99, m_set.imgMin));
  m_set.imgMax = std::max(m_set.imgMin + 1, std::min(100, m_set.imgMax));
  if (c->Read("ImgGamma", &d) && d >= 0.1 && d <= 5) m_set.imgGamma = (float)d;
  c->Read("ImgSmooth", &m_set.imgSmooth, m_set.imgSmooth);
  c->Read("ImgBgMode", &m_set.imgBgMode, m_set.imgBgMode);
  c->Read("ImgBgValue", &m_set.imgBgValue, m_set.imgBgValue);
  c->Read("ImgAuto", &m_set.imgAuto, m_set.imgAuto);
  c->Read("ImgAutoSource", &m_set.imgAutoSource, m_set.imgAutoSource);
  if (c->Read("ImgSonarPixel", &d) && d >= 0 && d <= 5) m_set.imgSonarPixel = d;
  c->Read("ImgSonarEqualize", &m_set.imgSonarEqualize, m_set.imgSonarEqualize);
  c->Read("ImgSonarOffsetAuto", &m_set.imgSonarOffsetAuto, m_set.imgSonarOffsetAuto);
  if (c->Read("ImgSonarOffsetFwd", &d) && std::fabs(d) <= 50) m_set.imgSonarOffsetFwd = d;
  if (c->Read("ImgSonarOffsetStbd", &d) && std::fabs(d) <= 50) m_set.imgSonarOffsetStbd = d;
  c->Read("ImgSonarOffsetEstOk", &m_set.imgSonarOffsetEstOk, m_set.imgSonarOffsetEstOk);
  if (c->Read("ImgSonarOffsetEstFwd", &d) && std::fabs(d) <= 50) m_set.imgSonarOffsetEstFwd = d;
  if (c->Read("ImgSonarOffsetEstStbd", &d) && std::fabs(d) <= 50) m_set.imgSonarOffsetEstStbd = d;
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
  {  // référence verticale de l'import : nouvelle numérotation (niveau moyen, LAT…)
    int old = 0;
    if (!c->Read("VrefChoice2", &m_set.vrefMode) && c->Read("VrefChoice", &old)) m_set.vrefMode = OldVrefMode(old);
    m_set.vrefMode = std::max(0, std::min(kVrefModes - 1, m_set.vrefMode));
    c->Read("DisplayDatum", &m_set.dispDatum, m_set.dispDatum);
    m_set.dispDatum = std::max(0, std::min(3, m_set.dispDatum));
    if (c->Read("MssGrid", &ws)) m_set.mssGrid = ws.ToUTF8().data();
    if (c->Read("NationalDir", &ws)) m_set.nationalDir = ws.ToUTF8().data();
    if (c->Read("LandMask", &ws)) m_set.landMask = ws.ToUTF8().data();
    c->Read("LandMaskOn", &m_set.landMaskOn, m_set.landMaskOn);
    if (c->Read("ShoreFiles", &ws)) m_set.shoreFiles = ws.ToUTF8().data();
    c->Read("ShoreZero", &m_set.shoreZero, m_set.shoreZero);
  }
  c->Read("ImportOrder", &m_set.importOrder, m_set.importOrder);
  c->Read("ImportSign", &m_set.importSign, m_set.importSign);
  {
    double v;
    if (c->Read("ImportOffset", &v)) m_set.importOffset = (float)v;
    c->Read("ImportAntAuto", &m_set.importAntAuto, m_set.importAntAuto);
    if (c->Read("ImportAntFwd", &v) && std::fabs(v) <= 50) m_set.importAntFwd = (float)v;
    if (c->Read("ImportAntStbd", &v) && std::fabs(v) <= 50) m_set.importAntStbd = (float)v;
    c->Read("ImportAntEstOk", &m_set.importAntEstOk, m_set.importAntEstOk);
    if (c->Read("ImportAntEstFwd", &v) && std::fabs(v) <= 50) m_set.importAntEstFwd = (float)v;
    if (c->Read("ImportAntEstStbd", &v) && std::fabs(v) <= 50) m_set.importAntEstStbd = (float)v;
  }
  c->Read("ImportCache", &m_set.importCache, m_set.importCache);
  c->Read("ImportZFeet", &m_set.importZFeet, m_set.importZFeet);
  {
    double v;
    if (c->Read("ImportRes", &v)) m_set.importRes = (float)std::max(0.0, v);
    c->Read("ImportInstrument", &m_set.importInstrument, m_set.importInstrument);
    c->Read("ImportNature", &m_set.importNature, m_set.importNature);
    c->Read("ImportInterp", &m_set.importInterp, m_set.importInterp);
    if (c->Read("ImportInterpD", &v)) m_set.importInterpD = (float)std::max(0.0, v);
    c->Read("ImportInterpMode", &m_set.importInterpMethod, m_set.importInterpMethod);
    m_set.importInterpMethod = std::max(0, std::min(3, m_set.importInterpMethod));
    c->Read("InterpShow", &m_set.interpShow, m_set.interpShow);
    c->Read("InterpDistinct", &m_set.interpDistinct, m_set.interpDistinct);
  }
  c->Read("ImportNodataOn", &m_set.importNodataOn, m_set.importNodataOn);
  if (c->Read("ImportNodata", &ws)) m_set.importNodata = ws.ToUTF8().data();
  if (c->Read("ImportFormat", &ws)) m_set.importFormat = ws.ToUTF8().data();
  c->Read("ImportWaterOn", &m_set.importWaterOn, m_set.importWaterOn);
  if (c->Read("ImportWater", &ws)) m_set.importWater = ws.ToUTF8().data();
  c->Read("ImportWaterSign", &m_set.importWaterSign, m_set.importWaterSign);
  if (c->Read("ImportWaterFormat", &ws)) m_set.importWaterFormat = ws.ToUTF8().data();
  c->Read("ImportWaterFeet", &m_set.importWaterFeet, m_set.importWaterFeet);
  c->Read("ImportBoundsOn", &m_set.importBoundsOn, m_set.importBoundsOn);
  {
    double v;
    if (c->Read("BoundMin", &v)) m_set.boundMin = (float)v;
    if (c->Read("BoundMax", &v)) m_set.boundMax = (float)v;
  }
  if (c->Read("VrefFranceDir", &ws)) m_set.vrefFranceDir = ws.ToUTF8().data();
  if (c->Read("PaletteDir", &ws)) m_set.paletteDir = ws.ToUTF8().data();
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
// Couches de profondeur : couleurs (interrupteur Bathymétrie), mais aussi
// isobathes et sondes, affichées même quand les couleurs sont masquées
std::vector<LayerPtr> PersonalDepth_pi::DrawLayers() const {
  std::vector<LayerPtr> v;
  if (!BathyNeeded()) return v;
  if (m_vd.view && m_vd.view->state() == BathyLayer::Ready) v.push_back(m_vd.view);  // dessous
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
  m_imgSources.clear();
  ++m_imgEpoch;
  if (m_set.dbFolder.empty() || m_dbBusy) return;
  {  // base d'imagerie (.pdi) du même dossier
    BathyDb idb;
    std::string ie;
    if (idb.open(m_set.dbFolder, ie, 0, DbKind::Imagery)) {
      idb.pruneEmptyCells();  // fichiers vides restés d'un retrait (fichier alors ouvert ailleurs)
      m_imgSources = idb.sources();
    }
  }
  BathyDb db;
  std::string err;
  const bool opened = db.open(m_set.dbFolder, err);
  m_dbCells.clear();
  if (opened && !db.needsMigration()) {
    // surfaces de référence : copies dans les .pdb (et reprises depuis un
    // .pdb copié d'un autre dossier) ; rapide quand rien n'a changé
    std::string se;
    db.syncDatum(se);
    db.pruneEmptyCells();  // fichiers vides restés d'un retrait (fichier alors ouvert ailleurs)
  }
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
  if (opened && db.needsMigration() && !m_migrationAsked) {
    m_migrationAsked = true;
    m_events->CallAfter([this]() { DbMigrate(true); });
  }
  if (opened && db.needsRepair())
    StartDbJob(U8("Mise à jour des statistiques des sources").ToStdString(wxConvUTF8),
               [](BathyDb& b, std::string& e, const DbProgress&) { return b.repairStats(e); });
}

void PersonalDepth_pi::StartDbJob(
    const std::string& title,
    std::function<bool(BathyDb&, std::string&, const DbProgress&)> job, DbKind kind) {
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
  m_dbJobKind = kind;
  const std::string folder = m_set.dbFolder;
  const DatumConfig dcfg = DatumCfg();
  const Datum vk = ViewDatum();
  m_dbThread = std::thread([this, folder, job, dcfg, vk, kind]() {
    if (kind == DbKind::Imagery) {  // imagerie : sans référence verticale
      BathyDb db;
      std::string err;
      DbProgress prog = [this](float f, const std::string& step) {
        m_dbProgress = f;
        std::lock_guard<std::mutex> lk(m_dbMx);
        m_dbStep = step;
      };
      bool ok = db.open(folder, err, 0, DbKind::Imagery) && job(db, err, prog);
      if (!ok && err.empty()) err = pd::tr("échec");
      std::string e2;
      db.reloadSources(e2);
      {
        std::lock_guard<std::mutex> lk(m_dbMx);
        m_dbErr = ok ? std::string() : (err == "Annulé" ? std::string(pd::tr("arrêté")) : err);
        m_imgSourcesNew = db.sources();
      }
      m_dbBusy = false;
      return;
    }
    BathyDb db;
    DatumProvider prov(dcfg);  // surfaces de référence calculées au besoin
    db.setDatumProvider(&prov);
    std::string err;
    DbProgress prog = [this](float f, const std::string& step) {
      m_dbProgress = f;
      std::lock_guard<std::mutex> lk(m_dbMx);
      m_dbStep = step;
    };
    bool ok = db.open(folder, err) && job(db, err, prog);
    if (!ok && err.empty()) err = pd::tr("échec");
    std::string e2;
    // référence d'affichage LAT / ellipsoïde : surfaces des nouvelles sources
    if (ok && (vk == Datum::Lat || vk == Datum::Ell) && prov.available(vk) && !db.needsMigration())
      db.ensureDatumForSources(vk, e2, prog, &m_dbCancel);
    e2.clear();
    if (ok && !db.needsMigration()) db.syncDatum(e2);  // copies des surfaces dans les .pdb
    e2.clear();
    db.reloadSources(e2);
    {
      std::lock_guard<std::mutex> lk(m_dbMx);
      m_dbErr = ok ? std::string() : (err == "Annulé" ? std::string(pd::tr("arrêté")) : err);
      m_dbSourcesNew = db.sources();
      m_dbCellsNew = db.cellInfo();
      m_dbCellDegNew = db.cellDeg();
    }
    m_dbBusy = false;
  });
  if (m_dlg) m_dlg->SetDbStatus(DbStatus());
}

// Import par blocs au-delà de 200 Mo de texte (~7 millions de sondes) : la
// version Windows 32 bits n'a que 2 Go d'adresses, un nuage coûte 12 o / sonde.
static constexpr uintmax_t kStreamBytes = 200ull << 20;
static constexpr size_t kStreamBlock = 4u << 20;  // sondes par bloc (~50 Mo)

void PersonalDepth_pi::DbImport(const std::vector<LayerParams>& files, const std::string& group,
                                const std::vector<std::vector<int64_t>>& replace) {
  const DbPriority prio = (DbPriority)m_set.dbPriority;
  const std::atomic<bool>* cancel = &m_dbCancel;
  const LandCfg landCfg = LandConfig();
  {
    std::lock_guard<std::mutex> lk(m_dbMx);
    m_estTracks = LeverEstimate();
  }
  LeverEstimate* est = &m_estTracks;
  std::mutex* estMx = &m_dbMx;
  StartDbJob(JobLabel("Import (%d fichier(s))", files.size()),
             [files, prio, cancel, group, replace, landCfg, est, estMx](BathyDb& db, std::string& err,
                                                                       const DbProgress& prog) {
               std::string errors;
               LandMask land;  // pas de surface interpolée sur la terre
               if (landCfg.any()) {
                 prog(0.f, pd::tr("Trait de côte"));
                 OpenLand(landCfg, land, errors);
                 if (land.isOpen()) db.setLandMask(&land);
                 db.setShoreConstraint(landCfg.shore);
               }
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
                 prog(f0, tag + name + pd::tr(" : lecture"));
                 // très gros fichier texte : lecture et import par blocs (mémoire limitée)
                 std::error_code fec;
                 const auto fsize = std::filesystem::file_size(std::filesystem::u8path(p.path), fec);
                 const bool streamed = BathyLayer::IsStreamable(p) && !fec && fsize > kStreamBytes;
                 BathyLayer L(p);
                 PointCloud sample;  // nuage entier, ou premier bloc (analyse du levé)
                 if (streamed) {
                   std::string se;
                   const bool sok = L.streamXYZ(kStreamBlock, [&](PointCloud& b) {
                     sample = b;
                     return false;  // premier bloc seulement
                   }, se);
                   if (*cancel) return stopped();
                   if (!sok || sample.empty()) {
                     errors += name + " : " + (se.empty() ? std::string(pd::tr("lecture impossible")) : se) + "\n";
                     continue;
                   }
                 } else {
                   L.loadSync();
                   if (*cancel) return stopped();
                   if (L.state() != BathyLayer::Ready || L.cloud.empty()) {
                     errors += name + " : " + (L.error().empty() ? std::string(pd::tr("lecture impossible")) : L.error()) + "\n";
                     continue;
                   }
                 }
                 const PointCloud& cloud = streamed ? sample : L.cloud;
                 // nature du levé (grille / nuage), résolution et distance D
                 prog(f0, tag + name + pd::tr(" : analyse du levé"));
                 const SurveyShape shape = AnalyzeSurvey(cloud, (Instrument)p.instrument, p.nature);
                 const bool gridded = shape.isGrid();
                 DbSource m;
                 m.name = name;
                 m.path = p.path;
                 m.info = shape.describe();
                 m.info += cloud.posInfo;  // traces : décalage sonde / antenne
                 if (p.interpD > 0 && !gridded && p.interpolate) m.info += pd::tr(" ; D imposée");
                 m.interpD = (!gridded && p.interpolate) ? (p.interpD > 0 ? p.interpD : shape.maxEdge) : 0;
                 // méthode : automatique = voisin naturel pour un levé en lignes (pas de
                 // traînées de longs triangles entre les lignes), sinon TIN
                 m.interpMethod = p.interpMethod > 0 ? p.interpMethod - 1
                                  : (int)(shape.lines ? InterpMethod::NaturalNeighbour : InterpMethod::Tin);
                 // pas de la surface : celui de l'analyse (D automatique), jamais
                 // déduit d'une distance max imposée (une grande D rendait la
                 // surface grossière) ; la base garde au moins D / 128 (coût borné)
                 m.res = m.interpD <= 0 ? shape.storeRes : shape.interpRes;
                 if (gridded) { m.gridStepX = shape.dx; m.gridStepY = shape.dy; }
                 if (p.storeRes > 0) m.res = p.storeRes;  // résolution imposée à l'import
                 // date du levé : imposée, sinon première date de la colonne
                 // date du fichier, sinon nom du fichier ou date du fichier
                 m.date = p.surveyDate;
                 // (dates avant 1970 comprises : pas de gmtime, NULL sous Windows)
                 if (m.date.empty() && cloud.tMin != 0) m.date = pd::FormatTime(cloud.tMin).substr(0, 10);
                 if (m.date.empty()) m.date = GuessSurveyDate(p.path);
                 m.params = p.importPrefs;
                 if (p.load.antAuto) {  // décalage sonde / antenne estimé sur ce fichier
                   char b[160];
                   std::snprintf(b, sizeof b, "antEstOk=%d\nantEstFwd=%.9g\nantEstStbd=%.9g\n", cloud.antEstOk ? 1 : 0,
                                 cloud.antEstFwd, cloud.antEstStbd);
                   if (!m.params.empty() && m.params.back() != '\n') m.params += '\n';
                   m.params += b;
                   std::lock_guard<std::mutex> lk(*estMx);
                   est->set = true;
                   est->ok = cloud.antEstOk;
                   est->fwd = cloud.antEstFwd;
                   est->stbd = cloud.antEstStbd;
                 }
                 m.crs = L.crsUsed().empty() ? "WGS84" : L.crsUsed();
                 m.datum = p.datum;
                 m.vref = p.vref.active() ? (p.vref.label.empty() ? "converti" : p.vref.label)
                                          : DatumName((Datum)p.datum);
                 m.group = !p.group.empty() ? p.group : (group.empty() ? GuessSourceGroup(p.path) : group);
                 std::string e;
                 const bool rep = i < replace.size() && !replace[i].empty();
                 auto sub = [&](float f, const std::string& step) {
                   prog(f0 + (f1 - f0) * (rep ? 0.7f * f : f), tag + name + " : " + step);
                 };
                 bool iok;
                 if (streamed) {
                   const double midLat = 0.5 * (sample.latMin + sample.latMax);
                   sample = PointCloud();  // libéré avant la lecture complète
                   BathyDb::CloudFeed feed = [&](const BathyDb::CloudSink& sink, std::string& fe) {
                     BathyLayer L2(p);
                     return L2.streamXYZ(kStreamBlock, [&](PointCloud& b) { return sink(b, L2.progress()); }, fe);
                   };
                   iok = db.importCloudStream(feed, midLat, m, gridded, prio, e, sub, cancel);
                 } else {
                   iok = db.importCloud(L.cloud, m, gridded, prio, e, sub, cancel);
                 }
                 if (!iok) {
                   if (*cancel || e == "Annulé") return stopped();
                   errors += name + " : " + e + "\n";
                   continue;
                 }
                 // réimport : l'ancienne version de ce fichier est retirée de la base
                 if (rep) {
                   prog(f0 + (f1 - f0) * 0.7f, tag + name + U8(" : retrait de l'ancienne version").ToStdString(wxConvUTF8));
                   if (!db.removeSources(replace[i], prio, e)) errors += name + pd::tr(" (ancienne version) : ") + e + "\n";
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

void PersonalDepth_pi::DbEditMeta(const std::vector<int64_t>& ids, const std::string& date,
                                  const std::string& group, const std::string& params) {
  if (ids.empty() || (date.empty() && group.empty() && params.empty())) return;
  const DbPriority prio = (DbPriority)m_set.dbPriority;
  StartDbJob(JobLabel("Modification (%d source(s))", ids.size()),
             [ids, date, group, params, prio](BathyDb& db, std::string& err, const DbProgress& prog) {
               if (!params.empty()) {  // seulement les sources sans réglages enregistrés
                 std::vector<int64_t> unknown;
                 for (int64_t id : ids)
                   if (const DbSource* d = db.source(id))
                     if (d->params.empty()) unknown.push_back(id);
                 if (!unknown.empty() && !db.setParams(unknown, params, err)) return false;
               }
               if (!date.empty() && !db.setDates(ids, date, prio, err, prog)) return false;
               return group.empty() || db.setGroup(ids, group, err);
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
  StartDbJob(JobLabel("Retrait de %d source(s)", ids.size()),
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
  StartDbJob(JobLabel("Nettoyage de %d source(s)", ids.size()),
             [ids, zMin, zMax, prio, cancel](BathyDb& db, std::string& err, const DbProgress& prog) {
               return db.cleanSources(ids, zMin, zMax, prio, err, prog, cancel);
             });
}

void PersonalDepth_pi::DbSetVisible(const std::vector<int64_t>& ids, bool visible) {
  if (ids.empty()) return;
  const DbPriority prio = (DbPriority)m_set.dbPriority;
  StartDbJob(JobLabel(visible ? "Affichage de %d source(s)" : "Masquage de %d source(s)", ids.size()),
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
  StartDbJob(U8("Classe ").ToStdString(wxConvUTF8) + group,
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

// ================================================== référence verticale
float PersonalDepth_pi::SourceDisplayOffset(const DbSource& d) {
  const Datum vk = ViewDatum();
  if (d.datum == (int)Datum::Legacy || d.datum == (int)Datum::Free || !DatumHasSurface(vk)) return 0.f;
  if (m_srcOffsetEpoch != m_dbEpoch || m_srcOffsetDatum != (int)vk) {
    m_srcOffset.clear();
    m_srcOffsetEpoch = m_dbEpoch;
    m_srcOffsetDatum = (int)vk;
    DatumStore st;
    std::string e;
    if (!m_set.dbFolder.empty() && st.open(m_set.dbFolder, e))
      for (const auto& s : m_dbSources) {
        const double v = st.at(vk, 0.5 * (s.lonMin + s.lonMax), 0.5 * (s.latMin + s.latMax));
        m_srcOffset[s.id] = std::isnan(v) ? 0.f : (float)v;
      }
  }
  auto it = m_srcOffset.find(d.id);
  return it == m_srcOffset.end() ? 0.f : it->second;
}

DatumConfig PersonalDepth_pi::DatumCfg() const {
  DatumConfig c;
  const wxString dir = W8(m_set.vrefFranceDir.c_str());
  if (!dir.empty() && wxDirExists(dir)) {  // BathyElli v2.1 : NM/ell et ZH/ell
    wxArrayString nm, zh;
    wxDir::GetAllFiles(dir, &nm, "*NM*.glhi");
    wxDir::GetAllFiles(dir, &zh, "*ZH*.glhi");
    for (auto& f : nm) c.nmFiles.push_back(f.ToUTF8().data());
    for (auto& f : zh) c.zhFiles.push_back(f.ToUTF8().data());
  }
  if (!m_set.tideAtlas.empty() && wxFileExists(W8(m_set.tideAtlas.c_str()))) c.fesAtlas = m_set.tideAtlas;
  if (!m_set.mssGrid.empty() && wxFileExists(W8(m_set.mssGrid.c_str()))) c.mssGrid = m_set.mssGrid;
  // zéros des cartes officiels des autres pays (dossier lu une fois)
  if (m_natDir != m_set.nationalDir) {
    m_natDir = m_set.nationalDir;
    m_natList = ScanNationalDatums(m_natDir);
  }
  c.national = m_natList;
  return c;
}

void PersonalDepth_pi::NationalChanged() {
  m_natDir = "\x01";  // relire le dossier
  SaveConfig();
  ++m_dbEpoch;
  // surfaces du zéro des cartes recalculées, sources corrigées
  if (!m_dbSources.empty() && !DbNeedsMigration()) DbUpdateDatum();
  if (m_renderer) m_renderer->Invalidate();
  if (m_dlg) m_dlg->RefreshDb();
  RequestRefresh(GetOCPNCanvasWindow());
}

bool PersonalDepth_pi::DbNeedsMigration() const {
  for (const auto& d : m_dbSources)
    if (d.datum == (int)Datum::Legacy) return true;
  return false;
}

Datum PersonalDepth_pi::ViewDatum() const {
  // avec la marée : hauteur d'eau = profondeur au zéro des cartes + marée
  return m_set.tideMode != 0 ? Datum::Cd : DisplayDatumOf(m_set.dispDatum);
}

wxString PersonalDepth_pi::DatumShort() const {
  switch (ViewDatum()) {
    case Datum::Lat: return "LAT";
    case Datum::Msl: return U8("NM");
    case Datum::Ell: return U8("ell.");
    default: return U8("ZH");
  }
}

wxString PersonalDepth_pi::DatumInfo() const {
  if (DbNeedsMigration())
    return U8("Base d'une version antérieure : profondeurs au zéro des cartes, à migrer vers le "
              "niveau moyen (bouton « Migrer la base… »).");
  const Datum vk = ViewDatum();
  const DatumConfig c = DatumCfg();
  const std::string natNames = NationalNames(c.national);
  wxString t = U8("Profondeurs affichées : ") + W8(DatumName(vk).c_str());
  if (DatumHasSurface(vk) && m_datumOrigin) {
    std::string model;
    if (vk == Datum::Ell && !c.mssGrid.empty()) {
      GeoGridFile g;
      std::string e;
      if (g.open(c.mssGrid, e)) model = g.model();
    }
    t += " (" + W8(DatumOriginText(vk, m_datumOrigin, model, natNames).c_str()) + ")";
  }
  if (m_set.tideMode != 0) t += U8(", plus la marée");
  // zéro des cartes : officiel ou approché, en clair
  if (vk == Datum::Cd && m_datumOrigin) {
    if (m_datumOrigin & kOrigNone)
      t += "\n" + U8("⚠ Zone sans zéro des cartes connu : profondeurs telles qu'importées.");
    else if (m_datumOrigin & kOrigFes)
      t += "\n" + U8("≈ Zéro approché ici (plus basse mer de la marée FES) : écart possible de quelques "
                      "décimètres avec les cartes officielles. Fichiers des pays : docs/external-data.md.");
    else
      t += "\n" + U8("✓ Zéro officiel du pays, comme sur les cartes marines.");
  }
  auto yes = [](bool b) { return b ? U8("oui") : U8("non"); };
  t += "\n" + U8("Références : BathyElli ") + yes(!c.nmFiles.empty() && !c.zhFiles.empty()) +
       U8(", FES2022 ") + yes(!c.fesAtlas.empty()) + U8(", MSS ") + yes(!c.mssGrid.empty()) +
       U8(", pays ") + (natNames.empty() ? U8("aucun") : W8(natNames.c_str()));
  return t;
}

void PersonalDepth_pi::DatumChanged() {
  SaveConfig();
  ++m_dbEpoch;
  if (m_renderer) m_renderer->Invalidate();
  const Datum vk = ViewDatum();
  // LAT / ellipsoïde : surfaces calculées en tâche de fond sur l'emprise des sources
  if ((vk == Datum::Lat || vk == Datum::Ell) && !m_dbSources.empty() && !DbNeedsMigration()) {
    if (!DatumCfg().fesAtlas.empty() || vk == Datum::Ell)
      StartDbJob(U8("Surfaces de référence : ").ToStdString(wxConvUTF8) + DatumName(vk),
                 [vk](BathyDb& db, std::string& err, const DbProgress& prog) {
                   std::string e;
                   db.ensureDatumForSources(vk, e, prog);  // hors couverture : non décalées
                   (void)err;
                   return true;
                 });
  }
  if (m_dlg) m_dlg->RefreshDb();
  RequestRefresh(GetOCPNCanvasWindow());
}

void PersonalDepth_pi::DbMigrate(bool ask) {
  if (!DbNeedsMigration() || m_dbBusy) return;
  wxFileName bak(W8(m_set.dbFolder.c_str()), "");
  bak.AppendDir("backup_before_MSL");
  wxString dir = bak.GetPath();
  for (int k = 2; wxDirExists(dir) && k < 100; ++k) dir = bak.GetPath() + wxString::Format("_%d", k);
  long long bytes = 0;
  for (const auto& c : m_dbCells) bytes += c.bytes;
  if (ask) {
    const wxString msg = wxString::Format(
        U8("Cette base a été créée par une version antérieure de PersonalDepth : ses profondeurs sont "
           "au zéro des cartes.\n\nLes profondeurs sont désormais rangées sous le niveau moyen, pour "
           "relier toutes les références (zéro des cartes, LAT, ellipsoïde) et afficher chaque source "
           "dans la référence choisie. L'affichage au zéro des cartes reste identique.\n\nMigrer "
           "maintenant ? Les fichiers de la base (%.0f Mo) sont d'abord copiés dans :\n%s\n\nSans "
           "migration, la base reste affichée telle quelle, mais l'import est bloqué."),
        bytes / 1e6, dir);
    if (wxMessageBox(msg, "PersonalDepth", wxYES_NO | wxICON_QUESTION) != wxYES) return;
  }
  const std::string backup = dir.ToUTF8().data();
  const DbPriority prio = (DbPriority)m_set.dbPriority;
  const std::atomic<bool>* cancel = &m_dbCancel;
  StartDbJob(U8("Migration vers le niveau moyen").ToStdString(wxConvUTF8),
             [backup, prio, cancel](BathyDb& db, std::string& err, const DbProgress& prog) {
               return db.migrate(backup, prio, err, prog, cancel);
             });
}

std::string PersonalDepth_pi::LandMaskFile(std::string* name) const {
  std::string file, nm;
  if (!m_set.landMaskOn || !LandMask::Find(m_set.landMask, file, nm)) file.clear(), nm.clear();
  if (name) *name = nm;
  return file;
}

std::vector<std::string> PersonalDepth_pi::ShoreFileList(const std::string& s) {
  std::vector<std::string> v;
  size_t a = 0;
  while (a <= s.size()) {
    size_t b = s.find(';', a);
    if (b == std::string::npos) b = s.size();
    std::string f = s.substr(a, b - a);
    while (!f.empty() && std::isspace((unsigned char)f.back())) f.pop_back();
    size_t k = 0;
    while (k < f.size() && std::isspace((unsigned char)f[k])) ++k;
    if (k < f.size()) v.push_back(f.substr(k));
    a = b + 1;
  }
  return v;
}

PersonalDepth_pi::LandCfg PersonalDepth_pi::LandConfig() const {
  LandCfg c;
  if (!m_set.landMaskOn) return c;
  c.base = LandMaskFile();
  c.custom = ShoreFileList(m_set.shoreFiles);
  c.shore = m_set.shoreZero;
  return c;
}

// fond et traits personnels ; erreurs ajoutées à `errors` (le reste est gardé)
void PersonalDepth_pi::OpenLand(const LandCfg& c, LandMask& m, std::string& errors) {
  std::string le;
  if (!c.base.empty() && !m.open(c.base, le)) errors += le + "\n";
  for (const std::string& f : c.custom) {
    le.clear();
    if (!m.addCustom(f, le)) errors += le + "\n";
  }
}

void PersonalDepth_pi::DbApplyLandMask() {
  if (m_dbBusy || DbNeedsMigration() || m_dbSources.empty()) return;
  const LandCfg landCfg = LandConfig();
  if (!landCfg.any()) return;
  std::vector<int64_t> ids;
  for (const auto& d : m_dbSources) ids.push_back(d.id);
  const DbPriority prio = (DbPriority)m_set.dbPriority;
  const std::atomic<bool>* cancel = &m_dbCancel;
  StartDbJob(U8("Trait de côte : interpolation retirée sur la terre").ToStdString(wxConvUTF8),
             [ids, prio, cancel, landCfg](BathyDb& db, std::string& err, const DbProgress& prog) {
               LandMask land;
               prog(0.f, pd::tr("Trait de côte"));
               std::string errors;
               OpenLand(landCfg, land, errors);
               if (!land.isOpen()) { err = errors; return false; }
               db.setLandMask(&land);
               long long removed = 0;
               if (!db.applyLandMask(ids, prio, err, prog, cancel, &removed)) return false;
               prog(1.f, std::to_string(removed) + U8(" pixel(s) retiré(s)").ToStdString(wxConvUTF8));
               return true;
             });
}

void PersonalDepth_pi::DbUpdateDatum() {
  if (m_dbBusy || DbNeedsMigration()) return;
  const DbPriority prio = (DbPriority)m_set.dbPriority;
  const std::atomic<bool>* cancel = &m_dbCancel;
  StartDbJob(U8("Mise à jour des références verticales").ToStdString(wxConvUTF8),
             [prio, cancel](BathyDb& db, std::string& err, const DbProgress& prog) {
               int changed = 0;
               if (!db.updateDatum(prio, err, prog, cancel, &changed)) return false;
               prog(1.f, std::to_string(changed) + U8(" source(s) corrigée(s)").ToStdString(wxConvUTF8));
               return true;
             });
}

// Vue de la base : reconstruite en tâche de fond quand la carte sort de la
// zone déjà préparée (marge de 50 %) ou quand le niveau de détail change
void PersonalDepth_pi::UpdateView(DbViewSlot& V, DbKind kind, BathyRenderer& R) {
  const bool depth = kind == DbKind::Depth;
  const auto& sources = depth ? m_dbSources : m_imgSources;
  if (m_set.dbFolder.empty() || sources.empty() || (depth ? !BathyNeeded() : !(m_set.shown && m_set.imgShow))) {
    if (V.view || V.next) {
      if (V.next) V.next->cancel();
      R.ClearPending();
      V.next.reset();
      V.view.reset();
      R.Invalidate();
      RequestRefresh(GetOCPNCanvasWindow());
    }
    V.target = -1;  // couche réaffichée : vue recalculée
    return;
  }
  // vues abandonnées : libérées une fois leur calcul arrêté
  V.dying.erase(std::remove_if(V.dying.begin(), V.dying.end(),
                                     [](const LayerPtr& l) { return l->state() != BathyLayer::Loading; }),
                      V.dying.end());
  bool swapNow = V.next && V.next->state() != BathyLayer::Loading;
  if (swapNow && V.next->state() == BathyLayer::Ready && V.view && m_lastRenderGL) {
    // OpenGL : la nouvelle vue est d'abord préparée (textures, isobathes) par
    // petits lots pendant que l'actuelle reste affichée, puis la remplace
    const wxLongLong now = wxGetLocalTimeMillis();
    if (R.Pending() != V.next) {
      R.SetPending(V.next);
      V.pendingSince = now;
      RequestRefresh(GetOCPNCanvasWindow());
      swapNow = false;
    } else if (!R.PendingDone() && (now - V.pendingSince).ToDouble() < 2000) {
      swapNow = false;
    }
  }
  if (swapNow) {
    const auto st = V.next->state();
    const bool first = !V.view;
    V.view = st == BathyLayer::Ready ? V.next : nullptr;
    if (depth && st == BathyLayer::Ready) {
      const double ms = (wxGetLocalTimeMillis() - m_perfViewStart).ToDouble();
      ++m_perfViewsReady;
      m_perfViewMs += ms;
      m_perfViewMaxMs = std::max(m_perfViewMaxMs, ms);
    }
    if (V.result && st == BathyLayer::Ready) {  // zone réellement préparée
      V.quad = V.result->quad;
      for (int k = 0; k < 4; ++k) V.box[k] = V.result->box[k];
    }
    if (depth && V.originNext) m_datumOrigin = V.originNext->load();
    if (depth && m_dlg) m_dlg->RefreshDatum();
    if (R.Pending() == V.next && st == BathyLayer::Ready) {
      R.AdoptPending();  // textures déjà prêtes : pas d'à-coup
    } else {
      R.ClearPending();
      R.Invalidate();
    }
    V.next.reset();
    if (depth) ++m_set.version;
    if (depth && first && V.view && m_set.tideMode && !m_set.tideOk) UpdateTide(true);
    if (m_dlg && m_dlg->IsShown()) m_dlg->RefreshLayers();
    RequestRefresh(GetOCPNCanvasWindow());
  }
  if (!m_vpValid) return;
  if (depth && m_set.tideMode) {  // zone de la marée : sources à l'écran
    std::string sig;
    double zb[4];
    TideZone(zb, &sig);
    if (sig != m_tideZoneSig) {
      m_tideZoneSig = sig;
      UpdateTide(true);
    }
  }
  // niveau : un pixel de la base ≈ un pixel d'écran
  const double ppd = std::max(1e-6, m_lastPpd);
  const int target = std::max(kDbMinLevel, std::min(kDbMaxLevel,
                              (int)std::lround(std::log2(ppd * 360.0 / kTileSize))));
  const double* v = m_vpBox;
  // Écran (carte tournée comprise) encore dans la zone préparée ?
  bool inside;
  if (m_vpQuadOk && V.hasQuad) {
    inside = true;
    for (int k = 0; k < 4 && inside; ++k) inside = V.quad.contains(m_vpQuad.x[k], m_vpQuad.y[k]);
  } else {
    inside = !V.hasQuad && v[0] >= V.box[0] && v[1] <= V.box[1] &&
             v[2] >= V.box[2] && v[3] <= V.box[3];
  }
  const Datum vk = depth ? ViewDatum() : Datum::Free;
  const unsigned epoch = depth ? m_dbEpoch : m_imgEpoch;
  if (inside && target == V.target && V.epoch == epoch && V.datum == (int)vk) return;
  if (m_dbBusy) return;  // la base est en cours de modification
  // Vue en cours de calcul devenue inutile (zoom, déplacement) : arrêtée tout
  // de suite plutôt que d'enchaîner des vues périmées (latence au zoom)
  if (V.next) {
    if (V.stop) *V.stop = true;
    V.next->cancel();
    if (R.Pending() == V.next) R.ClearPending();
    if (depth) ++m_perfViewsCancelled;
    V.dying.push_back(V.next);
    V.next.reset();
  }
  // Marge autour de l'écran pour suivre un déplacement sans recharger. Carte
  // tournée : seules les cellules de l'écran tourné (et de sa marge) sont
  // calculées, pas tout le rectangle qui l'englobe ; la marge est réduite
  // (jusqu'à 10 %) pour que la vue reste au niveau visé dans le budget.
  static constexpr size_t kViewCells = 4000000;
  // Écran + marge m : coins écartés du centre de (1 + 2m)
  using Prep = DbViewPrep;
  auto expand = [](const ViewQuad& sq, double m, Prep& r) {
    double cx = 0, cy = 0;
    for (int k = 0; k < 4; ++k) { cx += 0.25 * sq.x[k]; cy += 0.25 * sq.y[k]; }
    double x0 = 1e300, x1 = -1e300, y0 = 1e300, y1 = -1e300;
    for (int k = 0; k < 4; ++k) {
      r.quad.x[k] = cx + (sq.x[k] - cx) * (1 + 2 * m);
      r.quad.y[k] = cy + (sq.y[k] - cy) * (1 + 2 * m);
      x0 = std::min(x0, r.quad.x[k]); x1 = std::max(x1, r.quad.x[k]);
      y0 = std::min(y0, r.quad.y[k]); y1 = std::max(y1, r.quad.y[k]);
    }
    auto latOf = [](double my) { return std::atan(std::sinh(my * M_PI / 180.0)) * 180.0 / M_PI; };
    r.box[0] = x0; r.box[1] = x1; r.box[2] = latOf(y0); r.box[3] = latOf(y1);
  };
  const bool useQuad = m_vpQuadOk;
  const ViewQuad screen = m_vpQuad;
  Prep prov;  // zone prise en compte pendant le calcul (marge minimale)
  if (useQuad) {
    expand(screen, 0.1, prov);
  } else {
    const double w = v[1] - v[0], h = v[3] - v[2];
    prov.box[0] = v[0] - 0.5 * w; prov.box[1] = v[1] + 0.5 * w;
    prov.box[2] = v[2] - 0.5 * h; prov.box[3] = v[3] + 0.5 * h;
  }
  // zone réellement préparée, connue du calcul (marge selon le niveau des données)
  auto result = std::make_shared<Prep>(prov);
  LayerParams p;
  p.label = U8(depth ? "Base de bathymétrie" : "Base d'imagerie").ToStdString(wxConvUTF8);
  p.useCache = false;
  p.exaggeration = m_set.exaggeration;
  p.shadeSmooth = m_set.shadeSmooth;
  auto stop = std::make_shared<std::atomic<bool>>(false);
  p.stop = stop.get();
  const std::string folder = m_set.dbFolder;
  const std::set<std::string> hidden = m_set.dbHidden;
  const bool skipInterp = depth && !m_set.interpShow;
  auto origin = std::make_shared<std::atomic<int>>(0);
  p.custom = [folder, hidden, kind, target, skipInterp, vk, origin, stop, useQuad, screen, result, expand](
                 PointCloud& pc, double& dl, double& dt, std::vector<int64_t>& src,
                 std::vector<uint8_t>& interp, std::string& err) {
    BathyDb db;
    int used = 0, org = 0;
    if (!db.open(folder, err, 0, kind)) return false;
    db.setHiddenCells(hidden);
    Prep r = *result;
    if (useQuad) {
      // Marge la plus large qui garde la vue au niveau visé, plafonné par le
      // niveau des données (zoom au-delà de leur résolution : marge pleine,
      // moins de rechargements en faisant glisser la carte)
      const int zmax = db.maxMergedLevel(r.box[0], r.box[1], r.box[2], r.box[3]);
      const int eff = std::max(kDbMinLevel, std::min(target, zmax));
      const double d = 360.0 / std::ldexp((double)kTileSize, eff);
      const double cs = screen.area() / (d * d);
      auto marginFor = [](double c) { return 0.5 * (std::sqrt(0.95 * kViewCells / c) - 1.0); };
      double m = marginFor(cs);
      if (m < 0.1) m = marginFor(cs / 4);  // l'écran seul dépasse : un niveau plus bas
      expand(screen, std::max(0.1, std::min(0.5, m)), r);
    }
    const bool ok = db.buildView(r.box[0], r.box[1], r.box[2], r.box[3], target, kViewCells, pc, dl, dt, used,
                                 err, stop.get(), &src, &interp, skipInterp, vk, &org,
                                 useQuad ? &r.quad : nullptr);
    *result = r;  // lu après la fin du calcul seulement
    *origin = org;
    return ok;
  };
  for (int k = 0; k < 4; ++k) V.box[k] = prov.box[k];
  V.quad = prov.quad;
  V.hasQuad = useQuad;
  V.result = result;
  V.stop = stop;
  V.target = target;
  V.epoch = epoch;
  V.datum = (int)vk;
  V.originNext = origin;
  V.next = std::make_shared<BathyLayer>(p);
  V.next->startLoad();
  if (depth) {
    ++m_perfViewsStarted;
    m_perfViewStart = wxGetLocalTimeMillis();
  }
}

void PersonalDepth_pi::UpdateDbView() {
  UpdateView(m_vd, DbKind::Depth, *m_renderer);
  UpdateView(m_vi, DbKind::Imagery, *m_imgRenderer);
}

void PersonalDepth_pi::ResetView(DbViewSlot& V, BathyRenderer* R) {
  if (V.stop) *V.stop = true;
  if (V.next) V.next->cancel();
  if (R) R->ClearPending();
  V.next.reset();
  V.dying.clear();
  V.view.reset();
}

// ======================================================= palette automatique
void PersonalDepth_pi::AutoPaletteChanged() {
  SaveConfig();
  UpdateAutoPalette(true);
}

void PersonalDepth_pi::SetAutoPalette(bool on) {
  DisplaySettings& s = m_set;
  if (s.autoPalette == on) return;
  // couleurs de référence : celles de la palette affichée (préréglage ou seuils
  // modifiés en manuel depuis le dernier passage en automatique)
  if (on && (s.autoBase.empty() || !s.autoBaseFresh)) s.autoBase = s.ramp.serialize();
  s.autoBaseFresh = true;
  s.autoPalette = on;
  AutoPaletteChanged();
  ColorsChanged();
  if (m_dlg) m_dlg->RefreshPalette();
}

void PersonalDepth_pi::ApplyPreset(const std::string& name) {
  DisplaySettings& s = m_set;
  ColorRamp& r = s.ramp;
  ColorRamp p;
  if (name.rfind("file:", 0) == 0) {  // palette du dossier de l'utilisateur
    std::string err;
    if (!p.loadCPT(name.substr(5), err)) {
      wxMessageBox(U8(err.c_str()), "PersonalDepth", wxOK | wxICON_WARNING);
      return;
    }
  } else {
    p = ColorRamp::Preset(name);
  }
  if (name == ColorRamp::kEncName) {  // isobathes des cartes vectorielles d'OpenCPN
    float sh, sa, de;
    EncContours(sh, sa, de);
    p = ColorRamp::EncPreset(sh, sa, de);
    if (s.autoPalette) {  // paliers fixés par OpenCPN : mode automatique coupé
      s.autoPalette = false;
      AutoPaletteChanged();
    }
  }
  s.autoBase = p.serialize();  // couleurs reprises au passage en automatique
  s.autoBaseFresh = true;
  if (s.autoPalette) {  // automatique : nouvelles couleurs, mêmes seuils calculés
    r.mode = p.mode;
    AutoPaletteChanged();
  } else {
    r.stops = p.stops;
    r.mode = p.mode;
    if (p.safetyOn) { r.safetyOn = true; r.safetyDepth = p.safetyDepth; r.safetyColor = p.safetyColor; }
  }
  ColorsChanged();
  if (m_dlg) { m_dlg->SelectPreset(name); m_dlg->RefreshPalette(); }
}

// ------------------------------------------------------ bandeau de palette
bool PersonalDepth_pi::MouseEventHook(wxMouseEvent& e) {
  if (!m_set.banner || !m_renderer) return false;
  const BannerHit& h = m_renderer->Banner();
  if (!h.valid) return false;
  const bool button = e.ButtonDown() || e.ButtonUp() || e.ButtonDClick() ||
                      e.GetEventType() == wxEVT_MOUSEWHEEL;
  const bool pressed = m_bannerDown != BannerHit::NoPart || m_bannerDrag;
  if (!button && !(pressed && (e.Dragging() || e.Moving()))) return false;
  if (DrawLayers().empty() && ImgLayers().empty()) { m_renderer->HideBanner(); return false; }  // rien d'affiché
  // coordonnées souris (points) -> pixels du point de vue (écrans haute densité)
  wxWindow* win = wxDynamicCast(e.GetEventObject(), wxWindow);
  double k = 1.0;
  if (win && win->GetClientSize().x > 0 && h.pixW > 0) {
    k = (double)h.pixW / win->GetClientSize().x;
    if (std::fabs(k - 1.0) < 0.05) k = 1.0;
  }
  const wxPoint p((int)std::lround(e.GetX() * k), (int)std::lround(e.GetY() * k));

  // Déplacement du bandeau : appui dessus puis glisser (sauf position verrouillée)
  if (pressed && (e.Dragging() || e.Moving() || e.LeftUp())) {
    if (!m_bannerDrag && !e.LeftIsDown() && !e.LeftUp()) {  // bouton relâché hors du canevas
      m_bannerDown = BannerHit::NoPart;
      return false;
    }
    if (!m_bannerDrag && e.Dragging() && !m_set.bannerLock &&
        (std::abs(p.x - m_bannerDownPt.x) > 4 || std::abs(p.y - m_bannerDownPt.y) > 4))
      m_bannerDrag = true;
    if (m_bannerDrag) {
      const int W = h.pixW > 0 ? h.pixW : 1, H = std::max(1, h.pixH);
      const double cx = p.x - m_bannerGrab.x + h.box.width / 2.0;
      const double cy = p.y - m_bannerGrab.y + h.box.height / 2.0;
      m_set.bannerFx = (float)std::min(1.0, std::max(0.0, cx / W));
      m_set.bannerFy = (float)std::min(1.0, std::max(0.0, cy / H));
      m_set.bannerPos = 3;
      if (e.LeftUp() || !e.LeftIsDown()) {  // fin du déplacement
        m_bannerDrag = false;
        m_bannerDown = BannerHit::NoPart;
        SaveConfig();
        if (m_dlg) m_dlg->RefreshPalette();
      }
      RequestRefresh(GetOCPNCanvasWindow());
      return true;
    }
    if (!e.LeftUp()) return true;  // bouton tenu sur le bandeau : la carte ne bouge pas
  }
  const int part = h.PartAt(p);
  if (part == BannerHit::NoPart) {
    m_bannerDown = BannerHit::NoPart;
    return false;
  }
  if (e.LeftDown()) {
    m_bannerDownPt = p;
    m_bannerGrab = wxPoint(p.x - h.box.x, p.y - h.box.y);
  }
  // Action au relâchement du bouton (comme un bouton classique) : sous Windows,
  // OpenCPN capture la souris à l'appui ; ouvrir un menu ou une fenêtre à ce
  // moment-là laissait la carte « bouton enfoncé » (elle suivait la souris).
  if (e.LeftDown()) m_bannerDown = part;
  if (e.LeftUp()) {
    const bool same = (m_bannerDown == part);
    m_bannerDown = BannerHit::NoPart;
    if (same) {
      const wxPoint pos = e.GetPosition();
      if (m_events) m_events->CallAfter([this, part, win, pos] { BannerClick(part, win, pos); });
    }
  }
  return true;  // le clic sur le bandeau ne déplace pas la carte
}

void PersonalDepth_pi::BannerClick(int part, wxWindow* win, wxPoint pos) {
  if (!win) win = GetOCPNCanvasWindow();
  if (win && win->HasCapture()) win->ReleaseMouse();  // par sécurité
  switch (part) {
    case BannerHit::Mode:
      SetAutoPalette(!m_set.autoPalette);
      break;
    case BannerHit::Orient:  // rampe horizontale / verticale
      m_set.bannerVertical = !m_set.bannerVertical;
      SaveConfig();
      RequestRefresh(GetOCPNCanvasWindow());
      break;
    case BannerHit::Source:
      m_set.autoSource = m_set.autoSource == 1 ? 0 : 1;
      AutoPaletteChanged();
      ColorsChanged();
      if (m_dlg) m_dlg->RefreshPalette();
      break;
    case BannerHit::Min:
    case BannerHit::Max:
      BannerRange(part == BannerHit::Min, win);
      break;
    case BannerHit::Bar: {
      wxMenu menu;
      const auto list = PaletteList();
      for (size_t k = 0; k < list.size(); ++k) {
        if (k == 1) menu.AppendSeparator();  // ENC (S-52), puis palettes du dossier
        menu.Append(10000 + (int)k, list[k].label);
      }
      const int id = win->GetPopupMenuSelectionFromUser(menu, pos);
      if (id >= 10000 && id < 10000 + (int)list.size()) ApplyPreset(list[id - 10000].key);
      break;
    }
    // ---- imagerie
    case BannerHit::ImgMode:
      m_set.imgAuto = !m_set.imgAuto;
      UpdateImgAuto(true);
      ImageryChanged();
      break;
    case BannerHit::ImgSource:
      m_set.imgAutoSource = m_set.imgAutoSource == 1 ? 0 : 1;
      UpdateImgAuto(true);
      ImageryChanged();
      break;
    case BannerHit::ImgMin:
    case BannerHit::ImgMax: {
      const bool isMin = part == BannerHit::ImgMin;
      const int cur = isMin ? m_set.imgMin : m_set.imgMax;
      const wxString def = wxString::Format("%d", cur);
      wxString txt = wxGetTextFromUser(isMin ? U8("Imagerie : noir à (en pour cent de l'étirement de chaque image, 0 – 99)")
                                             : U8("Imagerie : blanc à (en pour cent de l'étirement de chaque image, 1 – 100)"),
                                       U8("Imagerie"), def, win);
      txt.Trim(true).Trim(false);
      long v;
      if (txt.empty() || txt.IsSameAs(def) || !txt.ToLong(&v)) break;
      if (isMin) m_set.imgMin = (int)std::max(0L, std::min(99L, v));
      else m_set.imgMax = (int)std::max(1L, std::min(100L, v));
      if (m_set.imgMax <= m_set.imgMin) {  // bornes croisées : l'autre suit
        if (isMin) m_set.imgMax = std::min(100, m_set.imgMin + 1);
        else m_set.imgMin = std::max(0, m_set.imgMax - 1);
      }
      m_set.imgAuto = false;  // borne saisie : contraste manuel
      ImageryChanged();
      break;
    }
    case BannerHit::ImgBar: {
      wxMenu menu;
      const char* pal[] = {"Niveaux de gris", "Cuivre", "Sépia"};
      for (int k = 0; k < 3; ++k) menu.AppendRadioItem(10000 + k, U8(pal[k]))->Check(m_set.imgPalette == k);
      menu.AppendSeparator();
      menu.AppendCheckItem(10010, U8("Inverser (réflectivité forte en sombre)"))->Check(m_set.imgInvert);
      const int id = win->GetPopupMenuSelectionFromUser(menu, pos);
      if (id >= 10000 && id < 10003) m_set.imgPalette = id - 10000;
      else if (id == 10010) m_set.imgInvert = !m_set.imgInvert;
      else break;
      ImageryChanged();
      break;
    }
    case BannerHit::ChartMenu:  // isobathes et sondes
      BannerChartMenu(win, pos);
      break;
    case BannerHit::Gear:
      ShowPanel();
      break;
    default:
      break;
  }
}

// Isobathes / sondes changées depuis le bandeau
void PersonalDepth_pi::ChartTextChanged(bool contours) {
  if (contours) ++m_set.contourVersion;  // isobathes recalculées
  ViewChanged();
  if (m_dlg) m_dlg->RefreshPalette();
}

// Menu de l'icône isobathes du bandeau : affichage des isobathes, seuils de
// la palette ou intervalle régulier (valeurs rondes dans l'unité affichée, ou
// saisie), affichage des sondes ; les autres réglages restent dans le panneau
void PersonalDepth_pi::BannerChartMenu(wxWindow* win, wxPoint pos) {
  DisplaySettings& s = m_set;
  const float k = s.depthK > 0 ? s.depthK : 1.f;
  const std::vector<double> vals = k > 1.5f ? std::vector<double>{1, 2, 5, 10, 20, 50, 100}
                                            : std::vector<double>{0.5, 1, 2, 5, 10, 20, 50};
  const double cur = s.contourStep * k;
  enum { kOn = 9980, kPal, kOther, kSound, kPanel, kVal = 10000 };
  wxMenu menu;
  menu.AppendCheckItem(kOn, U8("Isobathes"))->Check(s.contours);
  menu.AppendSeparator();
  menu.AppendRadioItem(kPal, U8("Seuils de la palette"))->Check(s.contourMode == 1);
  bool found = false;
  for (size_t i = 0; i < vals.size(); ++i) {
    const bool on = s.contourMode == 0 && std::fabs(cur - vals[i]) < 1e-3;
    found = found || on;
    menu.AppendRadioItem(kVal + (int)i, wxString::Format(U8("Intervalle %g %s"), vals[i], s.depthUnit))->Check(on);
  }
  wxString other = U8("Autre intervalle…");
  if (s.contourMode == 0 && !found) other = wxString::Format(U8("Autre intervalle (%g %s)…"),
                                                             std::round(cur * 100.0) / 100.0, s.depthUnit);
  menu.AppendRadioItem(kOther, other)->Check(s.contourMode == 0 && !found);
  menu.AppendSeparator();
  menu.AppendCheckItem(kSound, U8("Sondes"))->Check(s.soundings);
  menu.AppendSeparator();
  menu.Append(kPanel, U8("Réglages des isobathes et des sondes…"));
  const int id = win->GetPopupMenuSelectionFromUser(menu, pos);
  if (id == kOn) {
    s.contours = !s.contours;
    ChartTextChanged(true);
  } else if (id == kSound) {
    s.soundings = !s.soundings;
    ChartTextChanged(false);
  } else if (id == kPal) {
    s.contourMode = 1;
    s.contours = true;
    ChartTextChanged(true);
  } else if (id >= kVal && id < kVal + (int)vals.size()) {
    s.contourStep = (float)(vals[id - kVal] / k);
    s.contourMode = 0;
    s.contours = true;
    ChartTextChanged(true);
  } else if (id == kOther) {
    const wxString def = wxString::Format("%g", std::round(cur * 100.0) / 100.0);
    wxString txt = wxGetTextFromUser(U8("Intervalle des isobathes") + " (" + s.depthUnit + ")", U8("Isobathes"), def, win);
    txt.Trim(true).Trim(false);
    txt.Replace(",", ".");
    double v = 0;
    if (txt.empty() || !txt.ToCDouble(&v) || !(v > 0)) return;
    s.contourStep = (float)std::max(0.01, v / k);
    s.contourMode = 0;
    s.contours = true;
    ChartTextChanged(true);
  } else if (id == kPanel) {
    ShowPanel(2);  // onglet Affichage
  }
}

// Saisie d'une borne de la palette. Automatique : borne imposée (vide = calculée) ;
// manuel : les seuils sont étirés linéairement sur la nouvelle plage.
void PersonalDepth_pi::BannerRange(bool isMin, wxWindow* win) {
  DisplaySettings& s = m_set;
  auto& st = s.ramp.stops;
  if (st.empty()) return;
  const float k = s.depthK > 0 ? s.depthK : 1.f;
  const float lo = st.front().depth, hi = st.back().depth;
  const float cur = isMin ? lo : hi;
  wxString msg = isMin ? U8("Profondeur minimale de la palette") : U8("Profondeur maximale de la palette");
  msg += " (" + s.depthUnit + ")";
  if (s.autoPalette) msg += U8("\n(vide = valeur calculée d'après les données)");
  const wxString def = wxString::Format("%g", std::round(cur * k * 100.0) / 100.0);
  wxString txt = wxGetTextFromUser(msg, U8("Palette"), def, win);
  if (txt.IsSameAs(def)) return;
  txt.Trim(true).Trim(false);
  txt.Replace(",", ".");
  if (txt.empty()) {
    if (!s.autoPalette) return;  // annulé
    (isMin ? s.autoMinOn : s.autoMaxOn) = false;
    AutoPaletteChanged();
    ColorsChanged();
    if (m_dlg) m_dlg->RefreshPalette();
    return;
  }
  double v;
  if (!txt.ToCDouble(&v)) {
    wxMessageBox(U8("Valeur numérique attendue."), U8("Palette"), wxOK | wxICON_WARNING, win);
    return;
  }
  const float m = (float)(v / k);
  const float nlo = isMin ? m : lo, nhi = isMin ? hi : m;
  if (!(nhi > nlo)) {
    wxMessageBox(U8("La profondeur minimale doit être inférieure à la maximale."), U8("Palette"),
                 wxOK | wxICON_WARNING, win);
    return;
  }
  if (s.autoPalette) {
    if (isMin) { s.autoMinOn = true; s.autoMin = m; }
    else { s.autoMaxOn = true; s.autoMax = m; }
    AutoPaletteChanged();
  } else if (hi > lo) {
    for (auto& c : st) c.depth = std::round((nlo + (c.depth - lo) * (nhi - nlo) / (hi - lo)) * 100.f) / 100.f;
  } else {
    st.front().depth = nlo;
    if (st.size() > 1) st.back().depth = nhi;
  }
  ColorsChanged();
  if (m_dlg) m_dlg->RefreshPalette();
}

// Seuils recalculés d'après les profondeurs du moment (sonde ZH + marée) :
// toutes les données (base + couches) ou la seule zone affichée
wxString PersonalDepth_pi::AutoPaletteInfo() const {
  const DisplaySettings& s = m_set;
  if (!s.autoPalette || !m_autoSum.ok) return wxString();
  const float k = s.depthK > 0 ? s.depthK : 1.f;
  return wxString::Format(U8("Automatique : %d seuils de %.1f à %.1f %s (%s%s)"), m_autoSum.n, m_autoSum.lo * k,
                          m_autoSum.hi * k, s.depthUnit,
                          s.autoSource == 1 ? U8("zone affichée") : U8("toutes les données"),
                          s.autoMinOn || s.autoMaxOn ? U8(", bornes imposées") : wxString());
}

void PersonalDepth_pi::UpdateAutoPalette(bool force) {
  DisplaySettings& s = m_set;
  if (!s.autoPalette) { m_autoSum = AutoSummary(); m_autoKey.clear(); return; }
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
      if (d.visible) {
        const float off = SourceDisplayOffset(d);  // rangées sous le NM -> référence affichée
        lo = std::min(lo, d.zmin - off);
        hi = std::max(hi, d.zmax - off);
      }
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
  std::snprintf(key, sizeof key, "%.1f %.1f %.1f %d %d %d %zu %.4f", lo, hi, dataLo, s.autoCount,
                s.autoNice ? 1 : 0, s.autoSource, std::hash<std::string>()(s.autoBase), s.depthK);
  if (!force && key == m_autoKey) return;
  m_autoKey = key;
  ColorRamp base;
  if (s.autoBase.empty() || !base.deserialize(s.autoBase)) base = ColorRamp::Preset("TimeZero PBG (bleus)");
  // seuils ronds dans l'unité affichée (m ou ft), ramenés en mètres
  const float k = s.depthK > 0 ? s.depthK : 1.f;
  ColorRamp r = AutoRamp(base, s.ramp, lo * k, hi * k, s.autoCount, s.autoNice);
  for (auto& st : r.stops) st.depth /= k;
  if (r.safetyOn) r.safetyDepth = s.ramp.safetyDepth;
  // estran sous la borne minimale : couleur « terre » de la palette de référence
  // (celle la plus proche de 0), si elle en a une
  const ColorStop* land = nullptr;
  for (const auto& c : base.stops)
    if (c.depth < 0 && (!land || c.depth > land->depth)) land = &c;
  if (land && !r.stops.empty() && r.stops.front().depth >= 0 && r.stops.front().depth <= 0.5f &&
      dataLo < r.stops.front().depth) {
    r.stops.insert(r.stops.begin(), ColorStop{r.stops.front().depth - 0.01f, land->rgba});
  }
  m_autoSum.ok = true;
  m_autoSum.n = (int)r.stops.size();
  m_autoSum.lo = lo;
  m_autoSum.hi = hi;
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

// ================================================================ imagerie
std::vector<LayerPtr> PersonalDepth_pi::ImgLayers() const {
  std::vector<LayerPtr> v;
  if (m_set.shown && m_set.imgShow && m_vi.view && m_vi.view->state() == BathyLayer::Ready) v.push_back(m_vi.view);
  return v;
}

// Palette de l'imagerie : valeur étirée (0 – 1 entre les 2 % et 98 % de chaque
// source) -> niveau de gris (ou teinte) ; bornes du contraste, gamma, inversion
ColorRamp ImageryRamp(const DisplaySettings& s) {
  ColorRamp r;
  r.mode = ColorRamp::Mode::Continuous;
  float lo = std::max(0, std::min(99, s.imgMin)) / 100.f;
  float hi = std::max(1, std::min(100, s.imgMax)) / 100.f;
  if (hi < lo + 0.01f) hi = lo + 0.01f;
  const float g = std::max(0.1f, std::min(5.f, s.imgGamma));
  const int n = 32;
  for (int k = 0; k <= n; ++k) {
    const float t = (float)k / n;
    float v = std::pow(t, 1.f / g);
    if (s.imgInvert) v = 1.f - v;
    float R, G, B;
    switch (s.imgPalette) {
      case 1:  // cuivre (rendu « sonar » des mosaïques)
        R = std::min(1.f, v * 1.25f); G = v * 0.78f; B = v * 0.5f;
        break;
      case 2: {  // sépia
        R = 0.08f + 0.92f * v; G = 0.06f + 0.88f * v; B = 0.04f + 0.74f * v;
        break;
      }
      default:
        R = G = B = v;
    }
    r.stops.push_back({lo + (hi - lo) * t, RGBA((int)std::lround(R * 255), (int)std::lround(G * 255),
                                                (int)std::lround(B * 255))});
  }
  return r;
}

const DisplaySettings& PersonalDepth_pi::ImgSettings() {
  if (m_imgSetVersion == m_set.imgVersion) return m_imgSet;
  DisplaySettings& d = m_imgSet;
  d.ramp = ImageryRamp(m_set);
  d.opacity = m_set.imgOpacity;
  d.hillshade = false;
  d.soundings = false;
  d.contours = false;
  d.banner = false;
  d.legend = false;
  d.smooth = m_set.imgSmooth;
  d.interpShow = true;
  d.interpDistinct = false;
  d.tideMode = 0;
  d.tide.reset();
  d.tideOk = false;
  d.version = m_set.imgVersion;
  m_imgSetVersion = m_set.imgVersion;
  return d;
}

wxString PersonalDepth_pi::ImgCursorText() {
  const auto img = ImgLayers();
  if (img.empty()) return wxString();
  const double radiusDeg = 2.0 / std::max(m_lastPpd, 1e-3);
  float gz, sz;
  double dist;
  int64_t sid = 0;
  bool gi = false;
  if (!img[0]->probe(m_curLon, m_curLat, radiusDeg, gz, sz, dist, &sid, &gi)) return wxString();
  const float n = !std::isnan(gz) ? gz : sz;
  if (std::isnan(n)) return wxString();
  for (const auto& d : m_imgSources)
    if (d.id == sid) {
      const double v = d.vlo + n * (d.vhi - d.vlo);  // valeur d'origine
      wxString t = d.vunit == "dB" ? wxString::Format("%.1f dB", v)
                   : d.vunit.empty() ? wxString::Format("%.4g", v)
                                     : wxString::Format("%.0f", v) + " " + U8(d.vunit.c_str());
      wxString nm = wxString::FromUTF8(d.name.c_str());
      if (nm.size() > 24) nm = nm.Left(22) + "..";
      return U8("Imagerie") + " " + t + "  [" + nm + "]";
    }
  return U8("Imagerie") + wxString::Format(" %.0f %%", n * 100.f);
}

void PersonalDepth_pi::ImageryChanged() {
  ++m_set.imgVersion;
  SaveConfig();
  if (m_dlg) m_dlg->RefreshImageryDisplay();
  RequestRefresh(GetOCPNCanvasWindow());
}

// Contraste automatique de l'imagerie : noir et blanc aux valeurs à 2 % et
// 98 % de la zone affichée, ou étirement d'import de chaque image (0 – 100 %)
void PersonalDepth_pi::UpdateImgAuto(bool force) {
  if (!m_set.imgAuto) return;
  int lo = 0, hi = 100;
  if (m_set.imgAutoSource == 1) {
    const auto img = ImgLayers();
    if (img.empty() || !m_vpValid) return;
    const BathyLayer& l = *img[0];
    const GridLevel& g = l.grid.levels[l.grid.levelFor(m_lastPpd, 2.0)];
    const double* v = m_vpBox;
    const int i0 = std::max(0, (int)std::floor((v[0] - g.lonW) / g.dlon));
    const int i1 = std::min(g.nx - 1, (int)std::floor((v[1] - g.lonW) / g.dlon));
    const int j0 = std::max(0, (int)std::floor((g.latN - v[3]) / g.dlat));
    const int j1 = std::min(g.ny - 1, (int)std::floor((g.latN - v[2]) / g.dlat));
    std::vector<long long> hist(201, 0);  // 0 – 100 % par demi-pour-cent
    long long n = 0;
    for (int j = j0; j <= j1; ++j)
      for (int i = i0; i <= i1; ++i) {
        const float z = g.at(i, j);
        if (std::isnan(z)) continue;
        ++hist[(size_t)std::max(0, std::min(200, (int)std::lround(z * 200.f)))];
        ++n;
      }
    if (n < 100) return;
    long long acc = 0;
    bool loSet = false;
    for (int k = 0; k <= 200; ++k) {
      acc += hist[k];
      if (!loSet && acc > n * 0.02) { lo = k / 2; loSet = true; }
      if (acc >= n * 0.98) { hi = (k + 1) / 2; break; }
    }
    lo = std::max(0, std::min(99, lo));
    hi = std::max(lo + 1, std::min(100, hi));
  }
  if (!force && lo == m_set.imgMin && hi == m_set.imgMax) return;
  if (lo == m_set.imgMin && hi == m_set.imgMax) return;
  m_set.imgMin = lo;
  m_set.imgMax = hi;
  ++m_set.imgVersion;
  if (m_dlg) m_dlg->RefreshImageryDisplay();
  RequestRefresh(GetOCPNCanvasWindow());
}

void PersonalDepth_pi::LayersShownChanged() {
  SaveConfig();
  if (m_renderer) m_renderer->Invalidate();
  if (m_imgRenderer) m_imgRenderer->Invalidate();
  UpdateDbView();
  if (m_dlg && m_dlg->IsShown()) m_dlg->RefreshLayers();
  RequestRefresh(GetOCPNCanvasWindow());
}

// Réimport des images sélectionnées avec les réglages d'import actuels
// (pixel, égalisation, décalage du sonar, fond) : la nouvelle image remplace
// l'ancienne, qui garde sa date et sa classe ; l'ancienne reste si l'import échoue
bool PersonalDepth_pi::ImgReimport(const std::vector<int64_t>& ids, std::string& missing) {
  std::vector<std::string> files;
  std::vector<ImgReplace> rep;
  for (const auto& d : m_imgSources) {
    if (std::find(ids.begin(), ids.end(), d.id) == ids.end()) continue;
    std::error_code ec;
    if (d.path.empty() || !std::filesystem::exists(std::filesystem::u8path(d.path), ec)) {
      missing += (missing.empty() ? "" : "\n") + (d.path.empty() ? d.name : d.path);
      continue;
    }
    files.push_back(d.path);
    rep.push_back({d.id, d.date, d.group});
  }
  if (files.empty()) return false;
  ImgImport(files, std::string(), rep);
  return true;
}

void PersonalDepth_pi::ImgImport(const std::vector<std::string>& files, const std::string& group,
                                 const std::vector<ImgReplace>& replace) {
  if (files.empty()) return;
  ImageryOptions o;
  if (m_set.imgBgMode == 1) o.noBackground = true;
  else if (m_set.imgBgMode == 2) o.background = m_set.imgBgValue;
  SideScanOptions so;
  so.pixel = m_set.imgSonarPixel;
  so.equalize = m_set.imgSonarEqualize;
  so.autoOffset = m_set.imgSonarOffsetAuto;
  so.offsetFwd = m_set.imgSonarOffsetFwd;
  so.offsetStbd = m_set.imgSonarOffsetStbd;
  const std::atomic<bool>* cancel = &m_dbCancel;
  {
    std::lock_guard<std::mutex> lk(m_dbMx);
    m_estSonar = LeverEstimate();
  }
  LeverEstimate* est = &m_estSonar;
  std::mutex* estMx = &m_dbMx;
  StartDbJob(JobLabel(replace.empty() ? "Import d'imagerie (%d fichier(s))" : "Réimport d'imagerie (%d fichier(s))",
                      files.size()),
             [files, o, so, group, replace, cancel, est, estMx](BathyDb& db, std::string& err,
                                                                const DbProgress& prog) {
               std::string errors;
               size_t done = 0;
               for (size_t i = 0; i < files.size(); ++i) {
                 if (*cancel) { err = pd::tr("arrêté"); return false; }
                 const std::string name = std::filesystem::u8path(files[i]).filename().u8string();
                 const float f0 = (float)i / files.size(), f1 = (float)(i + 1) / files.size();
                 const std::string tag = "[" + std::to_string(i + 1) + "/" + std::to_string(files.size()) + "] ";
                 DbSource m;
                 m.name = name;
                 m.path = files[i];
                 m.date = GuessSurveyDate(files[i]);
                 m.group = !group.empty() ? group : GuessSourceGroup(files[i]);
                 const bool re = i < replace.size() && replace[i].id > 0;
                 if (re) {  // réimport : date et classe de l'image remplacée
                   if (!replace[i].date.empty()) m.date = replace[i].date;
                   if (!replace[i].group.empty()) m.group = replace[i].group;
                 }
                 std::string e;
                 auto sub = [&](float f, const std::string& step) { prog(f0 + (f1 - f0) * f, tag + name + " : " + step); };
                 // journal de sondeur (sonar latéral) ou image GeoTIFF
                 const bool sonar = IsSonarLogFile(files[i]);
                 SideScanInfo info;
                 const bool ok = sonar ? ImportSideScan(db, files[i], so, m, DbPriority::ResolutionDate, e, sub, cancel,
                                                        nullptr, &info)
                                       : ImportImagery(db, files[i], o, m, DbPriority::ResolutionDate, e, sub, cancel);
                 if (ok && sonar && so.autoOffset) {  // décalage estimé sur ce journal
                   std::lock_guard<std::mutex> lk(*estMx);
                   est->set = true;
                   est->ok = info.offsetAuto;
                   est->fwd = info.offsetFwd;
                   est->stbd = info.offsetStbd;
                 }
                 if (!ok) {
                   if (*cancel || e == "Annulé") { err = pd::tr("arrêté"); return false; }
                   errors += name + " : " + e + "\n";
                   continue;
                 }
                 if (re) {  // nouvelle image en place : l'ancienne est retirée
                   std::string e2;
                   if (!db.removeSources({replace[i].id}, DbPriority::ResolutionDate, e2, nullptr))
                     errors += name + " : " + e2 + "\n";
                 }
                 ++done;
               }
               if (!errors.empty()) {
                 err = std::to_string(done) + "/" + std::to_string(files.size()) +
                       U8(" importé(s)\n").ToStdString(wxConvUTF8) + errors;
                 return false;
               }
               return true;
             },
             DbKind::Imagery);
}

void PersonalDepth_pi::ImgRemove(const std::vector<int64_t>& ids) {
  if (ids.empty()) return;
  StartDbJob(JobLabel("Retrait de %d image(s)", ids.size()),
             [ids](BathyDb& db, std::string& err, const DbProgress& prog) {
               return db.removeSources(ids, DbPriority::ResolutionDate, err, prog);
             },
             DbKind::Imagery);
}

void PersonalDepth_pi::ImgSetVisible(const std::vector<int64_t>& ids, bool visible) {
  if (ids.empty()) return;
  StartDbJob(JobLabel(visible ? "Affichage de %d image(s)" : "Masquage de %d image(s)", ids.size()),
             [ids, visible](BathyDb& db, std::string& err, const DbProgress& prog) {
               return db.setVisible(ids, visible, DbPriority::ResolutionDate, err, prog);
             },
             DbKind::Imagery);
}

void PersonalDepth_pi::ImgZoomTo(const std::vector<int64_t>& ids) {
  double lo0 = 1e9, lo1 = -1e9, la0 = 1e9, la1 = -1e9;
  for (const auto& d : m_imgSources)
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

// ------------------------------------------------- réglages d'import d'une source
static std::string EscPref(const std::string& v) {
  std::string o;
  for (char c : v) {
    if (c == '\\') o += "\\\\";
    else if (c == '\n') o += "\\n";
    else o += c;
  }
  return o;
}

std::string SerializeImportPrefs(const DisplaySettings& s) {
  std::ostringstream o;
  auto kv = [&](const char* k, const std::string& v) { o << k << '=' << EscPref(v) << '\n'; };
  auto num = [&](const char* k, double v) {
    char b[48];
    std::snprintf(b, sizeof b, "%.9g", v);
    kv(k, b);
  };
  kv("crsCode", s.crsCode);
  num("crsZone", s.crsZone);
  num("crsSouth", s.crsSouth);
  kv("crsCustom", s.crsCustom);
  num("readPrj", s.readPrj);
  num("vref2", s.vrefMode);
  kv("vrefFranceDir", s.vrefFranceDir);
  kv("vrefZh", s.vrefZh);
  kv("vrefGeoid", s.vrefGeoid);
  kv("vrefDirect", s.vrefDirect);
  num("vrefInvert", s.vrefInvert);
  num("order", s.importOrder);
  num("sign", s.importSign);
  num("offset", s.importOffset);
  num("antAuto", s.importAntAuto);
  num("antFwd", s.importAntFwd);
  num("antStbd", s.importAntStbd);
  num("antEstOk", s.importAntEstOk);
  num("antEstFwd", s.importAntEstFwd);
  num("antEstStbd", s.importAntEstStbd);
  num("zFeet", s.importZFeet);
  num("res", s.importRes);
  num("instr", s.importInstrument);
  num("nature", s.importNature);
  num("interp", s.importInterp);
  num("dmax", s.importInterpD);
  num("imode", s.importInterpMethod);
  kv("fmt", s.importFormat);
  num("waterOn", s.importWaterOn);
  kv("water", s.importWater);
  num("waterSign", s.importWaterSign);
  kv("waterFmt", s.importWaterFormat);
  num("waterFeet", s.importWaterFeet);
  num("cache", s.importCache);
  num("nodataOn", s.importNodataOn);
  kv("nodata", s.importNodata);
  num("boundsOn", s.importBoundsOn);
  num("boundMin", s.boundMin);
  num("boundMax", s.boundMax);
  return o.str();
}

bool ApplyImportPrefs(const std::string& text, DisplaySettings& s) {
  if (text.empty()) return false;
  s.importRes = 0.f;  // réglages enregistrés avant la résolution imposée : automatique
  s.importInstrument = 0;
  s.importNature = 0;
  s.importInterp = true;
  s.importInterpD = 0.f;
  s.importInterpMethod = 0;
  s.importFormat.clear();
  s.importWaterOn = false;  // réglages antérieurs : pas de hauteurs d'eau
  s.importAntAuto = false;  // ni de décalage sonde / antenne
  s.importAntFwd = s.importAntStbd = 0.f;
  s.importAntEstOk = false;
  s.importAntEstFwd = s.importAntEstStbd = 0.f;
  std::istringstream in(text);
  std::string line;
  while (std::getline(in, line)) {
    const size_t e = line.find('=');
    if (e == std::string::npos) continue;
    const std::string k = line.substr(0, e);
    std::string v;
    for (size_t i = e + 1; i < line.size(); ++i) {
      if (line[i] == '\\' && i + 1 < line.size()) { v += line[i + 1] == 'n' ? '\n' : line[i + 1]; ++i; }
      else v += line[i];
    }
    const double d = std::atof(v.c_str());
    if (k == "crsCode") s.crsCode = v;
    else if (k == "crsZone") s.crsZone = (int)d;
    else if (k == "crsSouth") s.crsSouth = d != 0;
    else if (k == "crsCustom") s.crsCustom = v;
    else if (k == "readPrj") s.readPrj = d != 0;
    else if (k == "vrefMode") s.vrefMode = OldVrefMode((int)d);  // ancienne numérotation
    else if (k == "vref2") s.vrefMode = std::max(0, std::min(kVrefModes - 1, (int)d));
    else if (k == "vrefFranceDir") s.vrefFranceDir = v;
    else if (k == "vrefZh") s.vrefZh = v;
    else if (k == "vrefGeoid") s.vrefGeoid = v;
    else if (k == "vrefDirect") s.vrefDirect = v;
    else if (k == "vrefInvert") s.vrefInvert = d != 0;
    else if (k == "order") s.importOrder = (int)d;
    else if (k == "sign") s.importSign = (int)d;
    else if (k == "offset") s.importOffset = (float)d;
    else if (k == "antAuto") s.importAntAuto = d != 0;
    else if (k == "antFwd") s.importAntFwd = (float)d;
    else if (k == "antStbd") s.importAntStbd = (float)d;
    else if (k == "antEstOk") s.importAntEstOk = d != 0;
    else if (k == "antEstFwd") s.importAntEstFwd = (float)d;
    else if (k == "antEstStbd") s.importAntEstStbd = (float)d;
    else if (k == "zFeet") s.importZFeet = d != 0;
    else if (k == "res") s.importRes = (float)d;
    else if (k == "instr") s.importInstrument = std::max(0, std::min(5, (int)d));
    else if (k == "nature") s.importNature = std::max(0, std::min(2, (int)d));
    else if (k == "interp") s.importInterp = d != 0;
    else if (k == "dmax") s.importInterpD = (float)std::max(0.0, d);
    else if (k == "imode") s.importInterpMethod = std::max(0, std::min(3, (int)d));
    else if (k == "fmt") s.importFormat = v;
    else if (k == "waterOn") s.importWaterOn = d != 0;
    else if (k == "water") s.importWater = v;
    else if (k == "waterSign") s.importWaterSign = (int)d;
    else if (k == "waterFmt") s.importWaterFormat = v;
    else if (k == "waterFeet") s.importWaterFeet = d != 0;
    else if (k == "cache") s.importCache = d != 0;
    else if (k == "nodataOn") s.importNodataOn = d != 0;
    else if (k == "nodata") s.importNodata = v;
    else if (k == "boundsOn") s.importBoundsOn = d != 0;
    else if (k == "boundMin") s.boundMin = (float)d;
    else if (k == "boundMax") s.boundMax = (float)d;
  }
  return true;
}

// ------------------------------------------------------------ courbe de marée
time_t PersonalDepth_pi::TideChartTime() const {
  return m_set.tideMode == 1 ? std::time(nullptr)
                             : (time_t)(m_set.tideTime + (long long)m_set.tideShiftMin * 60);
}

std::vector<TideChartPoint> PersonalDepth_pi::TideChartPoints(wxString& source, wxString& why) {
  std::vector<TideChartPoint> pts;
  source.clear();
  why.clear();
  if (m_set.tideMode == 0) { why = U8("Marée désactivée (profondeurs au zéro hydrographique)"); return pts; }
  if (!m_tideBoxOk) { why = U8("En attente de données affichées"); return pts; }
  if (m_set.tideSource == 1) {  // FES2022 : 9 points répartis sur l'emprise des données
    if (!m_fes) { why = U8("Marée FES2022 en préparation"); return pts; }
    source = U8("FES2022 maillé");
    static const char* names[3][3] = {{"NO", "N", "NE"}, {"O", "Centre", "E"}, {"SO", "S", "SE"}};
    const double fx[3] = {0.1, 0.5, 0.9}, fy[3] = {0.9, 0.5, 0.1};
    for (int j = 0; j < 3; ++j)
      for (int i = 0; i < 3; ++i) {
        TideChartPoint p;
        p.lon = m_tideBox[0] + fx[i] * (m_tideBox[1] - m_tideBox[0]);
        p.lat = m_tideBox[2] + fy[j] * (m_tideBox[3] - m_tideBox[2]);
        // hors du modèle (à terre) ou sans référence : point écarté
        if (std::isnan(m_fes->z0At(p.lon, p.lat)) ||
            std::isnan(m_fes->heightNM(p.lon, p.lat, (double)TideChartTime())))
          continue;
        p.name = U8(names[j][i]);
        pts.push_back(p);
      }
    if (pts.empty()) why = U8("Aucun point de l'emprise dans le modèle FES2022");
    // le centre en premier
    std::stable_sort(pts.begin(), pts.end(), [](const TideChartPoint& a, const TideChartPoint& b) {
      return a.name == "Centre" && b.name != "Centre";
    });
    return pts;
  }
  if (m_tideStation < 0) { why = U8("Aucune station de marée"); return pts; }
  source = U8("Station OpenCPN");
  TideChartPoint p;
  p.name = m_tideStationName;
  p.lon = m_tideStationLon;  // position de la station
  p.lat = m_tideStationLat;
  pts.push_back(p);
  return pts;
}

void PersonalDepth_pi::TideChartSeries(const std::vector<TideChartPoint>& pts,
                                       const std::vector<time_t>& t,
                                       std::vector<std::vector<double>>& h) {
  h.assign(pts.size(), std::vector<double>(t.size(), NAN));
  if (pts.empty() || t.empty()) return;
  if (m_set.tideSource == 1) {
    if (!m_fes) return;
    for (size_t k = 0; k < pts.size(); ++k) {
      const double z0 = m_fes->z0At(pts[k].lon, pts[k].lat);
      for (size_t i = 0; i < t.size(); ++i)
        h[k][i] = m_fes->heightNM(pts[k].lon, pts[k].lat, (double)t[i]) + z0;  // au-dessus du ZH
    }
    return;
  }
  std::unique_ptr<HostApi> api;
  try { api = GetHostApi(); } catch (...) {}
  HostApi121* a = dynamic_cast<HostApi121*>(api.get());
  if (!a || m_tideStation < 0) return;
  for (size_t i = 0; i < t.size(); ++i) {
    float v = 0;
    if (a->GetTideHeight(m_tideStation, t[i], &v) && std::isfinite(v)) h[0][i] = v;
  }
}

void PersonalDepth_pi::ShowTideChart() {
  if (!m_chart) m_chart = new TideChartDialog(GetOCPNCanvasWindow(), this);
  m_chart->Show();
  m_chart->Raise();
  m_chart->TideUpdated();
}

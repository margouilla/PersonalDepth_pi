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
#include <cmath>
#include <filesystem>
#include <set>
#include <cstdio>
#include <ctime>

#include "BathyDialog.h"
#include "TideChart.h"
#include "BathyRenderer.h"
#include "core/WebMerc.h"
#include "core/SurveyShape.h"
#include "core/Triangulate.h"

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
            "LandXML TIN, GeoTIFF/BAG avec GDAL) sous forme de surface colorée ombrée "
            "et de sondes chiffrées exactes. Palette, seuils de profondeur, zone "
            "dangereuse et ombrage se modifient instantanément, sans régénérer les "
            "données — à la manière du module PBG de TimeZero.");
}

int PersonalDepth_pi::Init() {
  m_renderer = std::make_unique<BathyRenderer>();
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

  m_timer = std::make_unique<Ticker>(this);
  m_timer->Start(250);
  m_cursorRefresh = std::make_unique<CursorRefresh>();

  return WANTS_OVERLAY_CALLBACK | WANTS_OPENGL_OVERLAY_CALLBACK | WANTS_CURSOR_LATLON |
         WANTS_TOOLBAR_CALLBACK | INSTALLS_TOOLBAR_TOOL | WANTS_CONFIG | WANTS_PREFERENCES |
         WANTS_MOUSE_EVENTS;
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
  // Fenêtres supprimées tout de suite : Destroy() les laisse à wxWidgets, qui
  // les supprime plus tard, quand OpenCPN a déjà déchargé le plugin (mise à
  // jour, désinstallation) -> appel de code disparu et plantage.
  m_events.reset();
  if (m_chart) { m_chart->Hide(); delete m_chart; m_chart = nullptr; }
  if (m_dlg) { m_dlg->Hide(); delete m_dlg; m_dlg = nullptr; }
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

void PersonalDepth_pi::OnTick() {
  if (UpdateLanguage()) {  // langue changée : fenêtres reconstruites dans la nouvelle langue
    const bool dlgShown = m_dlg && m_dlg->IsShown(), chartShown = m_chart && m_chart->IsShown();
    wxPoint pos = m_dlg ? m_dlg->GetPosition() : wxDefaultPosition;
    if (m_chart) { m_chart->Destroy(); m_chart = nullptr; }
    if (m_dlg) { m_dlg->Destroy(); m_dlg = nullptr; }
    ++m_set.version;
    m_renderer->Invalidate();
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
    if (la0 > la1) { m_tideBoxOk = false; return U8("En attente d'une couche chargée"); }
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
    bool gi = false;
    if (!(*it)->probe(m_curLon, m_curLat, radiusDeg, gz, sz, dist, &sid, &gi)) continue;
    const float v = !std::isnan(sz) ? sz : gz;
    const float dk = m_set.depthK;
    const wxString du = m_set.depthUnit;
    wxString base = U8(!std::isnan(sz) ? "Sonde " : "Grille ") + wxString::Format("%.2f %s", v * dk, du);
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
    return;
  }
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
  c->Write("Banner", m_set.banner);
  c->Write("BannerPos", m_set.bannerPos);
  c->Write("BannerFx", (double)m_set.bannerFx);
  c->Write("BannerFy", (double)m_set.bannerFy);
  c->Write("BannerLock", m_set.bannerLock);
  c->Write("PanelTab", m_set.panelTab);
  c->Write("BannerVertical", m_set.bannerVertical);
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
  c->Write("VrefChoice", m_set.vrefMode);
  c->Write("ImportOrder", m_set.importOrder);
  c->Write("ImportSign", m_set.importSign);
  c->Write("ImportOffset", (double)m_set.importOffset);
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
  c->Read("PanelTab", &m_set.panelTab, m_set.panelTab);
  c->Read("BannerVertical", &m_set.bannerVertical, m_set.bannerVertical);
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
    if (!ok && err.empty()) err = pd::tr("échec");
    std::string e2;
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
  StartDbJob(JobLabel("Import (%d fichier(s))", files.size()),
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
                 if (p.storeRes > 0) m.res = p.storeRes;  // résolution imposée à l'import
                 // date du levé : imposée, sinon première date de la colonne
                 // date du fichier, sinon nom du fichier ou date du fichier
                 m.date = p.surveyDate;
                 // (dates avant 1970 comprises : pas de gmtime, NULL sous Windows)
                 if (m.date.empty() && cloud.tMin != 0) m.date = pd::FormatTime(cloud.tMin).substr(0, 10);
                 if (m.date.empty()) m.date = GuessSurveyDate(p.path);
                 m.params = p.importPrefs;
                 m.crs = L.crsUsed().empty() ? "WGS84" : L.crsUsed();
                 m.vref = p.vref.active() ? (p.vref.label.empty() ? "converti" : p.vref.label) : "ZH";
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
  const bool skipInterp = !m_set.interpShow;
  p.custom = [folder, hidden, b0, b1, b2, b3, target, skipInterp](
                 PointCloud& pc, double& dl, double& dt, std::vector<int64_t>& src,
                 std::vector<uint8_t>& interp, std::string& err) {
    BathyDb db;
    int used = 0;
    if (!db.open(folder, err)) return false;
    db.setHiddenCells(hidden);
    return db.buildView(b0, b1, b2, b3, target, 4000000, pc, dl, dt, used, err, nullptr, &src,
                        &interp, skipInterp);
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
  const bool pressed = m_bannerDown != BannerHit::None || m_bannerDrag;
  if (!button && !(pressed && (e.Dragging() || e.Moving()))) return false;
  if (DrawLayers().empty()) { m_renderer->HideBanner(); return false; }  // rien d'affiché
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
      m_bannerDown = BannerHit::None;
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
        m_bannerDown = BannerHit::None;
        SaveConfig();
        if (m_dlg) m_dlg->RefreshPalette();
      }
      RequestRefresh(GetOCPNCanvasWindow());
      return true;
    }
    if (!e.LeftUp()) return true;  // bouton tenu sur le bandeau : la carte ne bouge pas
  }
  const int part = h.PartAt(p);
  if (part == BannerHit::None) {
    m_bannerDown = BannerHit::None;
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
    m_bannerDown = BannerHit::None;
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
    case BannerHit::Gear:
      if (!m_dlg) m_dlg = new BathyDialog(GetOCPNCanvasWindow(), this);
      m_dlg->Show();
      m_dlg->Raise();
      SetToolbarItemState(m_toolId, true);
      break;
    default:
      break;
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
  m_autoInfo = wxString::Format(U8("Automatique : %d seuils de %.1f à %.1f %s (%s%s)"),
                                (int)r.stops.size(), lo * k, hi * k, s.depthUnit,
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
  num("vrefMode", s.vrefMode);
  kv("vrefFranceDir", s.vrefFranceDir);
  kv("vrefZh", s.vrefZh);
  kv("vrefGeoid", s.vrefGeoid);
  kv("vrefDirect", s.vrefDirect);
  num("vrefInvert", s.vrefInvert);
  num("order", s.importOrder);
  num("sign", s.importSign);
  num("offset", s.importOffset);
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
    else if (k == "vrefMode") s.vrefMode = (int)d;
    else if (k == "vrefFranceDir") s.vrefFranceDir = v;
    else if (k == "vrefZh") s.vrefZh = v;
    else if (k == "vrefGeoid") s.vrefGeoid = v;
    else if (k == "vrefDirect") s.vrefDirect = v;
    else if (k == "vrefInvert") s.vrefInvert = d != 0;
    else if (k == "order") s.importOrder = (int)d;
    else if (k == "sign") s.importSign = (int)d;
    else if (k == "offset") s.importOffset = (float)d;
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

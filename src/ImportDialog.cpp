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
#include "ImportDialog.h"

#include "core/BathyDb.h"
#include "TextFormatDialog.h"
#include "WaterLevelDialog.h"
#include "core/WaterLevel.h"

#include <wx/collpane.h>
#include <wx/display.h>

#include <cmath>
#include <cstdlib>
#include <sstream>

#include <wx/dir.h>
#include <wx/dirdlg.h>
#include <wx/filedlg.h>
#include <wx/filename.h>
#include <wx/tokenzr.h>

#include <algorithm>
#include <set>

#include "PersonalDepth_pi.h"
#include "core/Projection.h"

#define U8(s) wxString::FromUTF8(pd::tr(s))
#define W8(s) wxString::FromUTF8(s)  // données : pas de traduction

using namespace pd;

static const char* kBathyFilter =
    "Bathymétrie (*.xyz;*.txt;*.csv;*.dat;*.asc;*.xml;*.tif;*.tiff;*.bag)|"
    "*.xyz;*.XYZ;*.txt;*.csv;*.dat;*.asc;*.xml;*.landxml;*.tif;*.tiff;*.bag|"
    "Tous les fichiers|*";
static const char* kBathyExt[] = {"xyz", "txt", "csv", "dat", "asc", "xml", "landxml", "tif", "tiff", "bag"};

// libellé d'un système du catalogue dans la langue de l'interface
static wxString CrsLabel(const CrsEntry& e) {
  wxString n = U8(e.name.c_str());
  n.Replace(W8("(conique conforme)"), U8("(conique conforme)"));  // RGF93 / CCnn
  return U8(e.group.c_str()) + W8(" — ") + n;
}

ImportDialog::ImportDialog(wxWindow* parent, PersonalDepth_pi* pi,
                           const std::vector<DbSource>* edit)
    : wxDialog(parent, wxID_ANY,
               edit ? U8("Modifier les sources") : U8("Importer dans la base de bathymétrie"),
               wxDefaultPosition, wxDefaultSize, wxDEFAULT_DIALOG_STYLE | wxRESIZE_BORDER),
      m_pi(pi) {
  m_init = m_pi->Settings();
  if (edit && !edit->empty()) {
    m_edit = true;
    m_srcs = *edit;
    // réglages d'import de la première source (bases antérieures : inconnus,
    // on part alors des réglages du dernier import)
    m_prefsKnown = ApplyImportPrefs(m_srcs[0].params, m_init);
  }
  m_initPrefs = SerializeImportPrefs(m_init);
  // Disposition : fichiers et réglages courants toujours visibles ; systèmes
  // de coordonnées, référence verticale, hauteurs d'eau et options avancées
  // dans des sections repliées dont le titre résume le réglage. Contenu
  // défilant pour les petits écrans ; boutons toujours visibles en bas.
  m_sc = new wxScrolledWindow(this, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxVSCROLL);
  wxScrolledWindow* sc = m_sc;
  sc->SetScrollRate(0, FromDIP(12));
  auto* col = new wxBoxSizer(wxVERTICAL);
  col->Add(BuildFiles(sc), 1, wxEXPAND | wxALL, 6);
  col->Add(BuildMain(sc), 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, 10);
  auto pane = [&](wxCollapsiblePane*& cp, wxSizer* (ImportDialog::*build)(wxWindow*)) {
    cp = new wxCollapsiblePane(sc, wxID_ANY, "-", wxDefaultPosition, wxDefaultSize,
                               wxCP_DEFAULT_STYLE | wxCP_NO_TLW_RESIZE);
    wxWindow* in = cp->GetPane();
    auto* ps = new wxBoxSizer(wxVERTICAL);
    ps->Add((this->*build)(in), 0, wxEXPAND | wxLEFT | wxBOTTOM, FromDIP(14));
    in->SetSizer(ps);
    col->Add(cp, 0, wxEXPAND | wxLEFT | wxRIGHT, 6);
    cp->Bind(wxEVT_COLLAPSIBLEPANE_CHANGED, [this](wxCollapsiblePaneEvent&) { FitToContent(); });
  };
  pane(m_paneCrs, &ImportDialog::BuildCrs);
  pane(m_paneVref, &ImportDialog::BuildVref);
  pane(m_paneWater, &ImportDialog::BuildWater);
  pane(m_paneAdv, &ImportDialog::BuildAdvanced);

  // Date du levé : déduite du nom de fichier (AAAAMMJJ) ou de la date du
  // fichier, ou imposée
  auto* dr = new wxBoxSizer(wxHORIZONTAL);
  m_dateOn = new wxCheckBox(sc, wxID_ANY, U8("Date du levé imposée"));
  m_datePick = new wxDatePickerCtrl(sc, wxID_ANY, wxDateTime::Today(), wxDefaultPosition,
                                    wxDefaultSize, wxDP_DROPDOWN | wxDP_SHOWCENTURY);
  m_dateOn->SetToolTip(U8("Sinon : première date de la colonne date (format personnalisé), "
                         "ou date AAAAMMJJ trouvée dans le nom du fichier, à défaut date du "
                         "fichier. La date sert à la règle de priorité « plus récent »."));
  if (m_edit) {
    wxDateTime d;
    if (d.ParseISODate(W8(m_srcs[0].date.c_str()))) m_datePick->SetValue(d);
    m_dateOn->SetValue(true);
    m_dateOn->SetLabel(U8("Date du levé"));
  }
  m_datePick->Enable(m_dateOn->GetValue());
  m_dateOn->Bind(wxEVT_CHECKBOX, [this](wxCommandEvent&) { m_datePick->Enable(m_dateOn->GetValue()); });
  dr->Add(m_dateOn, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 6);
  dr->Add(m_datePick, 0, wxRIGHT, 16);

  // Classement dans la base (même ligne que la date)
  m_class = new wxComboBox(sc, wxID_ANY, "", wxDefaultPosition, wxDefaultSize, 0, nullptr,
                           wxCB_DROPDOWN);
  std::set<std::string> seen;
  for (const auto& d : m_pi->DbSources())
    if (seen.insert(d.group).second) m_class->Append(W8(d.group.c_str()));
  m_class->SetHint(m_edit ? U8("vide : classe actuelle de chaque source")
                          : U8("vide : déduite du nom de chaque fichier (LITTO3D, PFE…)"));
  dr->Add(new wxStaticText(sc, wxID_ANY, U8("Classe")), 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 6);
  dr->Add(m_class, 1, wxALIGN_CENTER_VERTICAL);
  col->Add(dr, 0, wxEXPAND | wxALL, 10);
  sc->SetSizer(col);

  auto* top = new wxBoxSizer(wxVERTICAL);
  top->Add(sc, 1, wxEXPAND);
  auto* btns = new wxStdDialogButtonSizer();
  auto* ok = new wxButton(this, wxID_OK, m_edit ? U8("Appliquer") : U8("Importer"));
  btns->AddButton(ok);
  btns->AddButton(new wxButton(this, wxID_CANCEL, U8("Annuler")));
  btns->Realize();
  top->Add(btns, 0, wxEXPAND | wxALL, 8);
  ok->Bind(wxEVT_BUTTON, &ImportDialog::OnOk, this);
  SetSizer(top);
  m_btnH = btns->GetMinSize().y + FromDIP(16);

  // une section dont le réglage n'est pas celui par défaut s'ouvre d'office
  UpdatePaneLabels();
  if (m_waterOn->GetValue()) m_paneWater->Expand();
  // titres des sections tenus à jour (réglages changés dans les sections ou
  // dans les fenêtres « Format… »)
  Bind(wxEVT_IDLE, [this](wxIdleEvent& e) { UpdatePaneLabels(); e.Skip(); });
  FitToContent();
  UpdateSummary();
  CentreOnParent();
}

// Taille : tout le contenu si l'écran le permet (sections ouvertes comprises),
// sinon 90 % de sa hauteur avec barre de défilement
void ImportDialog::FitToContent() {
  if (!m_sc || !m_sc->GetSizer() || !m_paneAdv) return;  // fenêtre en construction
  m_sc->Layout();
  m_sc->FitInside();
  const wxSize want = m_sc->GetSizer()->GetMinSize();
  const int disp = wxDisplay::GetFromWindow(this);
  const wxRect area = wxDisplay(disp == wxNOT_FOUND ? 0u : (unsigned)disp).GetClientArea();
  const int h = std::min(want.y + m_btnH + FromDIP(40), area.height * 9 / 10);
  const int w = std::max(GetSize().x, std::min(std::max(FromDIP(600), want.x + FromDIP(30)),
                                               area.width * 9 / 10));
  m_sc->SetMinSize(wxSize(-1, FromDIP(200)));
  SetSize(w, h);
  Layout();
}

// Titre des sections repliables : nom et résumé du réglage (raccourci)
static wxString PaneTitle(const wxString& name, const wxString& summary) {
  // espace avant les deux-points en français seulement
  wxString t = name + (pd::Language() == pd::kFrench ? W8(" : ") : W8(": ")) + summary;
  if (t.length() > 90) t = t.Left(88) + W8("…");
  return t;
}

void ImportDialog::UpdatePaneLabels() {
  if (!m_paneAdv) return;
  // coordonnées
  wxString crs;
  {
    const auto& cat = CrsCatalog();
    const int sel = m_crs->GetSelection();
    if (sel == (int)cat.size()) {
      crs = m_crsCustom->GetValue().Trim().Trim(false);
      if (crs.empty()) crs = U8("code EPSG ou chaîne PROJ à saisir");
    } else if (sel >= 0) {
      crs = CrsLabel(cat[sel]);
      if (cat[sel].zoneMin > 0)
        crs += wxString::Format(W8(", %d %s"), m_zone->GetValue(),
                                m_hemi->GetSelection() == 1 ? U8("Sud") : U8("Nord"));
    }
    if (m_prj->GetValue()) crs += U8(" (.prj prioritaire)");
  }
  // référence verticale
  static const char* vshort[kVrefModes] = {
      "zéro des cartes local", "niveau moyen", "LAT", "hauteurs ellipsoïdales", "IGN69 / IGN78C → ZH",
      "autre référence → zéro des cartes (surfaces)", "grille d'écart → zéro des cartes", "non rattachée"};
  const wxString vref = U8(vshort[std::max(0, std::min(kVrefModes - 1, m_vref->GetSelection()))]);
  // hauteurs d'eau
  wxString water = U8("aucune");
  if (m_waterOn->GetValue()) {
    const wxString w = m_water->GetValue().Trim().Trim(false);
    water = w.empty() ? U8("fichier ou dossier à choisir") : wxFileName(w).GetFullName();
  }
  // options avancées : seulement les réglages actifs
  wxArrayString adv;
  const wxString u = m_unitF == 1.0 ? "m" : "ft";
  if (std::fabs(m_offset->GetValue()) > 1e-9)
    adv.Add(wxString::Format(U8("décalage %+.2f "), m_offset->GetValue()) + u);
  if (m_nodataOn->GetValue()) adv.Add(W8("NODATA ") + m_nodata->GetValue());
  if (m_boundsOn->GetValue())
    adv.Add(wxString::Format(U8("bornes %.1f … %.1f "), m_boundMin->GetValue(), m_boundMax->GetValue()) + u);
  if (m_cache->GetValue()) adv.Add(U8("cache .pdpc"));
  wxString advs = adv.empty() ? U8("aucune") : wxJoin(adv, ';', 0);
  advs.Replace(";", "; ");
  const wxString labels[4] = {PaneTitle(U8("Coordonnées"), crs), PaneTitle(U8("Référence verticale"), vref),
                              PaneTitle(U8("Hauteurs d'eau observées"), water),
                              PaneTitle(U8("Avancé"), advs)};
  wxCollapsiblePane* panes[4] = {m_paneCrs, m_paneVref, m_paneWater, m_paneAdv};
  bool changed = false;
  for (int k = 0; k < 4; ++k)
    if (panes[k]->GetLabel() != labels[k]) {
      panes[k]->SetLabel(labels[k]);
      changed = true;
    }
  if (changed) m_sc->Layout();
}

// ---------------------------------------------------------------- fichiers
wxSizer* ImportDialog::BuildFiles(wxWindow* p) {
  auto* box = new wxStaticBoxSizer(
      wxVERTICAL, p, m_edit ? U8("Sources à modifier (fichiers d'origine)") : U8("Fichiers à importer"));
  wxWindow* sb = box->GetStaticBox();
  m_files = new wxListBox(sb, wxID_ANY, wxDefaultPosition, wxSize(-1, FromDIP(m_edit ? 90 : 130)), 0,
                          nullptr, wxLB_EXTENDED | wxLB_HSCROLL);
  box->Add(m_files, 1, wxEXPAND | wxALL, 4);
  if (m_edit) {  // pas d'ajout ni de retrait : ce sont les sources choisies
    for (const auto& d : m_srcs) m_files->Append(W8(d.path.c_str()));
    m_count = new wxStaticText(sb, wxID_ANY, "");
    wxString t = U8("La date et la classe se changent sans relire les données. Un changement de "
                    "lecture, de système de coordonnées, de référence verticale ou d'unité relit "
                    "les fichiers d'origine (même durée qu'un import).");
    if (!m_prefsKnown)
      t += U8("\nRéglages d'import d'origine inconnus (source importée avant l'enregistrement des réglages) : "
              "les réglages affichés sont ceux du dernier import, à vérifier.");
    if (m_srcs.size() > 1)
      t += U8("\nRéglages affichés : ceux de la première source ; ils s'appliquent à toutes.");
    m_count->SetLabel(t);
    m_count->Wrap(FromDIP(520));
    box->Add(m_count, 0, wxALL, 4);
    return box;
  }
  auto* r = new wxBoxSizer(wxHORIZONTAL);
  auto* addF = new wxButton(sb, wxID_ANY, U8("Ajouter des fichiers…"));
  auto* addD = new wxButton(sb, wxID_ANY, U8("Ajouter un dossier…"));
  auto* rem = new wxButton(sb, wxID_ANY, U8("Retirer"));
  addD->SetToolTip(U8("Tous les fichiers de bathymétrie du dossier et de ses sous-dossiers "
                      "(.xyz .txt .csv .dat .asc .xml .tif .bag)"));
  m_count = new wxStaticText(sb, wxID_ANY, "");
  r->Add(addF, 0, wxRIGHT, 4);
  r->Add(addD, 0, wxRIGHT, 4);
  r->Add(rem, 0, wxRIGHT, 10);
  r->Add(m_count, 0, wxALIGN_CENTER_VERTICAL);
  box->Add(r, 0, wxALL, 4);
  addF->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) {
    wxFileDialog fd(this, U8("Fichiers de bathymétrie"), "", "", U8(kBathyFilter),
                    wxFD_OPEN | wxFD_FILE_MUST_EXIST | wxFD_MULTIPLE);
    if (fd.ShowModal() != wxID_OK) return;
    wxArrayString ps;
    fd.GetPaths(ps);
    AddPaths(ps);
  });
  addD->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) {
    wxDirDialog dd(this, U8("Dossier de fichiers de bathymétrie"));
    if (dd.ShowModal() != wxID_OK) return;
    wxArrayString all, ps;
    wxDir::GetAllFiles(dd.GetPath(), &all);
    for (const wxString& f : all) {
      const wxString e = wxFileName(f).GetExt().Lower();
      for (const char* x : kBathyExt)
        if (e == x) { ps.Add(f); break; }
    }
    ps.Sort();
    if (ps.empty())
      wxMessageBox(U8("Aucun fichier de bathymétrie dans ce dossier."), "PersonalDepth",
                   wxICON_INFORMATION, this);
    AddPaths(ps);
  });
  rem->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) {
    wxArrayInt sel;
    m_files->GetSelections(sel);
    std::sort(sel.begin(), sel.end());
    for (int k = (int)sel.size() - 1; k >= 0; --k) m_files->Delete(sel[k]);
    UpdateSummary();
  });
  return box;
}

// fichier de points texte (format personnalisable)
static bool IsTextPoints(const wxString& f) {
  const wxString e = f.AfterLast('.').Lower();
  return e == "xyz" || e == "txt" || e == "csv" || e == "dat" || e == "pos" || e == "pts";
}

void ImportDialog::AddPaths(const wxArrayString& paths) {
  wxString firstText;  // premier fichier de points texte de cet ajout
  for (const wxString& f : paths)
    if (m_files->FindString(f, true) == wxNOT_FOUND) {
      m_files->Append(f);
      if (firstText.empty() && IsTextPoints(f)) firstText = f;
    }
  UpdateSummary();
  // à chaque ajout de fichiers de points texte : fenêtre « Format du fichier
  // texte » ouverte d'office (aperçu sur le premier fichier ajouté) pour
  // vérifier colonnes, entête et sens de Z
  if (!firstText.empty()) CallAfter([this, firstText]() { OnFormat(firstText); });
}

void ImportDialog::UpdateSummary() {
  if (m_edit) return;
  const unsigned n = m_files->GetCount();
  m_count->SetLabel(n == 0 ? U8("aucun fichier") : wxString::Format(U8("%u fichier%s"), n, n > 1 ? "s" : ""));
  if (wxWindow* ok = FindWindow(wxID_OK)) ok->Enable(n > 0);
}

// ----------------------------------------------------------------- lecture
// Type de données (liste unique) <-> type d'instrument + nature enregistrés
// 0 auto, 1 grille, 2 monofaisceau, 3 nuage dense (multifaisceau, lidar),
// 4 sondes de carte, 5 plomb, 6 nuage de points (autre)
static int DataTypeIndex(int instrument, int nature) {
  if (nature == 1) return 1;
  switch (instrument) {
    case 1: return 2;
    case 2: case 3: return 3;
    case 4: return 4;
    case 5: return 5;
    default: return nature == 2 ? 6 : 0;
  }
}
static void DataTypeToPrefs(int idx, int& instrument, int& nature) {
  static const int instr[] = {0, 0, 1, 2, 4, 5, 0};
  static const int nat[] = {0, 1, 2, 2, 2, 2, 2};
  idx = std::max(0, std::min(6, idx));
  instrument = instr[idx];
  nature = nat[idx];
}

// Réglages courants : colonnes, sens et unité de Z, type de données, interpolation,
// résolution
wxSizer* ImportDialog::BuildMain(wxWindow* p) {
  const DisplaySettings& st = m_init;
  wxString orders[] = {U8("X Y Z (lon lat z / Est Nord z)"), U8("Y X Z (lat lon z / Nord Est z)"),
                       U8("Format personnalisé (colonnes choisies)")};
  wxString signs[] = {U8("Automatique (majorité négative = altitudes)"),
                      U8("Profondeurs positives vers le bas"),
                      U8("Altitudes (fond négatif, Z positif vers le haut)")};
  m_order = new wxChoice(p, wxID_ANY, wxDefaultPosition, wxDefaultSize, 3, orders);
  m_sign = new wxChoice(p, wxID_ANY, wxDefaultPosition, wxDefaultSize, 3, signs);
  m_fmt.deserialize(st.importFormat);
  m_order->SetSelection(m_fmt.custom ? 2 : std::max(0, std::min(1, st.importOrder)));
  m_order->SetToolTip(U8("Fichiers texte (.xyz .txt .csv .dat) : les trois premiers nombres de chaque "
                         "ligne, ou un format personnalisé (lignes d'entête, séparateurs, colonnes X, "
                         "Y, Z, date et heure) réglé avec « Format… »."));
  m_fmtBtn = new wxButton(p, wxID_ANY, U8("Format…"), wxDefaultPosition, wxDefaultSize, wxBU_EXACTFIT);
  m_fmtInfo = new wxStaticText(p, wxID_ANY, m_fmt.custom ? TextFormatDialog::Describe(m_fmt) : wxString());
  m_fmtInfo->SetForegroundColour(wxColour(90, 90, 90));
  m_fmtBtn->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { OnFormat(); });
  m_order->Bind(wxEVT_CHOICE, [this](wxCommandEvent&) {
    if (m_order->GetSelection() == 2 && !m_fmt.custom) OnFormat();
    m_fmtInfo->SetLabel(m_order->GetSelection() == 2 ? TextFormatDialog::Describe(m_fmt) : wxString());
    FitToContent();
  });
  m_sign->SetSelection(std::max(0, std::min(2, st.importSign)));
  m_sign->SetToolTip(U8("Sens des valeurs Z du fichier. Les rasters (.asc, .tif) suivent la même règle."));
  // unité verticale des valeurs du fichier ; décalage et bornes s'expriment
  // dans cette unité, la base reste en mètres
  m_unitF = st.importZFeet ? 0.3048 : 1.0;
  wxString units[] = {U8("Mètres"), U8("Pieds")};
  m_zUnit = new wxChoice(p, wxID_ANY, wxDefaultPosition, wxDefaultSize, 2, units);
  m_zUnit->SetSelection(st.importZFeet ? 1 : 0);
  m_zUnit->SetToolTip(U8("Unité des valeurs Z du fichier. Les profondeurs sont converties en "
                         "mètres dans la base ; l'affichage suit ensuite l'unité de profondeur "
                         "choisie dans OpenCPN (Options › Affichage › Unités)."));
  m_zUnit->Bind(wxEVT_CHOICE, [this](wxCommandEvent&) { OnZUnit(); });

  // type de données : grille (déjà traitée) ou nuage de points, et instrument
  m_dtype = new wxChoice(p, wxID_ANY);
  for (const char* t : {"automatique (d'après les données)", "grille (pas régulier, déjà traitée)",
                        "sondeur monofaisceau (routes, profils)",
                        "nuage dense (multifaisceau, lidar)", "sondes de carte (sélection)",
                        "sondes au plomb (levés anciens)", "nuage de points (autre)"})
    m_dtype->Append(U8(t));
  m_dtype->SetSelection(DataTypeIndex(st.importInstrument, st.importNature));
  m_dtype->SetToolTip(U8("Grille : nœuds réguliers, déjà traitée, stockée à son pas sans "
                         "interpolation. Les autres types sont des nuages de points : la surface est "
                         "interpolée entre les sondes, sans franchir un vide plus large que la "
                         "distance maximale. Automatique : grille ou nuage reconnu d'après les "
                         "données, levé en lignes reconnu à l'écart entre les routes. Monofaisceau : "
                         "lignes. Nuage dense : trois fois l'espacement des sondes. Sondes de carte "
                         "et plomb : au moins les grands écarts courants entre sondes, plafond de "
                         "1 000 m / 500 m."));
  m_interp = new wxCheckBox(p, wxID_ANY, U8("Interpoler entre les sondes"));
  m_interp->SetValue(st.importInterp);
  m_interp->SetToolTip(U8("Surface triangulée entre les sondes, sans franchir un vide plus large "
                          "que la distance maximale. Marquée « interpolée » dans la base : une sonde "
                          "mesurée l'emporte toujours, et l'affichage peut la masquer ou "
                          "l'éclaircir."));
  m_imethod = new wxChoice(p, wxID_ANY);
  for (const char* t : {"méthode automatique", "TIN (linéaire)", "voisin naturel (Sibson)",
                        "IDW (inverse de la distance)"})
    m_imethod->Append(U8(t));
  m_imethod->SetSelection(std::max(0, std::min(3, st.importInterpMethod)));
  m_imethod->SetToolTip(U8("Méthode de calcul des valeurs interpolées. La zone remplie est la même "
                           "pour toutes (triangles dont aucun côté ne dépasse la distance maximale). "
                           "Automatique : voisin naturel pour un levé en lignes, sinon TIN. "
                           "TIN : plan dans chaque triangle, le plus rapide. Voisin naturel : surface "
                           "plus douce, sans arêtes de triangles. IDW : moyenne des sondes les plus "
                           "proches dans chaque quart autour du point, pondérée par l'inverse du carré "
                           "de la distance, peut former des « cuvettes » autour des sondes isolées."));
  m_dmaxOn = new wxCheckBox(p, wxID_ANY, U8("Distance max"));
  m_dmaxOn->SetValue(st.importInterpD > 0);
  m_dmax = new wxSpinCtrlDouble(p, wxID_ANY, "", wxDefaultPosition, wxSize(FromDIP(90), -1),
                                wxSP_ARROW_KEYS, 0.1, 100000, st.importInterpD > 0 ? st.importInterpD : 200.0, 10);
  m_dmax->SetDigits(1);
  const wxString dt = U8("Distance maximale d'interpolation : la surface n'est jamais tendue entre "
                         "deux sondes plus éloignées que cette distance (vides plus larges laissés "
                         "vides). Décoché : déduite de l'analyse du levé et du type de levé. "
                         "La valeur utilisée s'affiche dans Gestion des données.");
  m_dmaxOn->SetToolTip(dt);
  m_dmax->SetToolTip(dt);

  auto L = [&](const char* t) { return new wxStaticText(p, wxID_ANY, U8(t)); };
  auto* g = new wxFlexGridSizer(2, 6, 8);
  g->AddGrowableCol(1);
  {
    auto* orow = new wxBoxSizer(wxHORIZONTAL);
    orow->Add(m_order, 1, wxEXPAND | wxRIGHT, 4);
    orow->Add(m_fmtBtn, 0);
    auto* ocol = new wxBoxSizer(wxVERTICAL);
    ocol->Add(orow, 0, wxEXPAND);
    ocol->Add(m_fmtInfo, 0, wxTOP, 2);
    g->Add(L("Colonnes"), 0, wxALIGN_CENTER_VERTICAL); g->Add(ocol, 0, wxEXPAND);
  }
  {
    auto* zr = new wxBoxSizer(wxHORIZONTAL);
    zr->Add(m_sign, 1, wxRIGHT, 6);
    zr->Add(m_zUnit, 0);
    g->Add(L("Valeurs Z"), 0, wxALIGN_CENTER_VERTICAL); g->Add(zr, 0, wxEXPAND);
  }
  g->Add(L("Type de données"), 0, wxALIGN_CENTER_VERTICAL); g->Add(m_dtype, 0, wxEXPAND);
  {
    auto* ir = new wxBoxSizer(wxHORIZONTAL);
    ir->Add(m_imethod, 1, wxALIGN_CENTER_VERTICAL | wxRIGHT, 12);
    ir->Add(m_dmaxOn, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 4);
    ir->Add(m_dmax, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 4);
    ir->Add(L("m"), 0, wxALIGN_CENTER_VERTICAL);
    g->Add(m_interp, 0, wxALIGN_CENTER_VERTICAL); g->Add(ir, 0, wxEXPAND);
  }
  // pas horizontal de la base (avec l'interpolation : ensemble, ils donnent
  // l'aspect de la surface ; vaut aussi pour les grilles)
  // résolution de stockage (pas horizontal des tuiles) : estimée ou imposée
  m_resOn = new wxCheckBox(p, wxID_ANY, U8("Résolution imposée"));
  m_resOn->SetValue(st.importRes > 0);
  m_res = new wxSpinCtrlDouble(p, wxID_ANY, "", wxDefaultPosition, wxSize(FromDIP(90), -1),
                               wxSP_ARROW_KEYS, 0.05, 5000, st.importRes > 0 ? st.importRes : 5.0, 0.5);
  m_res->SetDigits(2);
  const wxString rt = U8("Pas horizontal de stockage des sondes et de la surface interpolée dans la "
                         "base. Décoché : déduit de l'analyse du levé (pas de la grille, espacement "
                         "des sondes le long des routes, ou pas fin de la surface interpolée d'un "
                         "nuage). Dans chaque case, la sonde la plus faible est gardée. Une "
                         "résolution plus grossière que l'espacement des sondes regroupe plusieurs "
                         "sondes par case et donne un aspect en blocs.");
  m_resOn->SetToolTip(rt);
  m_res->SetToolTip(rt);
  auto* resr = new wxBoxSizer(wxHORIZONTAL);  // même ligne : valeur et rappel
  resr->Add(m_res, 0, wxRIGHT, 4);
  resr->Add(L("m (automatique si décoché)"), 0, wxALIGN_CENTER_VERTICAL);
  g->Add(m_resOn, 0, wxALIGN_CENTER_VERTICAL); g->Add(resr, 0);
  m_resOn->Bind(wxEVT_CHECKBOX, [this](wxCommandEvent&) { m_res->Enable(m_resOn->GetValue()); });
  m_res->Enable(m_resOn->GetValue());
  auto sync = [this]() {
    const bool grid = m_dtype->GetSelection() == 1;  // grille : jamais interpolée
    m_interp->Enable(!grid);
    const bool on = m_interp->GetValue() && !grid;
    m_imethod->Enable(on);
    m_dmaxOn->Enable(on);
    m_dmax->Enable(on && m_dmaxOn->GetValue());
  };
  m_dtype->Bind(wxEVT_CHOICE, [sync](wxCommandEvent&) { sync(); });
  m_interp->Bind(wxEVT_CHECKBOX, [sync](wxCommandEvent&) { sync(); });
  m_dmaxOn->Bind(wxEVT_CHECKBOX, [sync](wxCommandEvent&) { sync(); });
  sync();
  return g;
}

// Hauteurs d'eau observées (échelle, marégraphe) : sondes horodatées corrigées
wxSizer* ImportDialog::BuildWater(wxWindow* p) {
  const DisplaySettings& st = m_init;
  m_waterOn = new wxCheckBox(p, wxID_ANY, U8("Corriger les sondes par des hauteurs d'eau observées"));
  m_waterOn->SetValue(st.importWaterOn);
  m_water = new wxTextCtrl(p, wxID_ANY, W8(st.importWater.c_str()), wxDefaultPosition,
                           wxSize(FromDIP(230), -1));
  m_waterFile = new wxButton(p, wxID_ANY, U8("Fichier…"), wxDefaultPosition, wxDefaultSize, wxBU_EXACTFIT);
  m_waterDir = new wxButton(p, wxID_ANY, U8("Dossier…"), wxDefaultPosition, wxDefaultSize, wxBU_EXACTFIT);
  m_waterCurve = new wxButton(p, wxID_ANY, U8("Courbe…"), wxDefaultPosition, wxDefaultSize, wxBU_EXACTFIT);
  m_waterFmtBtn = new wxButton(p, wxID_ANY, U8("Format…"), wxDefaultPosition, wxDefaultSize, wxBU_EXACTFIT);
  m_waterInfo = new wxStaticText(p, wxID_ANY, "");
  m_waterInfo->SetForegroundColour(wxColour(90, 90, 90));
  m_waterFmt.deserialize(st.importWaterFormat);
  m_waterSignV = std::max(0, std::min(1, st.importWaterSign));
  m_waterFeet = st.importWaterFeet;
  const wxString wt = U8("Fichiers texte d'une échelle ou d'un marégraphe : date (et heure) et hauteur "
                         "d'eau (ex. 2026-11-01 05:00:00,0.35). Un dossier : tous ses fichiers .txt, "
                         ".csv, .dat forment une seule série. Colonnes, unité, convention et fuseau "
                         "horaire se règlent avec « Format… ». Chaque sonde est corrigée de la "
                         "hauteur interpolée à son heure ; sans colonne date, la hauteur du jour "
                         "du levé est prise.");
  for (wxWindow* w : std::initializer_list<wxWindow*>{m_waterOn, m_water}) w->SetToolTip(wt);
  auto* col = new wxBoxSizer(wxVERTICAL);
  col->Add(m_waterOn, 0, wxBOTTOM, 4);
  auto* wr = new wxBoxSizer(wxHORIZONTAL);
  wr->Add(m_water, 1, wxALIGN_CENTER_VERTICAL | wxRIGHT, 4);
  wr->Add(m_waterFile, 0, wxRIGHT, 2);
  wr->Add(m_waterDir, 0, wxRIGHT, 2);
  wr->Add(m_waterFmtBtn, 0, wxRIGHT, 2);
  wr->Add(m_waterCurve, 0);
  col->Add(wr, 0, wxEXPAND);
  col->Add(m_waterInfo, 0, wxEXPAND | wxTOP, 2);
  UpdateWaterInfo();
  m_waterFile->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) {
    wxFileDialog fd(this, U8("Fichier de hauteurs d'eau"), "", "",
                    U8("Hauteurs d'eau (*.txt;*.csv;*.dat)|*.txt;*.csv;*.dat|Tous les fichiers|*.*"),
                    wxFD_OPEN | wxFD_FILE_MUST_EXIST);
    if (fd.ShowModal() != wxID_OK) return;
    m_water->SetValue(fd.GetPath());
    m_waterOn->SetValue(true);
    wxCommandEvent e(wxEVT_CHECKBOX); m_waterOn->GetEventHandler()->ProcessEvent(e);
    CallAfter([this] { OnWaterFormat(); });  // format réglé sur ce fichier
  });
  m_waterDir->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) {
    wxDirDialog dd(this, U8("Dossier des hauteurs d'eau"), m_water->GetValue());
    if (dd.ShowModal() != wxID_OK) return;
    m_water->SetValue(dd.GetPath());
    m_waterOn->SetValue(true);
    wxCommandEvent e(wxEVT_CHECKBOX); m_waterOn->GetEventHandler()->ProcessEvent(e);
    CallAfter([this] { OnWaterFormat(); });
  });
  m_waterFmtBtn->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { OnWaterFormat(); });
  m_waterCurve->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) {
    auto ser = std::make_shared<WaterLevelSeries>();
    std::string err;
    if (!ser->load(m_water->GetValue().Trim().Trim(false).ToStdString(wxConvUTF8), m_waterFmt,
                   m_waterFeet ? 0.3048 : 1.0, err)) {
      wxMessageBox(W8(err.c_str()), "PersonalDepth", wxOK | wxICON_WARNING, this);
      return;
    }
    WaterLevelDialog d(this, ser, m_waterFeet ? 0.3048 : 1.0, m_waterFeet ? "ft" : "m");
    d.ShowModal();
  });
  auto sync = [this]() {
    const bool w = m_waterOn->GetValue();
    for (wxWindow* c : std::initializer_list<wxWindow*>{m_water, m_waterFile, m_waterDir, m_waterCurve,
                                                        m_waterFmtBtn, m_waterInfo})
      c->Enable(w);
  };
  m_waterOn->Bind(wxEVT_CHECKBOX, [sync](wxCommandEvent&) { sync(); });
  sync();
  return col;
}

// Options avancées : décalage, NODATA, bornes de Z, cache
wxSizer* ImportDialog::BuildAdvanced(wxWindow* p) {
  const DisplaySettings& st = m_init;
  auto L = [&](const char* t) { return new wxStaticText(p, wxID_ANY, U8(t)); };
  m_offset = new wxSpinCtrlDouble(p, wxID_ANY, "", wxDefaultPosition, wxSize(FromDIP(90), -1),
                                  wxSP_ARROW_KEYS, -400, 400, st.importOffset / m_unitF, 0.01);
  m_offset->SetDigits(2);
  m_offset->SetToolTip(U8("Décalage constant ajouté aux profondeurs (dans l'unité de Z), "
                          "en plus de la transformation verticale"));
  m_cache = new wxCheckBox(p, wxID_ANY, U8("Cache binaire (.pdpc) à côté des fichiers (réimport rapide)"));
  m_cache->SetValue(st.importCache);
  auto* g = new wxFlexGridSizer(2, 4, 8);
  g->AddGrowableCol(1);
  m_offsetL = L("Décalage (m)");
  g->Add(m_offsetL, 0, wxALIGN_CENTER_VERTICAL); g->Add(m_offset, 0);
  // valeurs absentes : NODATA de l'utilisateur (Z brut du fichier)
  wxString nds[] = {"-9999", "9999", "-99999", "99999", "-9999 9999", "-99999 99999",
                    "-9999 9999 -99999 99999", "999 -999"};
  m_nodataOn = new wxCheckBox(p, wxID_ANY, U8("NODATA"));
  m_nodataOn->SetValue(st.importNodataOn);
  m_nodata = new wxComboBox(p, wxID_ANY, W8(st.importNodata.c_str()), wxDefaultPosition,
                            wxDefaultSize, 8, nds, wxCB_DROPDOWN);
  const wxString nt = U8("Valeurs Z du fichier à traiter comme absentes, séparées par des "
                         "espaces ou des « ; » (ex. -9999 9999 -99999 99999). Elles s'ajoutent "
                         "au NODATA déclaré dans l'entête des grilles ASC / GeoTIFF. Comparées "
                         "aux valeurs brutes, avant changement de signe et transformation.");
  m_nodataOn->SetToolTip(nt);
  m_nodata->SetToolTip(nt);
  g->Add(m_nodataOn, 0, wxALIGN_CENTER_VERTICAL); g->Add(m_nodata, 0, wxEXPAND);

  // bornes d'import : profondeur ZH finale (mêmes bornes que « Nettoyer »)
  m_boundsOn = new wxCheckBox(p, wxID_ANY, U8("Bornes de Z"));
  m_boundsOn->SetValue(st.importBoundsOn);
  m_boundMin = new wxSpinCtrlDouble(p, wxID_ANY, "", wxDefaultPosition, wxSize(FromDIP(90), -1),
                                    wxSP_ARROW_KEYS, -40000, 40000, st.boundMin / m_unitF, 1);
  m_boundMax = new wxSpinCtrlDouble(p, wxID_ANY, "", wxDefaultPosition, wxSize(FromDIP(90), -1),
                                    wxSP_ARROW_KEYS, -40000, 40000, st.boundMax / m_unitF, 1);
  m_boundMin->SetDigits(1);
  m_boundMax->SetDigits(1);
  const wxString bt = U8("Seules les sondes dont la profondeur finale (ZH, positive vers le "
                         "bas, après signe, décalage et transformation verticale ; dans "
                         "l'unité de Z choisie) est comprise "
                         "entre Z1 et Z2 sont importées. Exemple : -200 à 200 m garde l'estran "
                         "jusqu'à 200 m d'altitude et écarte les remplissages. Ce sont les mêmes "
                         "bornes que le bouton « Nettoyer » de la base. Au-delà de ±12 km, une "
                         "valeur est toujours ignorée.");
  m_boundsOn->SetToolTip(bt);
  m_boundMin->SetToolTip(bt);
  m_boundMax->SetToolTip(bt);
  auto* rr = new wxBoxSizer(wxHORIZONTAL);
  rr->Add(L("de"), 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 4);
  rr->Add(m_boundMin, 0, wxRIGHT, 4);
  rr->Add(L("à"), 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 4);
  rr->Add(m_boundMax, 0, wxRIGHT, 4);
  m_boundsU = L("m (profondeur ZH)");
  rr->Add(m_boundsU, 0, wxALIGN_CENTER_VERTICAL);
  g->Add(m_boundsOn, 0, wxALIGN_CENTER_VERTICAL); g->Add(rr, 0);


  auto sync = [this]() {
    m_nodata->Enable(m_nodataOn->GetValue());
    m_boundMin->Enable(m_boundsOn->GetValue());
    m_boundMax->Enable(m_boundsOn->GetValue());
  };
  m_nodataOn->Bind(wxEVT_CHECKBOX, [sync](wxCommandEvent&) { sync(); });
  m_boundsOn->Bind(wxEVT_CHECKBOX, [sync](wxCommandEvent&) { sync(); });
  sync();
  {  // libellés d'unité
    const wxString u = m_unitF == 1.0 ? "m" : "ft";
    m_offsetL->SetLabel(U8("Décalage (") + u + ")");
    m_boundsU->SetLabel(u + U8(" (profondeur ZH)"));
  }
  auto* col = new wxBoxSizer(wxVERTICAL);
  col->Add(g, 0, wxEXPAND);
  col->Add(m_cache, 0, wxTOP, 6);
  return col;
}

// ------------------------------------------------------------- coordonnées
wxSizer* ImportDialog::BuildCrs(wxWindow* p) {
  auto* box = new wxBoxSizer(wxVERTICAL);
  wxWindow* sb = p;
  const DisplaySettings& st = m_init;
  m_crs = new wxChoice(sb, wxID_ANY);
  {  // ajout en un seul appel : 300 systèmes, insertion une à une lente sous Windows
    wxArrayString items;
    items.reserve(CrsCatalog().size() + 1);
    for (const CrsEntry& e : CrsCatalog()) items.Add(CrsLabel(e));
    items.Add(U8("Autre — code EPSG ou chaîne PROJ"));
    m_crs->Append(items);
  }
  m_zone = new wxSpinCtrl(sb, wxID_ANY, "", wxDefaultPosition, wxSize(FromDIP(70), -1),
                          wxSP_ARROW_KEYS, 1, 60, 31);
  wxString hemis[] = {U8("Nord"), U8("Sud")};
  m_hemi = new wxChoice(sb, wxID_ANY, wxDefaultPosition, wxDefaultSize, 2, hemis);
  m_crsCustom = new wxTextCtrl(sb, wxID_ANY, "");
  m_crsCustom->SetHint(U8("ex. EPSG:32630 ou +proj=tmerc +lat_0=… +ellps=GRS80"));
  m_prj = new wxCheckBox(sb, wxID_ANY, U8("Utiliser le fichier .prj voisin s'il existe (prioritaire)"));
  auto* zoneRow = new wxBoxSizer(wxHORIZONTAL);
  zoneRow->Add(m_zone, 0, wxRIGHT, 6);
  zoneRow->Add(m_hemi, 0);
  // une ligne par réglage ; fuseau et EPSG / PROJ affichés seulement s'ils servent
  const int lw = FromDIP(110);
  auto row = [&](const char* label, wxSizer* content, int prop) {
    auto* r = new wxBoxSizer(wxHORIZONTAL);
    auto* t = new wxStaticText(sb, wxID_ANY, U8(label));
    t->SetMinSize(wxSize(lw, -1));
    r->Add(t, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 8);
    r->Add(content, prop, wxALIGN_CENTER_VERTICAL);
    box->Add(r, 0, wxEXPAND | wxBOTTOM, 4);
    return r;
  };
  auto* crsR = new wxBoxSizer(wxHORIZONTAL);
  crsR->Add(m_crs, 1);
  row("Système", crsR, 1);
  m_zoneRow = row("Fuseau UTM", zoneRow, 0);
  auto* custR = new wxBoxSizer(wxHORIZONTAL);
  custR->Add(m_crsCustom, 1);
  m_customRow = row("EPSG / PROJ", custR, 1);
  box->Add(m_prj, 0, wxTOP, 2);

  int sel = 0;
  const auto& cat = CrsCatalog();
  if (st.crsCode == "CUSTOM") sel = (int)cat.size();
  for (size_t k = 0; k < cat.size(); ++k)
    if (cat[k].code == st.crsCode) sel = (int)k;
  m_crs->SetSelection(sel);
  m_zone->SetValue(st.crsZone);
  m_hemi->SetSelection(st.crsSouth ? 1 : 0);
  m_crsCustom->SetValue(W8(st.crsCustom.c_str()));
  m_prj->SetValue(st.readPrj);
  UpdateCrsControls();
  m_crs->Bind(wxEVT_CHOICE, &ImportDialog::OnCrsChoice, this);
  return box;
}

void ImportDialog::UpdateCrsControls() {
  const auto& cat = CrsCatalog();
  const int sel = m_crs->GetSelection();
  const bool custom = sel == (int)cat.size();
  const bool zoned = !custom && sel >= 0 && cat[sel].zoneMin > 0;
  m_zone->Enable(zoned);
  m_hemi->Enable(zoned);
  m_crsCustom->Enable(custom);
  if (zoned) m_zone->SetRange(cat[sel].zoneMin, cat[sel].zoneMax);
  if (m_zoneRow) m_zoneRow->ShowItems(zoned);
  if (m_customRow) m_customRow->ShowItems(custom);
  if (m_sc) FitToContent();
}

void ImportDialog::OnCrsChoice(wxCommandEvent&) {
  const auto& cat = CrsCatalog();
  const int sel = m_crs->GetSelection();
  // nouvelle entrée à fuseau : hémisphère par défaut de la région
  if (sel >= 0 && sel < (int)cat.size() && cat[sel].zoneMin > 0)
    m_hemi->SetSelection(cat[sel].zoneSouthDefault ? 1 : 0);
  UpdateCrsControls();
}

// Définition et libellé du système choisi ; false si la saisie est invalide
bool ImportDialog::CurrentCrs(std::string& def, std::string& name) {
  const auto& cat = CrsCatalog();
  const int sel = m_crs->GetSelection();
  if (sel == (int)cat.size()) {
    def = m_crsCustom->GetValue().Trim().Trim(false).ToUTF8().data();
    name = def;
    if (def.empty()) {
      wxMessageBox(U8("Saisir un code EPSG ou une chaîne PROJ."), "PersonalDepth", wxICON_WARNING, this);
      return false;
    }
  } else if (sel >= 0) {
    const CrsEntry& e = cat[sel];
    def = CrsDefinition(e, m_zone->GetValue(), m_hemi->GetSelection() == 1);
    if (e.zoneMin) {
      char b[64];
      std::snprintf(b, sizeof b, "UTM %s %d%s", e.code.c_str() + 4, m_zone->GetValue(),
                    m_hemi->GetSelection() == 1 ? "S" : "N");
      name = b;
    } else {
      name = e.code;
    }
    if (e.code == "EPSG:4326") { def.clear(); name.clear(); }  // aucune conversion
  }
  if (def.empty()) return true;
  Crs test;
  std::string err;
  if (!Crs::Parse(def, test, err)) {
    wxMessageBox(U8("Système de coordonnées invalide : ") + U8(err.c_str()), "PersonalDepth",
                 wxICON_WARNING, this);
    return false;
  }
  return true;
}

// ----------------------------------------------------- référence verticale
wxSizer* ImportDialog::BuildVref(wxWindow* p) {
  auto* box = new wxBoxSizer(wxVERTICAL);
  wxWindow* sb = p;
  const DisplaySettings& st = m_init;
  // ordre : DisplaySettings::vrefMode (VrefModeDatum)
  wxString vrefs[kVrefModes] = {
      U8("Zéro des cartes local (ZH, CD…) — profondeurs des cartes, sondeur corrigé"),
      U8("Niveau moyen (MSL) — ex. GEBCO"),
      U8("Plus basse mer astronomique (LAT) — ex. EMODnet"),
      U8("Hauteurs ellipsoïdales (WGS84 / RGF93, GNSS)"),
      U8("IGN69 (continent) / IGN78C (Corse) → ZH : BathyElli v2.1 + RAF20 + RAC23"),
      U8("Autre référence terrestre → zéro des cartes : surface ZH/ellipsoïde + géoïde (fichiers)"),
      U8("Autre → zéro des cartes : grille d'écart référence / ZH (fichier)"),
      U8("Non rattachée (lac, retenue, plan d'eau sans marée)")};
  m_vref = new wxChoice(sb, wxID_ANY, wxDefaultPosition, wxDefaultSize, kVrefModes, vrefs);
  auto fileRow = [&](wxTextCtrl*& t, wxButton*& b) {
    auto* r = new wxBoxSizer(wxHORIZONTAL);
    t = new wxTextCtrl(sb, wxID_ANY);
    b = new wxButton(sb, wxID_ANY, U8("…"), wxDefaultPosition, wxSize(FromDIP(28), -1));
    r->Add(t, 1, wxRIGHT, 2);
    r->Add(b, 0);
    return r;
  };
  auto* rFr = fileRow(m_vrFrance, m_vrFranceBtn);
  auto* rZh = fileRow(m_vrZh, m_vrZhBtn);
  auto* rGe = fileRow(m_vrGeoid, m_vrGeoidBtn);
  auto* rDi = fileRow(m_vrDirect, m_vrDirectBtn);
  m_vrInvert = new wxCheckBox(sb, wxID_ANY, U8("inverser"));
  m_vrInvert->SetToolTip(U8("Cocher si la grille donne la profondeur du ZH sous la référence "
                            "(valeurs positives) au lieu de son altitude."));
  rDi->Add(m_vrInvert, 0, wxALIGN_CENTER_VERTICAL | wxLEFT, 4);
  m_vrFrance->SetHint(U8("dossier BathyElli *ZH*.glhi, RAF20.tac, RAC23.mnt"));
  m_vrZh->SetHint(U8("surface(s) ZH/ellipsoïde : .glhi .xyz .asc .gtx .tac .mnt (séparer par ;)"));
  m_vrGeoid->SetHint(U8("géoïde(s) de la référence : .tac .mnt .gtx .asc (séparer par ;, par priorité)"));
  m_vrDirect->SetHint(U8("altitude du ZH dans la référence : .asc .gtx .tac .xyz"));
  m_vrHelp = new wxStaticText(sb, wxID_ANY, "");
  m_vrHelp->SetForegroundColour(wxColour(90, 90, 90));
  // une ligne par réglage ; seules celles du mode choisi sont affichées
  const int lw = FromDIP(110);
  auto row = [&](const char* label, wxSizer* content) {
    auto* r = new wxBoxSizer(wxHORIZONTAL);
    auto* t = new wxStaticText(sb, wxID_ANY, U8(label));
    t->SetMinSize(wxSize(lw, -1));
    r->Add(t, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 8);
    r->Add(content, 1, wxALIGN_CENTER_VERTICAL);
    box->Add(r, 0, wxEXPAND | wxBOTTOM, 4);
    return r;
  };
  auto* vr = new wxBoxSizer(wxHORIZONTAL);
  vr->Add(m_vref, 1);
  row("Z du fichier", vr);
  m_vrRows[0] = row("Dossier France", rFr);
  m_vrRows[1] = row("Surface(s) ZH", rZh);
  m_vrRows[2] = row("Géoïde(s)", rGe);
  m_vrRows[3] = row("Grille d'écart", rDi);
  box->Add(m_vrHelp, 0, wxEXPAND | wxTOP, 2);

  m_vref->SetSelection(std::max(0, std::min(kVrefModes - 1, st.vrefMode)));
  m_vrFrance->SetValue(W8(st.vrefFranceDir.c_str()));
  m_vrZh->SetValue(W8(st.vrefZh.c_str()));
  m_vrGeoid->SetValue(W8(st.vrefGeoid.c_str()));
  m_vrDirect->SetValue(W8(st.vrefDirect.c_str()));
  m_vrInvert->SetValue(st.vrefInvert);
  UpdateVrefControls();
  m_vref->Bind(wxEVT_CHOICE, [this](wxCommandEvent&) { UpdateVrefControls(); });
  m_vrFranceBtn->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) {
    wxDirDialog dd(this, U8("Dossier BathyElli v2.1 + RAF20 + RAC23"), m_vrFrance->GetValue());
    if (dd.ShowModal() == wxID_OK) m_vrFrance->SetValue(dd.GetPath());
  });
  auto pick = [this](wxTextCtrl* t, const wxString& title, bool multi) {
    wxFileDialog fd(this, title, "", "",
                    U8("Grilles et surfaces (*.tac;*.mnt;*.gtx;*.asc;*.glhi;*.xyz;*.txt)|"
                       "*.tac;*.mnt;*.gtx;*.asc;*.glhi;*.xyz;*.txt|Tous|*"),
                    wxFD_OPEN | wxFD_FILE_MUST_EXIST | (multi ? wxFD_MULTIPLE : 0));
    if (fd.ShowModal() != wxID_OK) return;
    wxArrayString ps;
    fd.GetPaths(ps);
    wxString v;
    for (size_t k = 0; k < ps.size(); ++k) v << (k ? ";" : "") << ps[k];
    t->SetValue(v);
  };
  m_vrZhBtn->Bind(wxEVT_BUTTON, [this, pick](wxCommandEvent&) {
    pick(m_vrZh, U8("Surface(s) du zéro hydrographique / ellipsoïde"), true);
  });
  m_vrGeoidBtn->Bind(wxEVT_BUTTON, [this, pick](wxCommandEvent&) {
    pick(m_vrGeoid, U8("Grille(s) de la référence terrestre (géoïde)"), true);
  });
  m_vrDirectBtn->Bind(wxEVT_BUTTON, [this, pick](wxCommandEvent&) {
    pick(m_vrDirect, U8("Grille d'écart référence / ZH"), false);
  });
  return box;
}

void ImportDialog::UpdateVrefControls() {
  const int m = m_vref->GetSelection();
  const bool fr = m == 4;
  m_vrFrance->Enable(fr); m_vrFranceBtn->Enable(fr);
  m_vrZh->Enable(m == 5); m_vrZhBtn->Enable(m == 5);
  m_vrGeoid->Enable(m == 5); m_vrGeoidBtn->Enable(m == 5);
  m_vrDirect->Enable(m == 6); m_vrDirectBtn->Enable(m == 6); m_vrInvert->Enable(m == 6);
  static const char* help[kVrefModes] = {
      "Profondeurs rapportées au zéro des cartes du lieu (ZH du Shom en France, Chart Datum…), "
      "comme les sondes des cartes. La base les range sous le niveau moyen grâce à la surface du "
      "zéro des cartes (BathyElli en France, sinon LAT FES2022) ; sans ces fichiers, l'écart est "
      "nul et la base garde les valeurs telles quelles.",
      "Profondeurs sous le niveau moyen (GEBCO, modèles océaniques) : rangées sans conversion.",
      "Profondeurs sous la plus basse mer astronomique (EMODnet) : rangées sous le niveau moyen "
      "avec le LAT calculé par FES2022 (atlas .pdtide de l'onglet Marée requis).",
      "Hauteurs au-dessus de l'ellipsoïde (levé GNSS/RTK sans géoïde) : niveau moyen au-dessus de "
      "l'ellipsoïde par BathyElli NM en France, sinon par la surface moyenne océanique (MSS, .pdgrid).",
      "Altitudes NGF (ex. Litto3D) : profondeur ZH = profondeur + altitude du ZH, calculée à "
      "chaque sonde (RAC23 en Corse, RAF20 ailleurs).",
      "Surface du zéro des cartes au-dessus de l'ellipsoïde et géoïde de la référence du fichier.",
      "Grille donnant directement l'altitude du zéro des cartes dans la référence du fichier.",
      "Lac, retenue, plan d'eau sans marée : profondeurs gardées telles quelles, jamais converties."};
  m_vrHelp->SetLabel(U8(help[std::max(0, std::min(kVrefModes - 1, m))]));
  m_vrHelp->Wrap(FromDIP(520));
  if (m_vrRows[0]) {
    m_vrRows[0]->ShowItems(fr);
    m_vrRows[1]->ShowItems(m == 5);
    m_vrRows[2]->ShowItems(m == 5);
    m_vrRows[3]->ShowItems(m == 6);
  }
  if (m_sc) FitToContent();
}

// Paramètres de conversion verticale choisis ; false (avec message) si incomplets
bool ImportDialog::CurrentVref(VerticalRefParams& v) {
  v = VerticalRefParams();
  auto fail = [this](const wxString& m) {
    wxMessageBox(m, "PersonalDepth", wxICON_WARNING, this);
    return false;
  };
  auto exists = [](const wxString& f) { return wxFileExists(f); };
  const int m = m_vref->GetSelection();
  switch (m) {
    case 4: {  // IGN69 / IGN78C : BathyElli v2.1 « ZH/ell » + RAF20 (+ RAC23 en Corse)
      const wxString dir = m_vrFrance->GetValue().Trim().Trim(false);
      if (!wxDirExists(dir)) return fail(U8("Dossier BathyElli v2.1 introuvable :\n") + dir);
      wxArrayString zh, tac, rac;
      wxDir::GetAllFiles(dir, &zh, "*ZH*.glhi");
      if (zh.empty()) return fail(U8("Aucun fichier BathyElli « …ZH.glhi » dans ") + dir);
      v.mode = VerticalRefParams::Mode::Surfaces;
      for (auto& f : zh) v.zhFiles.push_back(f.ToUTF8().data());
      wxDir::GetAllFiles(dir, &tac, "RAF20*.tac");
      wxDir::GetAllFiles(dir, &rac, "RAC*.mnt");
      wxDir::GetAllFiles(dir, &rac, "RAC*.tac");
      if (tac.empty()) return fail(U8("RAF20.tac introuvable dans ") + dir);
      rac.Sort();  // la plus récente des grilles RAC (RAC23 après RAC09), prioritaire en Corse
      if (!rac.empty()) v.refGrid = std::string(rac.Last().ToUTF8().data()) + ";";
      v.refGrid += tac[0].ToUTF8().data();
      v.label = rac.empty() ? "IGN69 (BathyElli v2.1 + RAF20)"
                            : "IGN69/IGN78C (BathyElli v2.1 + RAF20 + " +
                                  std::string(wxFileName(rac.Last()).GetName().ToUTF8().data()) + ")";
      break;
    }
    case 5: {
      wxStringTokenizer tk(m_vrZh->GetValue(), ";");
      while (tk.HasMoreTokens()) {
        wxString f = tk.GetNextToken().Trim().Trim(false);
        if (f.empty()) continue;
        if (!exists(f)) return fail(U8("Surface ZH introuvable :\n") + f);
        v.zhFiles.push_back(f.ToUTF8().data());
      }
      wxStringTokenizer tg(m_vrGeoid->GetValue(), ";");
      wxString names;
      while (tg.HasMoreTokens()) {
        wxString f = tg.GetNextToken().Trim().Trim(false);
        if (f.empty()) continue;
        if (!exists(f)) return fail(U8("Grille de référence introuvable :\n") + f);
        if (!v.refGrid.empty()) { v.refGrid += ";"; names += "+"; }
        v.refGrid += f.ToUTF8().data();
        names += wxFileName(f).GetName();
      }
      if (v.zhFiles.empty() || v.refGrid.empty())
        return fail(U8("Indiquer la surface ZH/ellipsoïde et la grille de la référence terrestre."));
      v.mode = VerticalRefParams::Mode::Surfaces;
      v.label = std::string("Réf. ") + names.ToUTF8().data();
      break;
    }
    case 6: {
      const wxString f = m_vrDirect->GetValue().Trim().Trim(false);
      if (!exists(f)) return fail(U8("Grille d'écart introuvable :\n") + f);
      v.mode = VerticalRefParams::Mode::DirectGrid;
      v.directFile = f.ToUTF8().data();
      v.directInvert = m_vrInvert->GetValue();
      v.label = std::string("Écart ") + wxFileName(f).GetName().ToUTF8().data();
      break;
    }
    default: break;
  }
  return true;
}

// ------------------------------------------------------------- validation
// Changement d'unité de Z : décalage et bornes affichés sont convertis
void ImportDialog::OnZUnit() {
  const double f = m_zUnit->GetSelection() == 1 ? 0.3048 : 1.0;
  if (f == m_unitF) return;
  const double r = m_unitF / f;
  m_offset->SetValue(std::round(m_offset->GetValue() * r * 100.0) / 100.0);
  m_boundMin->SetValue(std::round(m_boundMin->GetValue() * r * 10.0) / 10.0);
  m_boundMax->SetValue(std::round(m_boundMax->GetValue() * r * 10.0) / 10.0);
  m_unitF = f;
  const wxString u = f == 1.0 ? "m" : "ft";
  m_offsetL->SetLabel(U8("Décalage (") + u + ")");
  m_boundsU->SetLabel(u + U8(" (profondeur ZH)"));
  Layout();
}

void ImportDialog::OnOk(wxCommandEvent&) {
  if (m_files->GetCount() == 0) return;
  std::string crsDef, crsName;
  if (!CurrentCrs(crsDef, crsName)) return;
  VerticalRefParams vref;
  if (!CurrentVref(vref)) return;

  // réglages mémorisés pour le prochain import (modification : copie locale)
  DisplaySettings& s = m_edit ? m_init : m_pi->Settings();
  const auto& cat = CrsCatalog();
  const int sel = m_crs->GetSelection();
  s.crsCode = sel == (int)cat.size() ? "CUSTOM" : (sel >= 0 ? cat[sel].code : "EPSG:4326");
  s.crsZone = m_zone->GetValue();
  s.crsSouth = m_hemi->GetSelection() == 1;
  s.crsCustom = m_crsCustom->GetValue().ToUTF8().data();
  s.readPrj = m_prj->GetValue();
  s.vrefMode = m_vref->GetSelection();
  s.vrefFranceDir = m_vrFrance->GetValue().ToUTF8().data();
  s.vrefZh = m_vrZh->GetValue().ToUTF8().data();
  s.vrefGeoid = m_vrGeoid->GetValue().ToUTF8().data();
  s.vrefDirect = m_vrDirect->GetValue().ToUTF8().data();
  s.vrefInvert = m_vrInvert->GetValue();
  s.importOrder = std::min(1, m_order->GetSelection());
  s.importFormat = m_order->GetSelection() == 2 ? m_fmt.serialize() : std::string();
  s.importSign = m_sign->GetSelection();
  // valeurs affichées dans l'unité de Z -> mètres (une valeur inchangée garde
  // sa valeur exacte, sans dérive d'arrondi)
  auto toM = [&](double ui, float stored, double p) {
    return std::fabs(ui - std::round(stored / m_unitF * p) / p) < 0.5 / p ? stored
                                                                       : (float)(ui * m_unitF);
  };
  s.importZFeet = m_zUnit->GetSelection() == 1;
  s.importOffset = toM(m_offset->GetValue(), s.importOffset, 100.0);
  s.importCache = m_cache->GetValue();
  s.importRes = m_resOn->GetValue() ? (float)m_res->GetValue() : 0.f;
  DataTypeToPrefs(m_dtype->GetSelection(), s.importInstrument, s.importNature);
  s.importInterp = m_interp->GetValue();
  s.importInterpD = m_dmaxOn->GetValue() ? (float)m_dmax->GetValue() : 0.f;
  s.importInterpMethod = m_imethod->GetSelection();
  // hauteurs d'eau observées : série lue une fois, partagée par tous les fichiers
  std::shared_ptr<const WaterLevelSeries> water;
  if (m_waterOn->GetValue()) {
    // sondes sans date : niveau du jour du levé (date imposée, sinon date
    // AAAAMMJJ du nom du fichier), pris à midi heure locale des hauteurs d'eau
    if (m_order->GetSelection() != 2 || m_fmt.colDate <= 0) {
      const bool dateOn = m_dateOn->GetValue();
      wxString missing;
      int nm = 0;
      for (unsigned k = 0; k < m_files->GetCount() && !dateOn; ++k) {
        const std::string f = m_files->GetString(k).ToUTF8().data();
        if (SurveyDateFromName(f).empty() && !(m_edit && k < m_srcs.size() && !m_srcs[k].date.empty()) &&
            nm++ < 8)
          missing += "\n  " + m_files->GetString(k);
      }
      if (nm) {
        wxMessageBox(U8("Les sondes n'ont pas d'heure : la correction par hauteurs d'eau prend le "
                        "niveau du jour du levé. Cocher « Date du levé » et la choisir, ou mettre "
                        "la date (AAAAMMJJ) dans le nom des fichiers, ou bien choisir « Format "
                        "personnalisé » avec la colonne date (et heure). Fichiers sans date :") +
                         missing,
                     "PersonalDepth", wxOK | wxICON_WARNING, this);
        return;
      }
    }
    auto ser = std::make_shared<WaterLevelSeries>();
    std::string err;
    if (!ser->load(m_water->GetValue().Trim().Trim(false).ToStdString(wxConvUTF8), m_waterFmt,
                   m_waterFeet ? 0.3048 : 1.0, err)) {
      wxMessageBox(W8(err.c_str()), "PersonalDepth", wxOK | wxICON_WARNING, this);
      return;
    }
    water = ser;
  }
  s.importWaterOn = m_waterOn->GetValue();
  s.importWater = m_water->GetValue().Trim().Trim(false).ToUTF8().data();
  s.importWaterSign = m_waterSignV;
  s.importWaterFormat = m_waterFmt.serialize();
  s.importWaterFeet = m_waterFeet;
  // NODATA : liste de nombres
  std::vector<double> nodata;
  if (m_nodataOn->GetValue()) {
    std::string txt = m_nodata->GetValue().ToUTF8().data();
    for (char& ch : txt)
      if (ch == ';' || ch == ',' || ch == '\t') ch = ' ';
    std::istringstream is(txt);
    std::string tok;
    while (is >> tok) {
      char* end = nullptr;
      const double v = std::strtod(tok.c_str(), &end);
      if (end == tok.c_str() || *end) {
        wxMessageBox(U8("Valeur NODATA illisible : « ") + W8(tok.c_str()) + U8(" »"), "PersonalDepth",
                     wxOK | wxICON_WARNING, this);
        return;
      }
      nodata.push_back(v);
    }
    if (nodata.empty()) {
      wxMessageBox(U8("Indiquer au moins une valeur NODATA, ou décocher l'option."),
                   "PersonalDepth", wxOK | wxICON_WARNING, this);
      return;
    }
  }
  if (m_boundsOn->GetValue() && !(m_boundMax->GetValue() > m_boundMin->GetValue())) {
    wxMessageBox(U8("Bornes de Z : Z1 doit être inférieur à Z2."), "PersonalDepth",
                 wxOK | wxICON_WARNING, this);
    return;
  }
  s.importNodataOn = m_nodataOn->GetValue();
  s.importNodata = m_nodata->GetValue().ToUTF8().data();
  s.importBoundsOn = m_boundsOn->GetValue();
  s.boundMin = toM(m_boundMin->GetValue(), s.boundMin, 10.0);
  s.boundMax = toM(m_boundMax->GetValue(), s.boundMax, 10.0);
  if (!m_edit) m_pi->SaveConfig();
  const std::string prefs = SerializeImportPrefs(s);
  m_prefs = prefs;
  m_date = m_dateOn->GetValue() ? std::string(m_datePick->GetValue().FormatISODate().ToUTF8().data())
                                : std::string();
  m_group = m_class->GetValue().Trim().Trim(false).ToUTF8().data();
  if (m_edit) {
    m_reread = prefs != m_initPrefs;
    if (!m_reread) { EndModal(wxID_OK); return; }  // date / classe seulement
    wxString missing;
    int nm = 0;
    for (const auto& d : m_srcs)
      if (!wxFileExists(W8(d.path.c_str())) && nm++ < 8) missing += "\n  " + W8(d.path.c_str());
    if (nm) {
      wxMessageBox(wxString::Format(U8("%d fichier(s) d'origine introuvable(s) :"), nm) + missing +
                       U8("\n\nRelecture impossible : remettre les fichiers à leur place, ou ne "
                          "changer que la date et la classe."),
                   "PersonalDepth", wxOK | wxICON_WARNING, this);
      return;
    }
  }

  m_out.clear();
  for (unsigned k = 0; k < m_files->GetCount(); ++k) {
    LayerParams p;
    p.path = m_files->GetString(k).ToUTF8().data();
    p.crsDef = crsDef;
    p.crsName = crsName;
    p.readPrj = s.readPrj || s.crsCode == "AUTO";  // automatique : .prj toujours lu
    p.vref = vref;
    p.datum = (int)VrefModeDatum(s.vrefMode);
    p.load.order = (LoadOptions::Order)s.importOrder;
    p.load.text.deserialize(s.importFormat);
    p.load.zsign = (LoadOptions::ZSign)s.importSign;
    p.load.zOffset = s.importOffset;
    p.load.zToMeters = s.importZFeet ? 0.3048 : 1.0;
    p.load.nodata = nodata;
    p.load.water = water;
    p.load.waterSubtract = s.importWaterSign == 0;
    p.depthBounds = s.importBoundsOn;
    p.depthMin = s.boundMin;
    p.depthMax = s.boundMax;
    p.useCache = s.importCache;
    p.importPrefs = prefs;
    p.storeRes = s.importRes;
    p.instrument = s.importInstrument;
    p.nature = s.importNature;
    p.interpolate = s.importInterp;
    p.interpD = s.importInterpD;
    p.interpMethod = s.importInterpMethod;
    p.surveyDate = m_date;
    if (m_edit && k < m_srcs.size()) {  // relecture : date et classe de la source gardées
      if (p.surveyDate.empty()) p.surveyDate = m_srcs[k].date;
      p.group = m_group.empty() ? m_srcs[k].group : m_group;
    }
    if (water && (m_order->GetSelection() != 2 || m_fmt.colDate <= 0)) {  // sondes sans heure : niveau du jour du levé, à midi (heure des hauteurs d'eau)
      const std::string day = !p.surveyDate.empty() ? p.surveyDate : SurveyDateFromName(p.path);
      long long t0;
      if (!day.empty() && ParseDateTime(day, "12:00", 0, t0))
        p.load.waterTime = t0 - (long long)m_waterFmt.tzMinutes * 60;
    }
    m_out.push_back(p);
  }
  EndModal(wxID_OK);
}

// Format des fichiers de hauteurs d'eau (aperçu sur le premier fichier du chemin)
void ImportDialog::OnWaterFormat() {
  const std::string sample =
      WaterLevelSeries::FirstFile(m_water->GetValue().Trim().Trim(false).ToStdString(wxConvUTF8));
  TextFormatDialog d(this, sample, m_waterFmt, m_waterSignV, m_waterFeet, true);
  if (d.ShowModal() != wxID_OK) return;
  m_waterFmt = d.Format();
  m_waterSignV = d.ZSign();
  m_waterFeet = d.ZFeet();
  UpdateWaterInfo();
}

void ImportDialog::UpdateWaterInfo() {
  wxString t = m_waterFmt.custom ? TextFormatDialog::Describe(m_waterFmt)
                                 : U8("format : reconnaissance automatique (date heure, hauteur)");
  t += W8(" ; ") + (m_waterFeet ? U8("pieds") : U8("mètres"));
  t += W8(" ; ") + (m_waterSignV == 1 ? U8("profondeur = sonde + hauteur")
                                      : U8("profondeur = sonde − hauteur"));
  m_waterInfo->SetLabel(t);
  m_waterInfo->Wrap(FromDIP(480));
  if (m_sc) FitToContent();
}

// Format personnalisé des fichiers texte (aperçu sur le premier fichier texte de la liste)
void ImportDialog::OnFormat(const wxString& preview) {
  std::string sample = preview.ToStdString(wxConvUTF8);
  for (unsigned k = 0; k < m_files->GetCount() && sample.empty(); ++k)
    if (IsTextPoints(m_files->GetString(k))) sample = m_files->GetString(k).ToStdString(wxConvUTF8);
  TextFormatDialog d(this, sample, m_fmt, m_sign->GetSelection(), m_zUnit->GetSelection() == 1);
  if (d.ShowModal() == wxID_OK) {
    m_fmt = d.Format();
    m_order->SetSelection(2);
    m_sign->SetSelection(d.ZSign());  // convention et unité de Z réglées dans le format
    if ((m_zUnit->GetSelection() == 1) != d.ZFeet()) {
      m_zUnit->SetSelection(d.ZFeet() ? 1 : 0);
      OnZUnit();  // décalage et bornes convertis
    }
  } else if (!m_fmt.custom) {
    m_order->SetSelection(0);
  }
  m_fmtInfo->SetLabel(m_order->GetSelection() == 2 ? TextFormatDialog::Describe(m_fmt) : wxString());
  if (m_sc) FitToContent();
}

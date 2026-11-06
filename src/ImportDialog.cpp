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
#include "TextFormatDialog.h"

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
  // contenu défilant (petits écrans) ; boutons toujours visibles en bas
  auto* sc = new wxScrolledWindow(this, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxVSCROLL);
  sc->SetScrollRate(0, FromDIP(12));
  auto* col = new wxBoxSizer(wxVERTICAL);
  col->Add(BuildFiles(sc), 1, wxEXPAND | wxALL, 6);
  col->Add(BuildReading(sc), 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, 6);
  col->Add(BuildCrs(sc), 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, 6);
  col->Add(BuildVref(sc), 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, 6);

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
  dr->Add(m_datePick, 0);
  col->Add(dr, 0, wxLEFT | wxRIGHT | wxBOTTOM, 10);

  // Classement dans la base
  auto* cr = new wxBoxSizer(wxHORIZONTAL);
  m_class = new wxComboBox(sc, wxID_ANY, "", wxDefaultPosition, wxDefaultSize, 0, nullptr,
                           wxCB_DROPDOWN);
  std::set<std::string> seen;
  for (const auto& d : m_pi->DbSources())
    if (seen.insert(d.group).second) m_class->Append(W8(d.group.c_str()));
  m_class->SetHint(m_edit ? U8("vide : classe actuelle de chaque source")
                          : U8("vide : déduite du nom de chaque fichier (LITTO3D, PFE…)"));
  cr->Add(new wxStaticText(sc, wxID_ANY, U8("Classe")), 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 6);
  cr->Add(m_class, 1);
  col->Add(cr, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, 10);
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

  // taille : tout le contenu si l'écran le permet, sinon 90 % de sa hauteur
  // avec barre de défilement
  SetSizer(top);
  const wxSize want = col->GetMinSize();
  const wxRect area = wxDisplay(wxDisplay::GetFromWindow(parent) == wxNOT_FOUND
                                    ? 0u : (unsigned)wxDisplay::GetFromWindow(parent)).GetClientArea();
  const int btnH = btns->GetMinSize().y + FromDIP(16);
  const int h = std::min(want.y + btnH + FromDIP(40), area.height * 9 / 10);
  const int w = std::min(std::max(FromDIP(560), want.x + FromDIP(30)), area.width * 9 / 10);
  sc->SetMinSize(wxSize(-1, FromDIP(200)));
  SetSize(w, h);
  UpdateSummary();
  CentreOnParent();
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
wxSizer* ImportDialog::BuildReading(wxWindow* p) {
  auto* box = new wxStaticBoxSizer(wxVERTICAL, p, U8("Lecture des valeurs"));
  wxWindow* sb = box->GetStaticBox();
  const DisplaySettings& st = m_init;
  wxString orders[] = {U8("X Y Z (lon lat z / Est Nord z)"), U8("Y X Z (lat lon z / Nord Est z)"),
                       U8("Format personnalisé (colonnes choisies)")};
  wxString signs[] = {U8("Automatique (majorité négative = altitudes)"),
                      U8("Profondeurs positives vers le bas"),
                      U8("Altitudes (fond négatif, Z positif vers le haut)")};
  m_order = new wxChoice(sb, wxID_ANY, wxDefaultPosition, wxDefaultSize, 3, orders);
  m_sign = new wxChoice(sb, wxID_ANY, wxDefaultPosition, wxDefaultSize, 3, signs);
  m_fmt.deserialize(st.importFormat);
  m_order->SetSelection(m_fmt.custom ? 2 : std::max(0, std::min(1, st.importOrder)));
  m_order->SetToolTip(U8("Fichiers texte (.xyz .txt .csv .dat) : les trois premiers nombres de chaque "
                         "ligne, ou un format personnalisé (lignes d'entête, séparateurs, colonnes X, "
                         "Y, Z, date et heure) réglé avec « Format… »."));
  m_fmtBtn = new wxButton(sb, wxID_ANY, U8("Format…"), wxDefaultPosition, wxDefaultSize, wxBU_EXACTFIT);
  m_fmtInfo = new wxStaticText(sb, wxID_ANY, TextFormatDialog::Describe(m_fmt));
  m_fmtInfo->SetForegroundColour(wxColour(90, 90, 90));
  m_fmtBtn->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { OnFormat(); });
  m_order->Bind(wxEVT_CHOICE, [this](wxCommandEvent&) {
    if (m_order->GetSelection() == 2 && !m_fmt.custom) OnFormat();
    m_fmtInfo->SetLabel(m_order->GetSelection() == 2 ? TextFormatDialog::Describe(m_fmt) : wxString());
    Layout();
  });
  m_sign->SetSelection(std::max(0, std::min(2, st.importSign)));
  m_sign->SetToolTip(U8("Sens des valeurs Z du fichier. Les rasters (.asc, .tif) suivent la même règle."));
  // unité verticale des valeurs du fichier ; décalage et bornes s'expriment
  // dans cette unité, la base reste en mètres
  m_unitF = st.importZFeet ? 0.3048 : 1.0;
  wxString units[] = {U8("Mètres"), U8("Pieds")};
  m_zUnit = new wxChoice(sb, wxID_ANY, wxDefaultPosition, wxDefaultSize, 2, units);
  m_zUnit->SetSelection(st.importZFeet ? 1 : 0);
  m_zUnit->SetToolTip(U8("Unité des valeurs Z du fichier. Les profondeurs sont converties en "
                         "mètres dans la base ; l'affichage suit ensuite l'unité de profondeur "
                         "choisie dans OpenCPN (Options › Affichage › Unités)."));
  m_offset = new wxSpinCtrlDouble(sb, wxID_ANY, "", wxDefaultPosition, wxSize(FromDIP(90), -1),
                                  wxSP_ARROW_KEYS, -400, 400, st.importOffset / m_unitF, 0.01);
  m_offset->SetDigits(2);
  m_offset->SetToolTip(U8("Décalage constant ajouté aux profondeurs (dans l'unité de Z), "
                          "en plus de la transformation verticale ci-dessous"));
  m_cache = new wxCheckBox(sb, wxID_ANY, U8("Cache binaire (.pdpc) à côté des fichiers (réimport rapide)"));
  m_cache->SetValue(st.importCache);
  auto* g = new wxFlexGridSizer(2, 4, 8);
  g->AddGrowableCol(1);
  auto L = [&](const char* t) { return new wxStaticText(sb, wxID_ANY, U8(t)); };
  {
    auto* orow = new wxBoxSizer(wxHORIZONTAL);
    orow->Add(m_order, 1, wxEXPAND | wxRIGHT, 4);
    orow->Add(m_fmtBtn, 0);
    auto* ocol = new wxBoxSizer(wxVERTICAL);
    ocol->Add(orow, 0, wxEXPAND);
    ocol->Add(m_fmtInfo, 0, wxTOP, 2);
    g->Add(L("Colonnes"), 0, wxALIGN_CENTER_VERTICAL); g->Add(ocol, 0, wxEXPAND);
  }
  g->Add(L("Sens de Z"), 0, wxALIGN_CENTER_VERTICAL); g->Add(m_sign, 0, wxEXPAND);
  g->Add(L("Unité de Z"), 0, wxALIGN_CENTER_VERTICAL); g->Add(m_zUnit, 0);
  m_offsetL = L("Décalage (m)");
  g->Add(m_offsetL, 0, wxALIGN_CENTER_VERTICAL); g->Add(m_offset, 0);
  // valeurs absentes : NODATA de l'utilisateur (Z brut du fichier)
  wxString nds[] = {"-9999", "9999", "-99999", "99999", "-9999 9999", "-99999 99999",
                    "-9999 9999 -99999 99999", "999 -999"};
  m_nodataOn = new wxCheckBox(sb, wxID_ANY, U8("NODATA"));
  m_nodataOn->SetValue(st.importNodataOn);
  m_nodata = new wxComboBox(sb, wxID_ANY, W8(st.importNodata.c_str()), wxDefaultPosition,
                            wxDefaultSize, 8, nds, wxCB_DROPDOWN);
  const wxString nt = U8("Valeurs Z du fichier à traiter comme absentes, séparées par des "
                         "espaces ou des « ; » (ex. -9999 9999 -99999 99999). Elles s'ajoutent "
                         "au NODATA déclaré dans l'entête des grilles ASC / GeoTIFF. Comparées "
                         "aux valeurs brutes, avant changement de signe et transformation.");
  m_nodataOn->SetToolTip(nt);
  m_nodata->SetToolTip(nt);
  g->Add(m_nodataOn, 0, wxALIGN_CENTER_VERTICAL); g->Add(m_nodata, 0, wxEXPAND);

  // bornes d'import : profondeur ZH finale (mêmes bornes que « Nettoyer »)
  m_boundsOn = new wxCheckBox(sb, wxID_ANY, U8("Bornes de Z"));
  m_boundsOn->SetValue(st.importBoundsOn);
  m_boundMin = new wxSpinCtrlDouble(sb, wxID_ANY, "", wxDefaultPosition, wxSize(FromDIP(90), -1),
                                    wxSP_ARROW_KEYS, -40000, 40000, st.boundMin / m_unitF, 1);
  m_boundMax = new wxSpinCtrlDouble(sb, wxID_ANY, "", wxDefaultPosition, wxSize(FromDIP(90), -1),
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

  // résolution de stockage (pas horizontal des tuiles) : estimée ou imposée
  m_resOn = new wxCheckBox(sb, wxID_ANY, U8("Résolution imposée"));
  m_resOn->SetValue(st.importRes > 0);
  m_res = new wxSpinCtrlDouble(sb, wxID_ANY, "", wxDefaultPosition, wxSize(FromDIP(90), -1),
                               wxSP_ARROW_KEYS, 0.05, 5000, st.importRes > 0 ? st.importRes : 5.0, 0.5);
  m_res->SetDigits(2);
  const wxString rt = U8("Pas horizontal de stockage des sondes dans la base. Sinon estimé : pas "
                         "de la grille, ou espacement des sondes consécutives (lignes de "
                         "sonde), ou surface / nombre de sondes. Une valeur plus grande donne une "
                         "surface plus continue entre des lignes de sonde espacées ; dans chaque "
                         "case, la sonde la plus faible est gardée.");
  m_resOn->SetToolTip(rt);
  m_res->SetToolTip(rt);
  auto* resr = new wxBoxSizer(wxHORIZONTAL);
  resr->Add(m_res, 0, wxRIGHT, 4);
  resr->Add(L("m (automatique si décoché)"), 0, wxALIGN_CENTER_VERTICAL);
  g->Add(m_resOn, 0, wxALIGN_CENTER_VERTICAL); g->Add(resr, 0);
  auto sync = [this]() {
    m_nodata->Enable(m_nodataOn->GetValue());
    m_boundMin->Enable(m_boundsOn->GetValue());
    m_boundMax->Enable(m_boundsOn->GetValue());
    m_res->Enable(m_resOn->GetValue());
  };
  m_resOn->Bind(wxEVT_CHECKBOX, [sync](wxCommandEvent&) { sync(); });
  m_nodataOn->Bind(wxEVT_CHECKBOX, [sync](wxCommandEvent&) { sync(); });
  m_zUnit->Bind(wxEVT_CHOICE, [this](wxCommandEvent&) { OnZUnit(); });
  m_boundsOn->Bind(wxEVT_CHECKBOX, [sync](wxCommandEvent&) { sync(); });
  sync();
  {  // libellés d'unité
    const wxString u = m_unitF == 1.0 ? "m" : "ft";
    m_offsetL->SetLabel(U8("Décalage (") + u + ")");
    m_boundsU->SetLabel(u + U8(" (profondeur ZH)"));
  }
  box->Add(g, 0, wxEXPAND | wxALL, 4);
  box->Add(m_cache, 0, wxLEFT | wxRIGHT | wxBOTTOM, 4);
  return box;
}

// ------------------------------------------------------------- coordonnées
wxSizer* ImportDialog::BuildCrs(wxWindow* p) {
  auto* box = new wxStaticBoxSizer(wxVERTICAL, p, U8("Système de coordonnées (X, Y)"));
  wxWindow* sb = box->GetStaticBox();
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
  auto* g = new wxFlexGridSizer(2, 4, 8);
  g->AddGrowableCol(1);
  auto L = [&](const char* t) { return new wxStaticText(sb, wxID_ANY, U8(t)); };
  g->Add(L("Système"), 0, wxALIGN_CENTER_VERTICAL); g->Add(m_crs, 0, wxEXPAND);
  g->Add(L("Fuseau UTM"), 0, wxALIGN_CENTER_VERTICAL); g->Add(zoneRow, 0);
  g->Add(L("EPSG / PROJ"), 0, wxALIGN_CENTER_VERTICAL); g->Add(m_crsCustom, 0, wxEXPAND);
  box->Add(g, 0, wxEXPAND | wxALL, 4);
  box->Add(m_prj, 0, wxLEFT | wxRIGHT | wxBOTTOM, 4);

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
  auto* box = new wxStaticBoxSizer(wxVERTICAL, p, U8("Référence verticale des Z et transformation vers le ZH"));
  wxWindow* sb = box->GetStaticBox();
  const DisplaySettings& st = m_init;
  wxString vrefs[] = {
      U8("Zéro hydrographique — aucune transformation"),
      U8("IGN69 (continent) / IGN78C (Corse) → ZH : BathyElli v2.1 + RAF20 + RAC23"),
      U8("Hauteurs ellipsoïdales (RGF93 / WGS84, GNSS) → ZH : BathyElli v2.1"),
      U8("Autre référence terrestre → ZH : surface ZH/ellipsoïde + géoïde (fichiers)"),
      U8("Autre → ZH : grille d'écart référence / ZH (fichier)")};
  m_vref = new wxChoice(sb, wxID_ANY, wxDefaultPosition, wxDefaultSize, 5, vrefs);
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
  auto* g = new wxFlexGridSizer(2, 4, 8);
  g->AddGrowableCol(1);
  auto L = [&](const char* t) { return new wxStaticText(sb, wxID_ANY, U8(t)); };
  g->Add(L("Z du fichier"), 0, wxALIGN_CENTER_VERTICAL); g->Add(m_vref, 0, wxEXPAND);
  g->Add(L("Dossier France"), 0, wxALIGN_CENTER_VERTICAL); g->Add(rFr, 0, wxEXPAND);
  g->Add(L("Surface(s) ZH"), 0, wxALIGN_CENTER_VERTICAL); g->Add(rZh, 0, wxEXPAND);
  g->Add(L("Géoïde(s)"), 0, wxALIGN_CENTER_VERTICAL); g->Add(rGe, 0, wxEXPAND);
  g->Add(L("Grille d'écart"), 0, wxALIGN_CENTER_VERTICAL); g->Add(rDi, 0, wxEXPAND);
  box->Add(g, 0, wxEXPAND | wxALL, 4);
  box->Add(m_vrHelp, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, 4);

  m_vref->SetSelection(std::max(0, std::min(4, st.vrefMode)));
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
  const bool fr = m == 1 || m == 2;
  m_vrFrance->Enable(fr); m_vrFranceBtn->Enable(fr);
  m_vrZh->Enable(m == 3); m_vrZhBtn->Enable(m == 3);
  m_vrGeoid->Enable(m == 3); m_vrGeoidBtn->Enable(m == 3);
  m_vrDirect->Enable(m == 4); m_vrDirectBtn->Enable(m == 4); m_vrInvert->Enable(m == 4);
  static const char* help[] = {
      "Les profondeurs sont déjà rapportées au zéro hydrographique (cartes, levés Shom).",
      "Altitudes NGF (ex. Litto3D) : profondeur ZH = profondeur + altitude du ZH, calculée à "
      "chaque sonde (RAC23 en Corse, RAF20 ailleurs).",
      "Hauteurs au-dessus de l'ellipsoïde (levé GNSS/RTK sans géoïde) : profondeur ZH = "
      "profondeur + hauteur ellipsoïdale du ZH (BathyElli).",
      "Hors France : surface du ZH au-dessus de l'ellipsoïde et géoïde de la référence du fichier.",
      "Grille donnant directement l'altitude du ZH dans la référence du fichier."};
  m_vrHelp->SetLabel(U8(help[std::max(0, std::min(4, m))]));
  m_vrHelp->Wrap(FromDIP(520));
  Layout();
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
    case 1:    // IGN69 / IGN78C : BathyElli v2.1 « ZH/ell » + RAF20 (+ RAC23 en Corse)
    case 2: {  // hauteurs ellipsoïdales : BathyElli seul
      const wxString dir = m_vrFrance->GetValue().Trim().Trim(false);
      if (!wxDirExists(dir)) return fail(U8("Dossier BathyElli v2.1 introuvable :\n") + dir);
      wxArrayString zh, tac, rac;
      wxDir::GetAllFiles(dir, &zh, "*ZH*.glhi");
      if (zh.empty()) return fail(U8("Aucun fichier BathyElli « …ZH.glhi » dans ") + dir);
      v.mode = VerticalRefParams::Mode::Surfaces;
      for (auto& f : zh) v.zhFiles.push_back(f.ToUTF8().data());
      if (m == 2) {
        v.label = "Ellipsoïde (BathyElli v2.1)";
        break;
      }
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
    case 3: {
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
    case 4: {
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
    p.load.order = (LoadOptions::Order)s.importOrder;
    p.load.text.deserialize(s.importFormat);
    p.load.zsign = (LoadOptions::ZSign)s.importSign;
    p.load.zOffset = s.importOffset;
    p.load.zToMeters = s.importZFeet ? 0.3048 : 1.0;
    p.load.nodata = nodata;
    p.depthBounds = s.importBoundsOn;
    p.depthMin = s.boundMin;
    p.depthMax = s.boundMax;
    p.useCache = s.importCache;
    p.importPrefs = prefs;
    p.storeRes = s.importRes;
    p.surveyDate = m_date;
    if (m_edit && k < m_srcs.size()) {  // relecture : date et classe de la source gardées
      if (p.surveyDate.empty()) p.surveyDate = m_srcs[k].date;
      p.group = m_group.empty() ? m_srcs[k].group : m_group;
    }
    m_out.push_back(p);
  }
  EndModal(wxID_OK);
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
  Layout();
}

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
#include "DataDialog.h"

#include <wx/spinctrl.h>
#include <wx/statline.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <map>

#include "ImportDialog.h"
#include "PersonalDepth_pi.h"

#define U8(s) wxString::FromUTF8(pd::tr(s))
#define W8(s) wxString::FromUTF8(s)  // données : pas de traduction

using namespace pd;

namespace {
// Donnée attachée aux lignes de l'arbre (0 = ligne de classe)
struct SourceItem : public wxClientData {
  explicit SourceItem(int64_t i) : id(i) {}
  int64_t id;
};

wxString Bytes(long long b) {
  if (b >= (1LL << 30)) return wxString::Format(U8("%.2f Go"), b / double(1LL << 30));
  if (b >= (1LL << 20)) return wxString::Format(U8("%.0f Mo"), b / double(1LL << 20));
  return wxString::Format(U8("%.0f ko"), b / 1024.0);
}

wxString Count(long long n) {
  if (n >= 1000000) return wxString::Format("%.1f M", n / 1e6);
  if (n >= 10000) return wxString::Format("%.0f k", n / 1e3);
  return wxString::Format("%lld", n);
}

// chemin comparable (Windows : casse et séparateurs indifférents)
std::string Norm(std::string p) {
  for (char& c : p) c = c == '\\' ? '/' : (char)std::tolower((unsigned char)c);
  return p;
}
std::string FileOf(const std::string& p) {
  const std::string n = Norm(p);
  const size_t k = n.find_last_of('/');
  return k == std::string::npos ? n : n.substr(k + 1);
}

wxString CellSizeText(double deg) {
  const int min = (int)std::lround(deg * 60);
  if (min % 60 == 0) return wxString::Format(U8("%d°"), min / 60);
  return wxString::Format(U8("%g° (%d′)"), deg, min);
}
}  // namespace

DataDialog::DataDialog(wxWindow* parent, PersonalDepth_pi* pi)
    : wxDialog(parent, wxID_ANY, U8("PersonalDepth — gestion des données"), wxDefaultPosition,
               wxDefaultSize, wxDEFAULT_DIALOG_STYLE | wxRESIZE_BORDER),
      m_pi(pi) {
  auto* col = new wxBoxSizer(wxVERTICAL);

  // règle de priorité et découpage des fichiers
  auto* g = new wxFlexGridSizer(2, 6, 8);
  g->AddGrowableCol(1, 1);
  wxString prios[] = {U8("Meilleure résolution, puis plus récente"), U8("Sonde la plus faible"),
                      U8("Levé le plus récent"), U8("Ordre de la liste (rang manuel)")};
  m_prio = new wxChoice(this, wxID_ANY, wxDefaultPosition, wxDefaultSize, 4, prios);
  m_prio->SetToolTip(U8("Dans les recouvrements, chaque pixel prend la valeur de la source "
                        "prioritaire (pas de moyenne). Changer la règle refusionne la base."));
  g->Add(new wxStaticText(this, wxID_ANY, U8("Priorité")), 0, wxALIGN_CENTER_VERTICAL);
  g->Add(m_prio, 0, wxEXPAND);
  auto* sr = new wxBoxSizer(wxHORIZONTAL);
  m_split = new wxStaticText(this, wxID_ANY, "");
  m_splitBtn = new wxButton(this, wxID_ANY, U8("Redécouper…"));
  m_splitBtn->SetToolTip(U8("Changer la taille des cellules géographiques des fichiers .pdb "
                            "(fichiers plus petits avec des cellules plus petites)"));
  sr->Add(m_split, 1, wxALIGN_CENTER_VERTICAL | wxRIGHT, 6);
  sr->Add(m_splitBtn, 0);
  g->Add(new wxStaticText(this, wxID_ANY, U8("Fichiers .pdb")), 0, wxALIGN_CENTER_VERTICAL);
  g->Add(sr, 0, wxEXPAND);
  col->Add(g, 0, wxEXPAND | wxALL, 8);

  // sources importées
  m_tree = new wxTreeListCtrl(this, wxID_ANY, wxDefaultPosition, wxSize(-1, FromDIP(260)),
                              wxTL_MULTIPLE | wxTL_CHECKBOX | wxTL_3STATE);
  m_tree->AppendColumn(U8("Source"), FromDIP(230));
  m_tree->AppendColumn(U8("Date du levé"), FromDIP(78));
  m_tree->AppendColumn(U8("Rés. (m)"), FromDIP(56), wxALIGN_RIGHT);
  m_tree->AppendColumn(U8("Sondes"), FromDIP(60), wxALIGN_RIGHT);
  m_tree->AppendColumn(U8("Rang"), FromDIP(42), wxALIGN_RIGHT);
  m_tree->AppendColumn(U8("Z min"), FromDIP(56), wxALIGN_RIGHT);
  m_tree->AppendColumn(U8("Z max"), FromDIP(56), wxALIGN_RIGHT);
  m_tree->AppendColumn(U8("Réf. verticale"), FromDIP(140));
  m_tree->AppendColumn(U8("Système"), FromDIP(90));
  m_tree->AppendColumn(U8("Unité Z"), FromDIP(52));
  m_tree->AppendColumn(U8("Importée le"), FromDIP(78));
  m_tree->SetToolTip(U8("Case : utiliser ou non une source (ou toute une classe) dans la fusion. "
                        "Maj ou Ctrl + clic : sélection multiple ; une classe sélectionnée "
                        "compte pour toutes ses sources."));
  col->Add(m_tree, 1, wxEXPAND | wxLEFT | wxRIGHT, 8);

  m_details = new wxTextCtrl(this, wxID_ANY, "", wxDefaultPosition, wxSize(-1, FromDIP(120)),
                             wxTE_MULTILINE | wxTE_READONLY | wxTE_DONTWRAP);
  col->Add(m_details, 0, wxEXPAND | wxALL, 8);

  auto* r3 = new wxBoxSizer(wxHORIZONTAL);
  m_import = new wxButton(this, wxID_ANY, U8("Importer…"));
  m_import->SetToolTip(U8("Fichiers, colonnes, NODATA, bornes, système de coordonnées, "
                          "référence verticale et transformation vers le ZH, classe."));
  m_up = new wxButton(this, wxID_ANY, U8("▲"), wxDefaultPosition, wxSize(FromDIP(30), -1));
  m_down = new wxButton(this, wxID_ANY, U8("▼"), wxDefaultPosition, wxSize(FromDIP(30), -1));
  m_up->SetToolTip(U8("Monter la sélection dans l'ordre des rangs (règle « rang manuel »)"));
  m_down->SetToolTip(U8("Descendre la sélection dans l'ordre des rangs"));
  m_del = new wxButton(this, wxID_ANY, U8("Retirer"));
  m_clean = new wxButton(this, wxID_ANY, U8("Nettoyer…"));
  m_clean->SetToolTip(U8("Retirer des sources sélectionnées les sondes hors des bornes Z1 – Z2, "
                         "puis refusionner"));
  m_edit = new wxButton(this, wxID_ANY, U8("Modifier…"));
  m_edit->SetToolTip(U8("Date du levé, classe, unité verticale, système de coordonnées, "
                        "référence verticale des sources sélectionnées (double-clic sur une "
                        "source : idem)"));
  m_zoom = new wxButton(this, wxID_ANY, U8("Centrer"));
  m_zoom->SetToolTip(U8("Centrer la carte sur les sources sélectionnées"));
  r3->Add(m_import, 0, wxRIGHT, 8);
  r3->Add(m_up, 0);
  r3->Add(m_down, 0, wxRIGHT, 8);
  r3->Add(m_edit, 0, wxRIGHT, 4);
  r3->Add(m_del, 0, wxRIGHT, 4);
  r3->Add(m_clean, 0, wxRIGHT, 4);
  r3->Add(m_zoom, 0);
  col->Add(r3, 0, wxLEFT | wxRIGHT | wxBOTTOM, 8);

  auto* r4 = new wxBoxSizer(wxHORIZONTAL);
  m_class = new wxComboBox(this, wxID_ANY, "", wxDefaultPosition, wxDefaultSize, 0, nullptr, wxCB_DROPDOWN);
  m_class->SetHint(U8("classe à donner aux sources sélectionnées"));
  m_setClass = new wxButton(this, wxID_ANY, U8("Classer la sélection"));
  r4->Add(new wxStaticText(this, wxID_ANY, U8("Classe")), 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 4);
  r4->Add(m_class, 1, wxRIGHT, 4);
  r4->Add(m_setClass, 0);
  col->Add(r4, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, 8);

  col->Add(new wxStaticLine(this), 0, wxEXPAND | wxLEFT | wxRIGHT, 8);
  auto* r5 = new wxBoxSizer(wxHORIZONTAL);
  auto* sc = new wxBoxSizer(wxVERTICAL);
  m_status = new wxStaticText(this, wxID_ANY, "");
  m_gauge = new wxGauge(this, wxID_ANY, 1000, wxDefaultPosition, wxSize(-1, FromDIP(8)));
  sc->Add(m_status, 0, wxEXPAND);
  sc->Add(m_gauge, 0, wxEXPAND | wxTOP, 2);
  m_stop = new wxButton(this, wxID_ANY, U8("Arrêter"));
  m_stop->SetToolTip(U8("Arrêter l'opération en cours. Pour un import, les fichiers déjà "
                        "terminés restent dans la base ; le fichier en cours est annulé."));
  r5->Add(sc, 1, wxALIGN_CENTER_VERTICAL | wxRIGHT, 8);
  r5->Add(m_stop, 0, wxALIGN_CENTER_VERTICAL);
  col->Add(r5, 0, wxEXPAND | wxALL, 8);
  auto* close = new wxButton(this, wxID_CLOSE, U8("Fermer"));
  col->Add(close, 0, wxALIGN_RIGHT | wxRIGHT | wxBOTTOM, 8);
  SetSizer(col);
  SetSize(FromDIP(wxSize(900, 640)));
  SetMinSize(FromDIP(wxSize(560, 420)));

  // --- événements
  close->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { Hide(); });
  Bind(wxEVT_CLOSE_WINDOW, [this](wxCloseEvent&) { Hide(); });
  m_prio->Bind(wxEVT_CHOICE, [this](wxCommandEvent&) {
    if (m_pi->DbBusy()) { m_prio->SetSelection(m_pi->Settings().dbPriority); return; }
    m_pi->DbSetPriority(m_prio->GetSelection());
    UpdateButtons();
  });
  m_splitBtn->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { OnResplit(); });
  m_import->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { OnImport(); });
  m_clean->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { OnClean(); });
  m_edit->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { OnEdit(); });
  m_tree->Bind(wxEVT_TREELIST_ITEM_ACTIVATED, [this](wxTreeListEvent&) {
    if (!m_pi->DbBusy() && !SelectedSources().empty()) OnEdit();
  });
  m_stop->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) {
    m_pi->DbStop();
    m_stop->Disable();
  });
  m_zoom->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { m_pi->DbZoomTo(SelectedSources()); });
  m_up->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { m_pi->DbMove(SelectedSources(), -1); UpdateButtons(); });
  m_down->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { m_pi->DbMove(SelectedSources(), +1); UpdateButtons(); });
  m_del->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) {
    const auto ids = SelectedSources();
    if (ids.empty()) return;
    const wxString q = ids.size() == 1
        ? U8("Retirer cette source de la base ?")
        : wxString::Format(U8("Retirer ces %d sources de la base ?"), (int)ids.size());
    if (wxMessageBox(q + U8("\nLeurs sondes seront supprimées de la base (les fichiers d'origine "
                            "ne sont pas touchés)."),
                     "PersonalDepth", wxYES_NO | wxICON_QUESTION, this) != wxYES)
      return;
    m_pi->DbRemove(ids);
    UpdateButtons();
  });
  m_setClass->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) {
    const std::string g = m_class->GetValue().Trim().Trim(false).ToUTF8().data();
    if (g.empty()) {
      wxMessageBox(U8("Saisir ou choisir une classe."), "PersonalDepth", wxICON_INFORMATION, this);
      return;
    }
    m_pi->DbSetGroup(SelectedSources(), g);
    UpdateButtons();
  });
  m_tree->Bind(wxEVT_TREELIST_SELECTION_CHANGED, [this](wxTreeListEvent&) {
    UpdateButtons();
    UpdateDetails();
  });
  m_tree->Bind(wxEVT_TREELIST_ITEM_CHECKED, [this](wxTreeListEvent& e) {
    const wxTreeListItem it = e.GetItem();
    if (m_pi->DbBusy()) { RefreshDb(); return; }  // opération en cours : état rétabli
    const bool on = m_tree->GetCheckedState(it) == wxCHK_CHECKED;
    std::vector<int64_t> ids;
    auto* d = static_cast<SourceItem*>(m_tree->GetItemData(it));
    if (d && d->id) {
      ids.push_back(d->id);
      m_tree->UpdateItemParentStateRecursively(it);
    } else {  // classe : toutes ses sources
      m_tree->CheckItemRecursively(it, on ? wxCHK_CHECKED : wxCHK_UNCHECKED);
      for (wxTreeListItem c = m_tree->GetFirstChild(it); c.IsOk(); c = m_tree->GetNextSibling(c))
        if (auto* cd = static_cast<SourceItem*>(m_tree->GetItemData(c))) ids.push_back(cd->id);
    }
    m_pi->DbSetVisible(ids, on);
    UpdateButtons();
  });
  RefreshDb();
}

// ------------------------------------------------------------------ import
void DataDialog::OnImport() {
  if (m_pi->Settings().dbFolder.empty()) {
    wxMessageBox(U8("Choisir d'abord le dossier de la base (panneau PersonalDepth)."),
                 "PersonalDepth", wxICON_INFORMATION, this);
    return;
  }
  ImportDialog dlg(this, m_pi);
  if (dlg.ShowModal() != wxID_OK || dlg.Params().empty()) return;
  std::vector<LayerParams> files = dlg.Params();

  // fichiers déjà présents dans la base (même chemin, ou même nom de fichier)
  std::map<std::string, std::vector<int64_t>> byPath, byName;
  for (const auto& d : m_pi->DbSources()) {
    byPath[Norm(d.path)].push_back(d.id);
    byName[FileOf(d.path.empty() ? d.name : d.path)].push_back(d.id);
  }
  std::vector<std::vector<int64_t>> old(files.size());
  size_t present = 0;
  for (size_t i = 0; i < files.size(); ++i) {
    auto it = byPath.find(Norm(files[i].path));
    if (it != byPath.end()) old[i] = it->second;
    else {
      auto jt = byName.find(FileOf(files[i].path));
      if (jt != byName.end()) old[i] = jt->second;
    }
    if (!old[i].empty()) ++present;
  }
  std::vector<std::vector<int64_t>> replace;
  if (present) {
    wxString list;
    size_t shown = 0;
    for (size_t i = 0; i < files.size() && shown < 8; ++i)
      if (!old[i].empty()) {
        list += "  • " + W8(FileOf(files[i].path).c_str()) + "\n";
        ++shown;
      }
    if (present > shown) list += wxString::Format(U8("  … et %d autre(s)\n"), (int)(present - shown));
    wxMessageDialog q(this,
                      wxString::Format(U8("%d fichier(s) sur %d sont déjà dans la base :\n\n"),
                                       (int)present, (int)files.size()) +
                          list +
                          U8("\nImporter seulement les nouveaux fichiers, ou tout réimporter ? "
                             "Au réimport, la version déjà présente de chaque fichier est "
                             "remplacée par la nouvelle."),
                      U8("Fichiers déjà importés"), wxYES_NO | wxCANCEL | wxICON_QUESTION);
    q.SetYesNoCancelLabels(
        wxString::Format(U8("Seulement les nouveaux (%d)"), (int)(files.size() - present)),
        U8("Tout réimporter (remplacer)"), U8("Annuler"));
    const int r = q.ShowModal();
    if (r == wxID_CANCEL) return;
    if (r == wxID_YES) {  // nouveaux seulement
      std::vector<LayerParams> keep;
      for (size_t i = 0; i < files.size(); ++i)
        if (old[i].empty()) keep.push_back(files[i]);
      files.swap(keep);
      if (files.empty()) {
        wxMessageBox(U8("Tous ces fichiers sont déjà dans la base."), "PersonalDepth",
                     wxICON_INFORMATION, this);
        return;
      }
    } else {
      replace = old;
    }
  }
  m_pi->DbImport(files, dlg.Group(), replace);
  UpdateButtons();
}

// ------------------------------------------------------------ modification
void DataDialog::OnEdit() {
  const auto ids = SelectedSources();
  if (ids.empty()) return;
  std::vector<DbSource> srcs;
  for (int64_t id : ids)
    for (const auto& d : m_pi->DbSources())
      if (d.id == id) srcs.push_back(d);
  if (srcs.empty()) return;
  ImportDialog dlg(this, m_pi, &srcs);
  if (dlg.ShowModal() != wxID_OK) return;
  if (!dlg.NeedsReread()) {  // date et/ou classe : rapide, sans relire les fichiers
    std::string date = dlg.SurveyDate();
    bool same = true;
    for (const auto& d : srcs) same = same && d.date == date;
    if (same) date.clear();
    // sources importées avant l'enregistrement des réglages : ceux affichés
    // (validés par « Appliquer ») sont enregistrés, sans relecture
    bool unknown = false;
    for (const auto& d : srcs) unknown = unknown || d.params.empty();
    const std::string params = unknown ? dlg.Prefs() : std::string();
    if (!date.empty() || !dlg.Group().empty() || !params.empty())
      m_pi->DbEditMeta(ids, date, dlg.Group(), params);
    UpdateButtons();
    return;
  }
  if (wxMessageBox(wxString::Format(U8("Les réglages de lecture ont changé : les %d fichier(s) "
                                       "d'origine vont être relus et convertis à nouveau "
                                       "(durée comparable à l'import), puis remplacer les "
                                       "sources actuelles.\n\nContinuer ?"),
                                    (int)srcs.size()),
                   "PersonalDepth", wxYES_NO | wxICON_QUESTION, this) != wxYES)
    return;
  std::vector<std::vector<int64_t>> replace;
  for (const auto& d : srcs) replace.push_back({d.id});
  m_pi->DbImport(dlg.Params(), "", replace);
  UpdateButtons();
}

// ---------------------------------------------------------------- nettoyage
void DataDialog::OnClean() {
  // profondeurs affichées dans l'unité d'OpenCPN (m ou ft), stockées en m
  const float dk = m_pi->Settings().depthK > 0 ? m_pi->Settings().depthK : 1.f;
  const wxString du = m_pi->Settings().depthUnit;
  (void)du;
  const auto ids = SelectedSources();
  if (ids.empty()) return;
  wxDialog d(this, wxID_ANY, U8("Nettoyer les sources"));
  auto* col = new wxBoxSizer(wxVERTICAL);
  auto* t = new wxStaticText(
      &d, wxID_ANY,
      wxString::Format(U8("%d source(s) sélectionnée(s). Les sondes dont la profondeur (ZH, m, "
                          "positive vers le bas) sort des bornes Z1 – Z2 sont supprimées de la "
                          "base, puis les pyramides et la fusion sont reconstruites. Les "
                          "fichiers d'origine ne sont pas touchés.\n\nCe sont les bornes de "
                          "la fenêtre d'import : les valeurs choisies ici y sont reprises."),
                       (int)ids.size()));
  t->Wrap(d.FromDIP(380));
  col->Add(t, 0, wxALL, 10);
  auto* r = new wxBoxSizer(wxHORIZONTAL);
  auto* lo = new wxSpinCtrlDouble(&d, wxID_ANY, "", wxDefaultPosition, wxSize(d.FromDIP(90), -1),
                                  wxSP_ARROW_KEYS, -12000 * dk, 12000 * dk, m_pi->Settings().boundMin * dk, 1);
  auto* hi = new wxSpinCtrlDouble(&d, wxID_ANY, "", wxDefaultPosition, wxSize(d.FromDIP(90), -1),
                                  wxSP_ARROW_KEYS, -12000 * dk, 12000 * dk, m_pi->Settings().boundMax * dk, 1);
  lo->SetDigits(1);
  hi->SetDigits(1);
  r->Add(new wxStaticText(&d, wxID_ANY, U8("Garder de Z1 =")), 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 4);
  r->Add(lo, 0, wxRIGHT, 4);
  r->Add(new wxStaticText(&d, wxID_ANY, U8("à Z2 =")), 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 4);
  r->Add(hi, 0, wxRIGHT, 4);
  r->Add(new wxStaticText(&d, wxID_ANY, m_pi->Settings().depthUnit), 0, wxALIGN_CENTER_VERTICAL);
  col->Add(r, 0, wxLEFT | wxRIGHT | wxBOTTOM, 10);
  auto* h = new wxStaticText(&d, wxID_ANY,
                             U8("Exemple : -200 à 200 m retire les remplissages 999 (affichés "
                                "vers -1000 m) et garde l'estran jusqu'à 200 m d'altitude."));
  h->Wrap(d.FromDIP(380));
  col->Add(h, 0, wxLEFT | wxRIGHT | wxBOTTOM, 10);
  col->Add(d.CreateSeparatedButtonSizer(wxOK | wxCANCEL), 0, wxEXPAND | wxALL, 10);
  d.SetSizerAndFit(col);
  d.CentreOnParent();
  if (d.ShowModal() != wxID_OK) return;
  if (!(hi->GetValue() > lo->GetValue())) {
    wxMessageBox(U8("Bornes vides : Z1 doit être inférieur à Z2."), "PersonalDepth",
                 wxOK | wxICON_WARNING, this);
    return;
  }
  // bornes saisies dans l'unité de profondeur d'OpenCPN, stockées en mètres ;
  // une borne inchangée garde sa valeur exacte
  auto toM = [&](double ui, float stored) {
    return std::fabs(ui - std::round(stored * dk * 10.0) / 10.0) < 0.05 ? stored : (float)(ui / dk);
  };
  m_pi->DbClean(ids, toM(lo->GetValue(), m_pi->Settings().boundMin),
                toM(hi->GetValue(), m_pi->Settings().boundMax));
  UpdateButtons();
}

// --------------------------------------------------------------- découpage
void DataDialog::OnResplit() {
  const double cur = m_pi->DbCellDeg();
  const double sizes[] = {1.0, 0.5, 0.25, 0.1};
  wxArrayString ch;
  for (double s : sizes) {
    const double km = s * 111.0;
    ch.Add(CellSizeText(s) + wxString::Format(U8("  — environ %.0f × %.0f km"), km * 0.66, km) +
           (std::fabs(s - cur) < 1e-9 ? U8("  (actuel)") : wxString()));
  }
  long long total = 0;
  for (const auto& c : m_pi->DbCells()) total += c.bytes;
  wxSingleChoiceDialog d(
      this,
      U8("Taille des cellules géographiques des fichiers .pdb. Des cellules plus petites donnent "
         "des fichiers plus petits (à 1 m de résolution, une cellule de 0,25° pleine de données "
         "fait de l'ordre de 300 à 600 Mo ; 0,1° : 50 à 100 Mo).\n\nToutes les tuiles sont "
         "recopiées dans un sous-dossier puis les anciens fichiers sont remplacés : prévoir ") +
          Bytes(total) +
          U8(" d'espace libre en plus. Les fichiers masqués à l'affichage redeviennent visibles."),
      U8("Redécouper les fichiers de la base"), ch);
  d.SetSelection(std::fabs(cur - 0.25) < 1e-9 ? 3 : 2);  // propose une autre taille que l'actuelle
  if (d.ShowModal() != wxID_OK) return;
  const double deg = sizes[d.GetSelection()];
  if (std::fabs(deg - cur) < 1e-9) return;
  m_pi->DbResplit(deg);
  UpdateButtons();
}

// ------------------------------------------------------------------- liste
std::vector<int64_t> DataDialog::SelectedSources() const {
  std::vector<int64_t> ids;
  std::set<int64_t> seen;
  wxTreeListItems sel;
  m_tree->GetSelections(sel);
  auto add = [&](const wxTreeListItem& it) {
    auto* d = static_cast<SourceItem*>(m_tree->GetItemData(it));
    if (d && d->id && seen.insert(d->id).second) ids.push_back(d->id);
  };
  for (const auto& it : sel) {
    auto* d = static_cast<SourceItem*>(m_tree->GetItemData(it));
    if (d && d->id) add(it);
    else
      for (wxTreeListItem c = m_tree->GetFirstChild(it); c.IsOk(); c = m_tree->GetNextSibling(c)) add(c);
  }
  return ids;
}

void DataDialog::UpdateButtons() {
  const bool busy = m_pi->DbBusy(), has = !m_pi->Settings().dbFolder.empty();
  const bool any = !SelectedSources().empty();
  m_import->Enable(has && !busy);
  m_up->Enable(any && !busy);
  m_down->Enable(any && !busy);
  m_del->Enable(any && !busy);
  m_clean->Enable(any && !busy);
  m_edit->Enable(any && !busy);
  m_zoom->Enable(any);
  m_setClass->Enable(any && !busy);
  m_prio->Enable(has && !busy);
  m_splitBtn->Enable(has && !busy && !m_pi->DbCells().empty());
  m_stop->Enable(busy);
}

void DataDialog::UpdateSplitInfo() {
  long long total = 0, biggest = 0;
  for (const auto& c : m_pi->DbCells()) {
    total += c.bytes;
    biggest = std::max(biggest, c.bytes);
  }
  const size_t n = m_pi->DbCells().size();
  m_split->SetLabel(n ? wxString::Format(U8("cellules de %s — %d fichier(s), %s (le plus gros : %s)"),
                                         CellSizeText(m_pi->DbCellDeg()), (int)n, Bytes(total),
                                         Bytes(biggest))
                      : wxString::Format(U8("cellules de %s (base vide)"), CellSizeText(m_pi->DbCellDeg())));
}

// métadonnées de la sélection
void DataDialog::UpdateDetails() {
  // profondeurs affichées dans l'unité d'OpenCPN (m ou ft), stockées en m
  const float dk = m_pi->Settings().depthK > 0 ? m_pi->Settings().depthK : 1.f;
  const wxString du = m_pi->Settings().depthUnit;
  (void)du;
  const auto ids = SelectedSources();
  if (ids.empty()) { m_details->Clear(); return; }
  wxString t;
  if (ids.size() == 1) {
    for (const auto& d : m_pi->DbSources()) {
      if (d.id != ids[0]) continue;
      static const char* prioNames[] = {"résolution puis date", "sonde la plus faible",
                                        "levé le plus récent", "rang manuel"};
      t << U8("Source      : ") << W8(d.name.c_str()) << U8("   (classe ") << W8(d.group.c_str()) << ")\n";
      t << U8("Fichier     : ") << W8(d.path.c_str()) << "\n";
      t << U8("Levé        : ") << W8(d.date.c_str()) << U8("   importé le ") << W8(d.imported.c_str())
        << wxString::Format(U8("   rang %d (règle : %s)"), d.rank + 1,
                            U8(prioNames[std::max(0, std::min(3, m_pi->Settings().dbPriority))]))
        << "\n";
      t << wxString::Format(U8("Résolution  : %.2f m (niveau de tuiles %d)   %lld sondes   Z %.2f … %.2f %s (ZH)\n"),
                            d.res, d.level, d.count, d.zmin * dk, d.zmax * dk, du);
      t << U8("Coordonnées : ") << U8(d.crs.c_str()) << U8("   Réf. verticale : ") << U8(d.vref.c_str()) << "\n";
      t << wxString::Format(U8("Emprise     : lon %.5f … %.5f   lat %.5f … %.5f\n"), d.lonMin, d.lonMax,
                            d.latMin, d.latMax);
      wxString files;
      for (const auto& c : m_pi->DbCells())
        if (c.lonW < d.lonMax && c.lonW + c.deg > d.lonMin && c.latS < d.latMax && c.latS + c.deg > d.latMin)
          files << (files.empty() ? "" : ", ") << W8(c.name.c_str());
      t << U8("Fichiers    : ") << files;
      break;
    }
  } else {
    long long n = 0;
    float zlo = 1e9f, zhi = -1e9f;
    double rlo = 1e9, rhi = 0;
    std::string dlo = "9999", dhi = "0000";
    for (const auto& d : m_pi->DbSources())
      if (std::find(ids.begin(), ids.end(), d.id) != ids.end()) {
        n += d.count;
        zlo = std::min(zlo, d.zmin); zhi = std::max(zhi, d.zmax);
        rlo = std::min(rlo, d.res); rhi = std::max(rhi, d.res);
        dlo = std::min(dlo, d.date); dhi = std::max(dhi, d.date);
      }
    t << wxString::Format(U8("%d sources sélectionnées — %s sondes\n"), (int)ids.size(), Count(n));
    t << wxString::Format(U8("Résolution %.2f … %.2f m   Z %.2f … %.2f %s (ZH)\n"), rlo, rhi, zlo * dk,
                          zhi * dk, du);
    t << U8("Levés du ") << W8(dlo.c_str()) << U8(" au ") << W8(dhi.c_str());
  }
  m_details->ChangeValue(t);
}

// Arbre des sources groupées par classe ; les classes sont rangées selon le
// rang de leur première source, les sources selon leur rang
void DataDialog::RefreshDb() {
  // profondeurs affichées dans l'unité d'OpenCPN (m ou ft), stockées en m
  const float dk = m_pi->Settings().depthK > 0 ? m_pi->Settings().depthK : 1.f;
  const wxString du = m_pi->Settings().depthUnit;
  (void)du;
  m_prio->SetSelection(std::max(0, std::min(3, m_pi->Settings().dbPriority)));
  const auto selIds = SelectedSources();
  const std::set<int64_t> sel(selIds.begin(), selIds.end());
  for (wxTreeListItem g = m_tree->GetFirstChild(m_tree->GetRootItem()); g.IsOk();
       g = m_tree->GetNextSibling(g)) {
    const std::string name = m_tree->GetItemText(g).BeforeLast('(').Trim().ToUTF8().data();
    if (m_tree->IsExpanded(g)) m_collapsed.erase(name);
    else m_collapsed.insert(name);
  }
  m_tree->DeleteAllItems();
  const auto& src = m_pi->DbSources();
  std::vector<std::string> order;
  std::map<std::string, int> count;
  std::map<std::string, long long> sondes;
  for (const auto& d : src) {
    if (!count.count(d.group)) order.push_back(d.group);
    count[d.group]++;
    sondes[d.group] += d.count;
  }
  std::map<std::string, wxTreeListItem> groups;
  const wxTreeListItem root = m_tree->GetRootItem();
  for (const auto& g : order) {
    const wxTreeListItem gi = m_tree->AppendItem(
        root, wxString::Format("%s (%d)", W8(g.c_str()), count[g]), -1, -1, new SourceItem(0));
    m_tree->SetItemText(gi, 3, Count(sondes[g]));
    groups[g] = gi;
  }
  for (const auto& d : src) {
    const wxTreeListItem it =
        m_tree->AppendItem(groups[d.group], W8(d.name.c_str()), -1, -1, new SourceItem(d.id));
    m_tree->SetItemText(it, 1, W8(d.date.c_str()));
    m_tree->SetItemText(it, 2, wxString::Format("%.2f", d.res));
    m_tree->SetItemText(it, 3, Count(d.count));
    m_tree->SetItemText(it, 4, wxString::Format("%d", d.rank + 1));
    m_tree->SetItemText(it, 5, wxString::Format("%.2f %s", d.zmin * dk, du));
    m_tree->SetItemText(it, 6, wxString::Format("%.2f %s", d.zmax * dk, du));
    m_tree->SetItemText(it, 7, U8(d.vref.c_str()));
    m_tree->SetItemText(it, 8, U8(d.crs.c_str()));
    {  // unité verticale du fichier d'origine (réglages d'import gardés avec la source)
      wxString u = "?";
      const size_t k = d.params.find("zFeet=");
      if (k != std::string::npos) u = d.params.compare(k + 6, 1, "1") == 0 ? "ft" : "m";
      m_tree->SetItemText(it, 9, u);
    }
    m_tree->SetItemText(it, 10, W8(d.imported.c_str()));
    m_tree->CheckItem(it, d.visible ? wxCHK_CHECKED : wxCHK_UNCHECKED);
    if (sel.count(d.id)) m_tree->Select(it);
  }
  for (const auto& kv : groups) {
    const wxTreeListItem first = m_tree->GetFirstChild(kv.second);
    if (first.IsOk()) m_tree->UpdateItemParentStateRecursively(first);
    if (!m_collapsed.count(kv.first)) m_tree->Expand(kv.second);
  }
  const wxString cur = m_class->GetValue();
  m_class->Clear();
  for (const auto& g : order) m_class->Append(W8(g.c_str()));
  m_class->SetValue(cur);
  UpdateSplitInfo();
  UpdateDetails();
  SetDbStatus(m_pi->DbStatus());
  UpdateButtons();
}

void DataDialog::SetDbStatus(const wxString& t) {
  if (m_status->GetLabel() != t) {
    m_status->SetLabel(t);
    m_status->Wrap(std::max(200, m_status->GetSize().x));
    Layout();
  }
  const float p = m_pi->DbProgressValue();
  if (p < 0) m_gauge->SetValue(0);
  else m_gauge->SetValue((int)std::lround(1000 * std::max(0.f, std::min(1.f, p))));
  UpdateButtons();
}

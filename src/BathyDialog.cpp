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

#include "BathyDialog.h"

#include <wx/colordlg.h>
#include <wx/dir.h>
#include <wx/dirdlg.h>
#include <wx/filename.h>
#include <wx/tokenzr.h>
#include <wx/filedlg.h>
#include <wx/scrolwin.h>
#include <wx/statline.h>

#include <map>

#include "DataDialog.h"
#include "PersonalDepth_pi.h"
#include "core/Projection.h"

#define U8(s) wxString::FromUTF8(s)

using namespace pd;

static wxColour ToWx(uint32_t c) { return wxColour(R_(c), G_(c), B_(c), A_(c)); }
static uint32_t FromWx(const wxColour& c, int alpha = 255) {
  return RGBA(c.Red(), c.Green(), c.Blue(), alpha);
}

static bool ParseNum(wxString s, double& v) {
  s.Replace(",", ".");
  return s.Trim().Trim(false).ToCDouble(&v);
}

BathyDialog::BathyDialog(wxWindow* parent, PersonalDepth_pi* pi)
    : wxDialog(parent, wxID_ANY, U8("PersonalDepth — bathymétrie personnelle"),
               wxDefaultPosition, wxDefaultSize,
               wxDEFAULT_DIALOG_STYLE | wxRESIZE_BORDER),
      m_pi(pi) {
  auto* scroll = new wxScrolledWindow(this, wxID_ANY);
  scroll->SetScrollRate(0, 12);
  auto* col = new wxBoxSizer(wxVERTICAL);
  col->Add(BuildDbBox(scroll), 0, wxEXPAND | wxALL, 6);
  col->Add(BuildTideBox(scroll), 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, 6);
  col->Add(BuildPaletteBox(scroll), 1, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, 6);
  col->Add(BuildContourBox(scroll), 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, 6);
  col->Add(BuildDisplayBox(scroll), 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, 6);
  scroll->SetSizer(col);
  auto* top = new wxBoxSizer(wxVERTICAL);
  top->Add(scroll, 1, wxEXPAND);
  SetSizer(top);
  SetSize(FromDIP(wxSize(470, 760)));
  Bind(wxEVT_CLOSE_WINDOW, &BathyDialog::OnClose, this);
  RefreshPalette();
  RefreshDb();
}

// -------------------------------------------------------------------- marée
wxSizer* BathyDialog::BuildTideBox(wxWindow* p) {
  auto* box = new wxStaticBoxSizer(wxVERTICAL, p, U8("Marée"));
  wxWindow* sb = box->GetStaticBox();
  wxString srcs[] = {U8("Stations de marée OpenCPN (hauteur unique)"),
                     U8("FES2022 maillé (hauteur variable sur la zone)")};
  m_tideSrc = new wxChoice(sb, wxID_ANY, wxDefaultPosition, wxDefaultSize, 2, srcs);
  m_tideSrc->SetToolTip(U8("FES2022 : hauteur calculée en chaque nœud de l'atlas (1/30°), "
                           "ajoutée cellule par cellule ; nécessite le fichier .pdtide."));
  auto fileRow = [&](wxTextCtrl*& t, wxButton*& b) {
    auto* r = new wxBoxSizer(wxHORIZONTAL);
    t = new wxTextCtrl(sb, wxID_ANY, "", wxDefaultPosition, wxDefaultSize, wxTE_PROCESS_ENTER);
    b = new wxButton(sb, wxID_ANY, U8("…"), wxDefaultPosition, wxSize(FromDIP(28), -1));
    r->Add(t, 1, wxRIGHT, 2);
    r->Add(b, 0);
    return r;
  };
  auto* rAt = fileRow(m_tideAtlas, m_tideAtlasBtn);
  auto* rZ0 = fileRow(m_tideZ0, m_tideZ0Btn);
  m_tideAtlas->SetHint(U8("atlas FES2022 : FES2022b_ocean.pdtide"));
  m_tideZ0->SetHint(U8("facultatif : grille(s) Z0 = niveau moyen au-dessus du ZH (m)"));
  m_tideBE = new wxCheckBox(sb, wxID_ANY, U8("Z0 par BathyElli NM/ell − ZH/ell (dossier France) si présents"));
  m_tideBE->SetToolTip(U8("Ordre de priorité de Z0 : grille fournie, puis BathyElli NM/ell − ZH/ell, "
                          "puis plus basse mer astronomique FES sur 19 ans (approximation du ZH)."));
  auto* g = new wxFlexGridSizer(2, 4, 6);
  g->AddGrowableCol(1, 1);
  g->Add(new wxStaticText(sb, wxID_ANY, U8("Atlas")), 0, wxALIGN_CENTER_VERTICAL);
  g->Add(rAt, 1, wxEXPAND);
  g->Add(new wxStaticText(sb, wxID_ANY, U8("Grille Z0")), 0, wxALIGN_CENTER_VERTICAL);
  g->Add(rZ0, 1, wxEXPAND);
  box->Add(m_tideSrc, 0, wxEXPAND | wxALL, 4);
  box->Add(g, 0, wxEXPAND | wxLEFT | wxRIGHT, 4);
  box->Add(m_tideBE, 0, wxALL, 4);
  wxString modes[] = {U8("Aucune — profondeurs au zéro hydrographique"),
                      U8("Hauteur d'eau actuelle (heure du PC)"),
                      U8("Date et heure choisies")};
  m_tideMode = new wxChoice(sb, wxID_ANY, wxDefaultPosition, wxDefaultSize, 3, modes);
  m_tideDate = new wxDatePickerCtrl(sb, wxID_ANY);
  m_tideTime = new wxTimePickerCtrl(sb, wxID_ANY);
  m_tideShift = new wxSlider(sb, wxID_ANY, 0, -48, 48);  // pas de 15 min, ±12 h
  m_tideShift->SetToolTip(U8("Faire défiler l'heure de ±12 h par pas de 15 min"));
  m_tideShiftLbl = new wxStaticText(sb, wxID_ANY, "+0:00");
  m_tideInfo = new wxStaticText(sb, wxID_ANY, "");
  m_tideInfo->SetFont(m_tideInfo->GetFont().Bold());
  auto* r1 = new wxBoxSizer(wxHORIZONTAL);
  r1->Add(m_tideDate, 0, wxRIGHT, 4);
  r1->Add(m_tideTime, 0);
  auto* r2 = new wxBoxSizer(wxHORIZONTAL);
  r2->Add(m_tideShift, 1, wxALIGN_CENTER_VERTICAL);
  r2->Add(m_tideShiftLbl, 0, wxALIGN_CENTER_VERTICAL | wxLEFT, 4);
  box->Add(m_tideMode, 0, wxEXPAND | wxALL, 4);
  box->Add(r1, 0, wxLEFT | wxRIGHT, 4);
  box->Add(r2, 0, wxEXPAND | wxALL, 4);
  box->Add(m_tideInfo, 0, wxEXPAND | wxALL, 4);

  const DisplaySettings& s = m_pi->Settings();
  m_tideMode->SetSelection(std::max(0, std::min(2, s.tideMode)));
  m_tideSrc->SetSelection(std::max(0, std::min(1, s.tideSource)));
  m_tideAtlas->SetValue(U8(s.tideAtlas.c_str()));
  m_tideZ0->SetValue(U8(s.tideZ0Files.c_str()));
  m_tideBE->SetValue(s.tideUseBathyElli);
  wxDateTime dt = s.tideTime > 0 ? wxDateTime((time_t)s.tideTime) : wxDateTime::Now();
  m_tideDate->SetValue(dt);
  m_tideTime->SetValue(dt);
  m_tideShift->SetValue(s.tideShiftMin / 15);
  m_tideInfo->SetLabel(s.tideInfo);
  UpdateTideControls();

  m_tideMode->Bind(wxEVT_CHOICE, &BathyDialog::OnTide, this);
  m_tideSrc->Bind(wxEVT_CHOICE, &BathyDialog::OnTide, this);
  m_tideBE->Bind(wxEVT_CHECKBOX, &BathyDialog::OnTide, this);
  for (wxTextCtrl* t : {m_tideAtlas, m_tideZ0}) {
    t->Bind(wxEVT_TEXT_ENTER, &BathyDialog::OnTide, this);
    t->Bind(wxEVT_KILL_FOCUS, [this](wxFocusEvent& e) { wxCommandEvent c; OnTide(c); e.Skip(); });
  }
  m_tideAtlasBtn->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) {
    wxFileDialog fd(this, U8("Atlas de marée FES2022"), "", m_tideAtlas->GetValue(),
                    U8("Atlas PersonalDepth (*.pdtide)|*.pdtide|Tous|*"),
                    wxFD_OPEN | wxFD_FILE_MUST_EXIST);
    if (fd.ShowModal() != wxID_OK) return;
    m_tideAtlas->SetValue(fd.GetPath());
    wxCommandEvent c; OnTide(c);
  });
  m_tideZ0Btn->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) {
    wxFileDialog fd(this, U8("Grille(s) Z0 : niveau moyen au-dessus du ZH"), "", "",
                    U8("Grilles (*.tac;*.mnt;*.gtx;*.asc;*.glhi;*.xyz;*.txt)|*.tac;*.mnt;*.gtx;*.asc;*.glhi;*.xyz;*.txt|Tous|*"),
                    wxFD_OPEN | wxFD_FILE_MUST_EXIST | wxFD_MULTIPLE);
    if (fd.ShowModal() != wxID_OK) return;
    wxArrayString ps; fd.GetPaths(ps);
    wxString v;
    for (size_t k = 0; k < ps.size(); ++k) v << (k ? ";" : "") << ps[k];
    m_tideZ0->SetValue(v);
    wxCommandEvent c; OnTide(c);
  });
  m_tideDate->Bind(wxEVT_DATE_CHANGED, [this](wxDateEvent&) { wxCommandEvent e; OnTide(e); });
  m_tideTime->Bind(wxEVT_TIME_CHANGED, [this](wxDateEvent&) { wxCommandEvent e; OnTide(e); });
  m_tideShift->Bind(wxEVT_SLIDER, &BathyDialog::OnTide, this);
  return box;
}

void BathyDialog::UpdateTideControls() {
  const bool date = m_tideMode->GetSelection() == 2;
  const bool fes = m_tideSrc->GetSelection() == 1;
  for (wxWindow* w : std::initializer_list<wxWindow*>{m_tideAtlas, m_tideAtlasBtn, m_tideZ0, m_tideZ0Btn, m_tideBE})
    w->Enable(fes);
  m_tideDate->Enable(date);
  m_tideTime->Enable(date);
  m_tideShift->Enable(date);
  const int m = m_tideShift->GetValue() * 15;
  m_tideShiftLbl->SetLabel(wxString::Format("%c%d:%02d", m < 0 ? '-' : '+', std::abs(m) / 60, std::abs(m) % 60));
}

void BathyDialog::OnTide(wxCommandEvent&) {
  if (m_updating) return;
  UpdateTideControls();
  DisplaySettings& s = m_pi->Settings();
  s.tideMode = m_tideMode->GetSelection();
  wxDateTime d = m_tideDate->GetValue();
  const wxDateTime t = m_tideTime->GetValue();
  if (d.IsValid() && t.IsValid()) {
    d.SetHour(t.GetHour());
    d.SetMinute(t.GetMinute());
    d.SetSecond(0);
    s.tideTime = (long long)d.GetTicks();
  }
  s.tideShiftMin = m_tideShift->GetValue() * 15;
  s.tideSource = m_tideSrc->GetSelection();
  s.tideAtlas = m_tideAtlas->GetValue().Trim().Trim(false).ToUTF8().data();
  s.tideZ0Files = m_tideZ0->GetValue().Trim().Trim(false).ToUTF8().data();
  s.tideUseBathyElli = m_tideBE->GetValue();
  m_pi->TideChanged();
}

void BathyDialog::SetTideInfo(const wxString& t) {
  if (m_tideInfo && m_tideInfo->GetLabel() != t) {
    m_tideInfo->SetLabel(t);
    m_tideInfo->Wrap(FromDIP(420));
    Layout();
  }
}

// ------------------------------------------------------ base de bathymétrie
namespace {
struct CellItem : public wxClientData {
  explicit CellItem(const std::string& n) : name(n) {}
  std::string name;
};
wxString CellBytes(long long b) {
  if (b >= (1LL << 30)) return wxString::Format("%.2f Go", b / double(1LL << 30));
  if (b >= (1LL << 20)) return wxString::Format("%.0f Mo", b / double(1LL << 20));
  return wxString::Format("%.0f ko", b / 1024.0);
}
}  // namespace

wxSizer* BathyDialog::BuildDbBox(wxWindow* p) {
  auto* box = new wxStaticBoxSizer(wxVERTICAL, p, U8("Base de bathymétrie"));
  wxWindow* sb = box->GetStaticBox();
  auto* r1 = new wxBoxSizer(wxHORIZONTAL);
  m_dbFolder = new wxTextCtrl(sb, wxID_ANY, U8(m_pi->Settings().dbFolder.c_str()), wxDefaultPosition,
                              wxDefaultSize, wxTE_PROCESS_ENTER);
  m_dbFolder->SetHint(U8("dossier de la base (fichiers PD_*.pdb)"));
  m_dbFolderBtn = new wxButton(sb, wxID_ANY, U8("…"), wxDefaultPosition, wxSize(FromDIP(28), -1));
  r1->Add(new wxStaticText(sb, wxID_ANY, U8("Dossier")), 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 4);
  r1->Add(m_dbFolder, 1, wxRIGHT, 2);
  r1->Add(m_dbFolderBtn, 0);
  box->Add(r1, 0, wxEXPAND | wxALL, 4);

  // fichiers .pdb (cellules géographiques) : affichés ou non
  m_cells = new wxTreeListCtrl(sb, wxID_ANY, wxDefaultPosition, wxSize(-1, FromDIP(150)),
                               wxTL_MULTIPLE | wxTL_CHECKBOX);
  m_cells->AppendColumn(U8("Fichier .pdb"), FromDIP(170));
  m_cells->AppendColumn(U8("Taille"), FromDIP(70), wxALIGN_RIGHT);
  m_cells->AppendColumn(U8("Sources"), FromDIP(60), wxALIGN_RIGHT);
  m_cells->SetToolTip(U8("Case : afficher ou masquer la bathymétrie de ce fichier (sa cellule "
                         "géographique). Les sources et leur import se gèrent dans "
                         "« Gestion des données »."));
  box->Add(m_cells, 0, wxEXPAND | wxLEFT | wxRIGHT, 4);

  auto* r3 = new wxBoxSizer(wxHORIZONTAL);
  m_dbManage = new wxButton(sb, wxID_ANY, U8("Gestion des données…"));
  m_dbManage->SetToolTip(U8("Importer des fichiers, voir les sources importées et leurs "
                            "métadonnées, priorités, classes, nettoyage, découpage des fichiers."));
  m_cellAll = new wxButton(sb, wxID_ANY, U8("Tout"), wxDefaultPosition, wxSize(FromDIP(44), -1));
  m_cellNone = new wxButton(sb, wxID_ANY, U8("Aucun"), wxDefaultPosition, wxSize(FromDIP(52), -1));
  m_cellZoom = new wxButton(sb, wxID_ANY, U8("Centrer"));
  m_cellZoom->SetToolTip(U8("Centrer la carte sur les fichiers sélectionnés"));
  r3->Add(m_dbManage, 0, wxRIGHT, 8);
  r3->Add(m_cellAll, 0, wxRIGHT, 2);
  r3->Add(m_cellNone, 0, wxRIGHT, 8);
  r3->Add(m_cellZoom, 0);
  box->Add(r3, 0, wxALL, 4);
  auto* r4 = new wxBoxSizer(wxHORIZONTAL);
  m_dbStatus = new wxStaticText(sb, wxID_ANY, "");
  m_dbStop = new wxButton(sb, wxID_ANY, U8("Arrêter"));
  m_dbStop->SetToolTip(U8("Arrêter l'opération en cours sur la base (import, nettoyage…). "
                          "Pour un import, les fichiers déjà terminés restent dans la base."));
  r4->Add(m_dbStatus, 1, wxALIGN_CENTER_VERTICAL | wxRIGHT, 4);
  r4->Add(m_dbStop, 0, wxALIGN_CENTER_VERTICAL);
  box->Add(r4, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, 4);

  auto setFolder = [this]() {
    const std::string f = m_dbFolder->GetValue().Trim().Trim(false).ToUTF8().data();
    if (f == m_pi->Settings().dbFolder) return;
    if (m_pi->DbBusy()) { m_dbFolder->SetValue(U8(m_pi->Settings().dbFolder.c_str())); return; }
    m_pi->Settings().dbFolder = f;
    m_pi->Settings().dbHidden.clear();
    m_pi->SaveConfig();
    m_pi->DbOpen();
    RefreshDb();
  };
  m_dbFolder->Bind(wxEVT_TEXT_ENTER, [setFolder](wxCommandEvent&) { setFolder(); });
  m_dbFolder->Bind(wxEVT_KILL_FOCUS, [setFolder](wxFocusEvent& e) { setFolder(); e.Skip(); });
  m_dbFolderBtn->Bind(wxEVT_BUTTON, [this, setFolder](wxCommandEvent&) {
    wxDirDialog dd(this, U8("Dossier de la base de bathymétrie"), m_dbFolder->GetValue());
    if (dd.ShowModal() != wxID_OK) return;
    m_dbFolder->SetValue(dd.GetPath());
    setFolder();
  });
  m_dbManage->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) {
    if (!m_data) m_data = new DataDialog(this, m_pi);
    m_data->RefreshDb();
    m_data->Show();
    m_data->Raise();
  });
  m_dbStop->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) {
    m_pi->DbStop();
    m_dbStop->Disable();
  });
  auto setAll = [this](bool on) {
    std::vector<std::string> names;
    for (const auto& c : m_pi->DbCells()) names.push_back(c.name);
    m_pi->DbSetCellsVisible(names, on);
    RefreshCells();
  };
  m_cellAll->Bind(wxEVT_BUTTON, [setAll](wxCommandEvent&) { setAll(true); });
  m_cellNone->Bind(wxEVT_BUTTON, [setAll](wxCommandEvent&) { setAll(false); });
  m_cellZoom->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { m_pi->DbZoomToCells(SelectedCells()); });
  m_cells->Bind(wxEVT_TREELIST_SELECTION_CHANGED, [this](wxTreeListEvent&) { UpdateDbButtons(); });
  m_cells->Bind(wxEVT_TREELIST_ITEM_CHECKED, [this](wxTreeListEvent& e) {
    const wxTreeListItem it = e.GetItem();
    auto* d = static_cast<CellItem*>(m_cells->GetItemData(it));
    if (!d) return;
    // case cochée sur une ligne sélectionnée : toute la sélection suit
    const bool on = m_cells->GetCheckedState(it) == wxCHK_CHECKED;
    std::vector<std::string> names = SelectedCells();
    if (std::find(names.begin(), names.end(), d->name) == names.end()) names = {d->name};
    m_pi->DbSetCellsVisible(names, on);
    RefreshCells();
  });
  return box;
}

std::vector<std::string> BathyDialog::SelectedCells() const {
  std::vector<std::string> v;
  wxTreeListItems sel;
  m_cells->GetSelections(sel);
  for (const auto& it : sel)
    if (auto* d = static_cast<CellItem*>(m_cells->GetItemData(it))) v.push_back(d->name);
  return v;
}

void BathyDialog::UpdateDbButtons() {
  const bool busy = m_pi->DbBusy(), has = !m_pi->DbCells().empty();
  m_dbStop->Enable(busy);
  m_cellAll->Enable(has);
  m_cellNone->Enable(has);
  m_cellZoom->Enable(!SelectedCells().empty());
  m_dbFolder->Enable(!busy);
  m_dbFolderBtn->Enable(!busy);
}

void BathyDialog::RefreshCells() {
  std::set<std::string> sel;
  for (const auto& n : SelectedCells()) sel.insert(n);
  m_cells->DeleteAllItems();
  const auto& hidden = m_pi->Settings().dbHidden;
  for (const auto& c : m_pi->DbCells()) {
    const wxTreeListItem it =
        m_cells->AppendItem(m_cells->GetRootItem(), U8(c.name.c_str()), -1, -1, new CellItem(c.name));
    m_cells->SetItemText(it, 1, CellBytes(c.bytes));
    m_cells->SetItemText(it, 2, wxString::Format("%d", c.sources));
    m_cells->CheckItem(it, hidden.count(c.name) ? wxCHK_UNCHECKED : wxCHK_CHECKED);
    if (sel.count(c.name)) m_cells->Select(it);
  }
  UpdateDbButtons();
}

void BathyDialog::RefreshDb() {
  RefreshCells();
  SetDbStatus(m_pi->DbStatus());
  if (m_data) m_data->RefreshDb();
}

void BathyDialog::SetDbStatus(const wxString& t) {
  if (m_dbStatus && m_dbStatus->GetLabel() != t) {
    m_dbStatus->SetLabel(t);
    m_dbStatus->Wrap(FromDIP(330));
    Layout();
  }
  if (m_dbStop) UpdateDbButtons();
  if (m_data && m_data->IsShown()) m_data->SetDbStatus(t);
}

// ---------------------------------------------------------------- Palette
wxSizer* BathyDialog::BuildPaletteBox(wxWindow* p) {
  auto* box = new wxStaticBoxSizer(wxVERTICAL, p, U8("Palette et seuils de profondeur"));
  wxWindow* sb = box->GetStaticBox();
  auto* row = new wxBoxSizer(wxHORIZONTAL);
  m_preset = new wxChoice(sb, wxID_ANY);
  m_preset->Append(U8("(personnalisée)"));
  for (auto& n : ColorRamp::PresetNames()) m_preset->Append(U8(n.c_str()));
  m_preset->SetSelection(0);
  wxString modes[] = {U8("Dégradé continu"), U8("Paliers")};
  m_mode = new wxRadioBox(sb, wxID_ANY, "", wxDefaultPosition, wxDefaultSize, 2, modes, 2);
  row->Add(m_preset, 1, wxALIGN_CENTER_VERTICAL | wxRIGHT, 6);
  row->Add(m_mode, 0);
  box->Add(row, 0, wxEXPAND | wxALL, 4);

  // Palette automatique : seuils d'après les profondeurs présentes
  auto* ar = new wxBoxSizer(wxHORIZONTAL);
  m_autoOn = new wxCheckBox(sb, wxID_ANY, U8("Automatique"));
  m_autoOn->SetToolTip(U8("Seuils répartis entre la profondeur minimale et maximale, couleurs "
                          "reprises de la palette choisie. Modifier un seuil à la main "
                          "désactive le mode automatique."));
  wxString srcs[] = {U8("toutes les données"), U8("zone affichée")};
  m_autoSrc = new wxChoice(sb, wxID_ANY, wxDefaultPosition, wxDefaultSize, 2, srcs);
  m_autoSrc->SetToolTip(U8("Toutes les données : base et couches visibles. Zone affichée : "
                           "recalculé quand la carte se déplace."));
  m_autoCount = new wxSpinCtrl(sb, wxID_ANY, "", wxDefaultPosition, wxSize(FromDIP(60), -1),
                               wxSP_ARROW_KEYS, 2, 40, 10);
  m_autoNice = new wxCheckBox(sb, wxID_ANY, U8("valeurs rondes"));
  m_autoNice->SetToolTip(U8("Seuils multiples de 1, 2, 2,5 ou 5 × 10ⁿ (sinon répartis exactement)"));
  ar->Add(m_autoOn, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 6);
  ar->Add(m_autoSrc, 1, wxALIGN_CENTER_VERTICAL | wxRIGHT, 6);
  ar->Add(new wxStaticText(sb, wxID_ANY, U8("seuils")), 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 4);
  ar->Add(m_autoCount, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 6);
  ar->Add(m_autoNice, 0, wxALIGN_CENTER_VERTICAL);
  box->Add(ar, 0, wxEXPAND | wxLEFT | wxRIGHT, 4);
  auto* br = new wxBoxSizer(wxHORIZONTAL);
  m_autoMinOn = new wxCheckBox(sb, wxID_ANY, U8("min imposé"));
  m_autoMinOn->SetToolTip(U8("Premier seuil fixé (ex. 0 m : palette sur la seule bathymétrie, "
                             "les altitudes gardent une seule couleur)"));
  m_autoMin = new wxSpinCtrlDouble(sb, wxID_ANY, "", wxDefaultPosition, wxSize(FromDIP(80), -1),
                                   wxSP_ARROW_KEYS, -500, 11000, 0, 0.5);
  m_autoMin->SetDigits(1);
  m_autoMaxOn = new wxCheckBox(sb, wxID_ANY, U8("max imposé"));
  m_autoMaxOn->SetToolTip(U8("Dernier seuil fixé : les fonds plus profonds gardent la dernière couleur"));
  m_autoMax = new wxSpinCtrlDouble(sb, wxID_ANY, "", wxDefaultPosition, wxSize(FromDIP(80), -1),
                                   wxSP_ARROW_KEYS, -500, 11000, 50, 0.5);
  m_autoMax->SetDigits(1);
  br->Add(m_autoMinOn, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 4);
  br->Add(m_autoMin, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 2);
  br->Add(new wxStaticText(sb, wxID_ANY, "m"), 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 12);
  br->Add(m_autoMaxOn, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 4);
  br->Add(m_autoMax, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 2);
  br->Add(new wxStaticText(sb, wxID_ANY, "m"), 0, wxALIGN_CENTER_VERTICAL);
  box->Add(br, 0, wxLEFT | wxRIGHT | wxTOP, 4);
  m_autoInfo = new wxStaticText(sb, wxID_ANY, "");
  box->Add(m_autoInfo, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, 4);
  {
    const DisplaySettings& ds = m_pi->Settings();
    m_autoOn->SetValue(ds.autoPalette);
    m_autoSrc->SetSelection(std::max(0, std::min(1, ds.autoSource)));
    m_autoCount->SetValue(ds.autoCount);
    m_autoNice->SetValue(ds.autoNice);
    m_autoMinOn->SetValue(ds.autoMinOn);
    m_autoMaxOn->SetValue(ds.autoMaxOn);
    m_autoMin->SetValue(ds.autoMin);
    m_autoMax->SetValue(ds.autoMax);
  }
  m_autoMinOn->Bind(wxEVT_CHECKBOX, &BathyDialog::OnAuto, this);
  m_autoMaxOn->Bind(wxEVT_CHECKBOX, &BathyDialog::OnAuto, this);
  m_autoMin->Bind(wxEVT_SPINCTRLDOUBLE, [this](wxSpinDoubleEvent&) { wxCommandEvent e; OnAuto(e); });
  m_autoMax->Bind(wxEVT_SPINCTRLDOUBLE, [this](wxSpinDoubleEvent&) { wxCommandEvent e; OnAuto(e); });
  m_autoOn->Bind(wxEVT_CHECKBOX, &BathyDialog::OnAuto, this);
  m_autoSrc->Bind(wxEVT_CHOICE, &BathyDialog::OnAuto, this);
  m_autoNice->Bind(wxEVT_CHECKBOX, &BathyDialog::OnAuto, this);
  m_autoCount->Bind(wxEVT_SPINCTRL, [this](wxSpinEvent&) { wxCommandEvent e; OnAuto(e); });

  m_stops = new wxGrid(sb, wxID_ANY, wxDefaultPosition, wxSize(-1, FromDIP(200)));
  m_stops->CreateGrid(0, 2);
  m_stops->SetColLabelValue(0, U8("Seuil (m)"));
  m_stops->SetColLabelValue(1, U8("Couleur (double-clic)"));
  m_stops->HideRowLabels();
  m_stops->SetColSize(0, FromDIP(110));
  m_stops->SetColSize(1, FromDIP(170));
  box->Add(m_stops, 1, wxEXPAND | wxALL, 4);

  auto* b = new wxBoxSizer(wxHORIZONTAL);
  auto* plus = new wxButton(sb, wxID_ANY, U8("+ seuil"), wxDefaultPosition, wxDefaultSize, wxBU_EXACTFIT);
  auto* minus = new wxButton(sb, wxID_ANY, U8("− seuil"), wxDefaultPosition, wxDefaultSize, wxBU_EXACTFIT);
  auto* imp = new wxButton(sb, wxID_ANY, U8("Importer .cpt…"), wxDefaultPosition, wxDefaultSize, wxBU_EXACTFIT);
  auto* exp = new wxButton(sb, wxID_ANY, U8("Exporter .cpt…"), wxDefaultPosition, wxDefaultSize, wxBU_EXACTFIT);
  b->Add(plus, 0, wxRIGHT, 4); b->Add(minus, 0, wxRIGHT, 12);
  b->Add(imp, 0, wxRIGHT, 4); b->Add(exp);
  box->Add(b, 0, wxALL, 4);

  auto* s = new wxBoxSizer(wxHORIZONTAL);
  m_safeOn = new wxCheckBox(sb, wxID_ANY, U8("Zone dangereuse si prof. <"));
  m_safeDepth = new wxSpinCtrlDouble(sb, wxID_ANY, "", wxDefaultPosition, wxSize(FromDIP(80), -1),
                                     wxSP_ARROW_KEYS, -20, 200, 2, 0.1);
  m_safeDepth->SetDigits(1);
  m_safeColour = new wxColourPickerCtrl(sb, wxID_ANY, *wxRED);
  s->Add(m_safeOn, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 4);
  s->Add(m_safeDepth, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 4);
  s->Add(new wxStaticText(sb, wxID_ANY, "m"), 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 8);
  s->Add(m_safeColour, 0, wxALIGN_CENTER_VERTICAL);
  box->Add(s, 0, wxALL, 4);

  m_preset->Bind(wxEVT_CHOICE, &BathyDialog::OnPreset, this);
  m_mode->Bind(wxEVT_RADIOBOX, &BathyDialog::OnMode, this);
  m_stops->Bind(wxEVT_GRID_CELL_CHANGED, &BathyDialog::OnStopEdited, this);
  m_stops->Bind(wxEVT_GRID_CELL_LEFT_DCLICK, &BathyDialog::OnStopColour, this);
  plus->Bind(wxEVT_BUTTON, &BathyDialog::OnAddStop, this);
  minus->Bind(wxEVT_BUTTON, &BathyDialog::OnDelStop, this);
  imp->Bind(wxEVT_BUTTON, &BathyDialog::OnImportCPT, this);
  exp->Bind(wxEVT_BUTTON, &BathyDialog::OnExportCPT, this);
  m_safeOn->Bind(wxEVT_CHECKBOX, &BathyDialog::OnSafety, this);
  m_safeDepth->Bind(wxEVT_SPINCTRLDOUBLE, [this](wxSpinDoubleEvent&) {
    wxCommandEvent e; OnSafety(e);
  });
  m_safeColour->Bind(wxEVT_COLOURPICKER_CHANGED, [this](wxColourPickerEvent&) {
    wxCommandEvent e; OnSafety(e);
  });
  return box;
}

void BathyDialog::FillStopRow(int r) {
  const ColorStop& st = m_pi->Settings().ramp.stops[r];
  m_stops->SetCellValue(r, 0, wxString::FromCDouble(st.depth, 2));
  m_stops->SetCellBackgroundColour(r, 1, ToWx(st.rgba | 0xFF000000u));
  m_stops->SetCellValue(r, 1, wxString::Format("#%02X%02X%02X", R_(st.rgba), G_(st.rgba), B_(st.rgba)));
  int lum = (R_(st.rgba) * 299 + G_(st.rgba) * 587 + B_(st.rgba) * 114) / 1000;
  m_stops->SetCellTextColour(r, 1, lum > 128 ? *wxBLACK : *wxWHITE);
  m_stops->SetReadOnly(r, 1, true);
}

void BathyDialog::RefreshPalette() {
  m_updating = true;
  const ColorRamp& r = m_pi->Settings().ramp;
  int n = (int)r.stops.size(), cur = m_stops->GetNumberRows();
  if (cur < n) m_stops->AppendRows(n - cur);
  else if (cur > n) m_stops->DeleteRows(n, cur - n);
  for (int k = 0; k < n; ++k) FillStopRow(k);
  m_mode->SetSelection(r.mode == ColorRamp::Mode::Stepped ? 1 : 0);
  if (m_autoOn) {
    const DisplaySettings& ds = m_pi->Settings();
    m_autoOn->SetValue(ds.autoPalette);
    m_autoSrc->Enable(ds.autoPalette);
    m_autoCount->Enable(ds.autoPalette);
    m_autoNice->Enable(ds.autoPalette);
    m_autoMinOn->Enable(ds.autoPalette);
    m_autoMaxOn->Enable(ds.autoPalette);
    m_autoMin->Enable(ds.autoPalette && ds.autoMinOn);
    m_autoMax->Enable(ds.autoPalette && ds.autoMaxOn);
    const wxString info = ds.autoPalette ? m_pi->AutoPaletteInfo() : wxString();
    if (m_autoInfo->GetLabel() != info) { m_autoInfo->SetLabel(info); Layout(); }
  }
  m_safeOn->SetValue(r.safetyOn);
  m_safeDepth->SetValue(r.safetyDepth);
  m_safeColour->SetColour(wxColour(R_(r.safetyColor), G_(r.safetyColor), B_(r.safetyColor)));

  const DisplaySettings& s = m_pi->Settings();
  m_opacity->SetValue(s.opacity);
  m_shade->SetValue(s.hillshade);
  m_shadeStrength->SetValue((int)(s.shadeStrength * 100));
  m_exag->SetValue(s.exaggeration);
  m_shadeSmooth->SetValue(s.shadeSmooth);
  m_sound->SetValue(s.soundings);
  m_decimals->SetValue(s.decimals);
  m_spacing->SetValue(s.spacingPx);
  m_font->SetValue(s.fontSize);
  m_smooth->SetValue(s.smooth);
  m_legend->SetValue(s.legend);
  m_ctOn->SetValue(s.contours);
  m_ctMode->SetSelection(s.contourMode);
  m_ctStep->SetValue(s.contourStep);
  m_ctMajor->SetValue(s.contourMajor);
  m_ctLabels->SetValue(s.contourLabels);
  m_ctSmooth->SetValue(s.contourSmooth);
  m_ctColour->SetColour(wxColour(R_(s.contourColor), G_(s.contourColor), B_(s.contourColor)));
  m_ctStep->Enable(s.contourMode == 0);
  m_updating = false;
}

// ------------------------------------------------------ palette automatique
void BathyDialog::OnAuto(wxCommandEvent&) {
  if (m_updating) return;
  DisplaySettings& s = m_pi->Settings();
  const bool was = s.autoPalette;
  s.autoPalette = m_autoOn->GetValue();
  s.autoSource = m_autoSrc->GetSelection();
  s.autoCount = m_autoCount->GetValue();
  s.autoNice = m_autoNice->GetValue();
  s.autoMinOn = m_autoMinOn->GetValue();
  s.autoMaxOn = m_autoMaxOn->GetValue();
  s.autoMin = (float)m_autoMin->GetValue();
  s.autoMax = (float)m_autoMax->GetValue();
  if (s.autoPalette && !was) {  // couleurs de référence : préréglage choisi, sinon palette actuelle
    const int k = m_preset->GetSelection();
    s.autoBase = k > 0 ? ColorRamp::Preset(ColorRamp::PresetNames()[k - 1]).serialize()
                       : s.ramp.serialize();
  }
  m_autoSrc->Enable(s.autoPalette);
  m_autoCount->Enable(s.autoPalette);
  m_autoNice->Enable(s.autoPalette);
  m_autoMinOn->Enable(s.autoPalette);
  m_autoMaxOn->Enable(s.autoPalette);
  m_autoMin->Enable(s.autoPalette && s.autoMinOn);
  m_autoMax->Enable(s.autoPalette && s.autoMaxOn);
  m_pi->AutoPaletteChanged();
  RefreshPalette();
}

void BathyDialog::StopAuto() {
  DisplaySettings& s = m_pi->Settings();
  if (!s.autoPalette) return;
  s.autoPalette = false;
  m_autoOn->SetValue(false);
  m_autoSrc->Enable(false);
  m_autoCount->Enable(false);
  m_autoNice->Enable(false);
  m_autoMinOn->Enable(false);
  m_autoMaxOn->Enable(false);
  m_autoMin->Enable(false);
  m_autoMax->Enable(false);
  m_pi->AutoPaletteChanged();
}

void BathyDialog::ApplyPaletteFromUI() {
  StopAuto();
  ColorRamp& r = m_pi->Settings().ramp;
  std::vector<ColorStop> st;
  for (int k = 0; k < m_stops->GetNumberRows(); ++k) {
    double d;
    if (!ParseNum(m_stops->GetCellValue(k, 0), d)) continue;
    st.push_back({(float)d, FromWx(m_stops->GetCellBackgroundColour(k, 1))});
  }
  if (st.empty()) return;
  r.stops = st;
  r.sort();
  r.mode = m_mode->GetSelection() == 1 ? ColorRamp::Mode::Stepped : ColorRamp::Mode::Continuous;
  m_pi->ColorsChanged();
  RefreshPalette();  // ré-affiche trié
}

void BathyDialog::OnPreset(wxCommandEvent&) {
  int k = m_preset->GetSelection();
  if (k <= 0) return;
  ColorRamp& r = m_pi->Settings().ramp;
  ColorRamp p = ColorRamp::Preset(ColorRamp::PresetNames()[k - 1]);
  if (m_pi->Settings().autoPalette) {  // automatique : nouvelles couleurs, mêmes seuils calculés
    m_pi->Settings().autoBase = p.serialize();
    r.mode = p.mode;
    m_pi->AutoPaletteChanged();
    RefreshPalette();
    return;
  }
  r.stops = p.stops;
  r.mode = p.mode;
  if (p.safetyOn) { r.safetyOn = true; r.safetyDepth = p.safetyDepth; }
  m_pi->ColorsChanged();
  RefreshPalette();
}

void BathyDialog::OnMode(wxCommandEvent&) {
  if (m_updating) return;
  if (m_pi->Settings().autoPalette) {  // paliers / continu sans quitter le mode automatique
    m_pi->Settings().ramp.mode =
        m_mode->GetSelection() == 1 ? ColorRamp::Mode::Stepped : ColorRamp::Mode::Continuous;
    m_pi->ColorsChanged();
    return;
  }
  m_preset->SetSelection(0);
  ApplyPaletteFromUI();
}

void BathyDialog::OnStopEdited(wxGridEvent& e) {
  if (m_updating) return;
  double d;
  if (e.GetCol() == 0 && !ParseNum(m_stops->GetCellValue(e.GetRow(), 0), d)) {
    wxBell();
    FillStopRow(e.GetRow());
    return;
  }
  m_preset->SetSelection(0);
  ApplyPaletteFromUI();
}

void BathyDialog::OnStopColour(wxGridEvent& e) {
  if (e.GetCol() != 1) { e.Skip(); return; }
  wxColourData cd;
  cd.SetColour(m_stops->GetCellBackgroundColour(e.GetRow(), 1));
  cd.SetChooseFull(true);
  wxColourDialog dlg(this, &cd);
  if (dlg.ShowModal() != wxID_OK) return;
  m_stops->SetCellBackgroundColour(e.GetRow(), 1, dlg.GetColourData().GetColour());
  m_preset->SetSelection(0);
  ApplyPaletteFromUI();
}

void BathyDialog::OnAddStop(wxCommandEvent&) {
  StopAuto();
  ColorRamp& r = m_pi->Settings().ramp;
  int sel = m_stops->GetGridCursorRow();
  if (r.stops.empty()) r.stops.push_back({0.f, RGBA(120, 200, 240)});
  else if (sel >= 0 && sel + 1 < (int)r.stops.size()) {  // milieu de l'intervalle sélectionné
    float d = 0.5f * (r.stops[sel].depth + r.stops[sel + 1].depth);
    r.stops.push_back({d, r.colorAt(d) | 0xFF000000u});
  } else {
    float d = r.stops.back().depth * 2 + 1;
    r.stops.push_back({d, r.stops.back().rgba});
  }
  r.sort();
  m_preset->SetSelection(0);
  m_pi->ColorsChanged();
  RefreshPalette();
}

void BathyDialog::OnDelStop(wxCommandEvent&) {
  StopAuto();
  ColorRamp& r = m_pi->Settings().ramp;
  int sel = m_stops->GetGridCursorRow();
  if (sel < 0 || sel >= (int)r.stops.size() || r.stops.size() <= 2) return;
  r.stops.erase(r.stops.begin() + sel);
  m_preset->SetSelection(0);
  m_pi->ColorsChanged();
  RefreshPalette();
}

void BathyDialog::OnImportCPT(wxCommandEvent&) {
  wxFileDialog fd(this, U8("Importer une palette GMT"), "", "",
                  U8("Palette GMT (*.cpt)|*.cpt"), wxFD_OPEN | wxFD_FILE_MUST_EXIST);
  if (fd.ShowModal() != wxID_OK) return;
  ColorRamp& r = m_pi->Settings().ramp;
  ColorRamp tmp = r;
  std::string err;
  if (!tmp.loadCPT(fd.GetPath().ToUTF8().data(), err)) {
    wxMessageBox(U8(err.c_str()), "PersonalDepth", wxICON_WARNING, this);
    return;
  }
  StopAuto();
  r = tmp;
  m_preset->SetSelection(0);
  m_pi->ColorsChanged();
  RefreshPalette();
}

void BathyDialog::OnExportCPT(wxCommandEvent&) {
  wxFileDialog fd(this, U8("Exporter la palette"), "", "palette.cpt",
                  U8("Palette GMT (*.cpt)|*.cpt"), wxFD_SAVE | wxFD_OVERWRITE_PROMPT);
  if (fd.ShowModal() != wxID_OK) return;
  if (!m_pi->Settings().ramp.saveCPT(fd.GetPath().ToUTF8().data()))
    wxMessageBox(U8("Échec de l'écriture"), "PersonalDepth", wxICON_WARNING, this);
}

void BathyDialog::OnSafety(wxCommandEvent&) {
  if (m_updating) return;
  ColorRamp& r = m_pi->Settings().ramp;
  r.safetyOn = m_safeOn->GetValue();
  r.safetyDepth = (float)m_safeDepth->GetValue();
  r.safetyColor = FromWx(m_safeColour->GetColour(), A_(r.safetyColor));
  m_pi->ColorsChanged();
}

// ---------------------------------------------------------------- Isobathes
wxSizer* BathyDialog::BuildContourBox(wxWindow* p) {
  auto* box = new wxStaticBoxSizer(wxVERTICAL, p, U8("Isobathes"));
  wxWindow* sb = box->GetStaticBox();
  auto* g = new wxFlexGridSizer(2, 4, 8);
  g->AddGrowableCol(1);
  auto L = [&](const char* t) { return new wxStaticText(sb, wxID_ANY, U8(t)); };
  m_ctOn = new wxCheckBox(sb, wxID_ANY, U8("Afficher les isobathes"));
  m_ctColour = new wxColourPickerCtrl(sb, wxID_ANY, wxColour(25, 45, 95));
  wxString modes[] = {U8("Intervalle régulier"), U8("Seuils de la palette")};
  m_ctMode = new wxChoice(sb, wxID_ANY, wxDefaultPosition, wxDefaultSize, 2, modes);
  m_ctStep = new wxSpinCtrlDouble(sb, wxID_ANY, "", wxDefaultPosition, wxDefaultSize,
                                  wxSP_ARROW_KEYS, 0.1, 100, 1, 0.5);
  m_ctStep->SetDigits(1);
  m_ctMajor = new wxSpinCtrlDouble(sb, wxID_ANY, "", wxDefaultPosition, wxDefaultSize,
                                   wxSP_ARROW_KEYS, 0, 500, 5, 1);
  m_ctMajor->SetDigits(1);
  m_ctMajor->SetToolTip(U8("Ligne épaisse tous les ... m (0 = aucune)"));
  m_ctLabels = new wxCheckBox(sb, wxID_ANY, U8("Valeurs sur les lignes"));
  m_ctSmooth = new wxCheckBox(sb, wxID_ANY, U8("Lissage des lignes"));
  m_ctSmooth->SetToolTip(U8("Arrondit les angles du tracé (purement graphique)"));
  g->Add(m_ctOn, 0, wxALIGN_CENTER_VERTICAL); g->Add(m_ctColour, 0);
  g->Add(L("Niveaux"), 0, wxALIGN_CENTER_VERTICAL); g->Add(m_ctMode, 0, wxEXPAND);
  g->Add(L("Intervalle (m)"), 0, wxALIGN_CENTER_VERTICAL); g->Add(m_ctStep, 0, wxEXPAND);
  g->Add(L("Maîtresse tous les (m)"), 0, wxALIGN_CENTER_VERTICAL); g->Add(m_ctMajor, 0, wxEXPAND);
  g->Add(m_ctLabels, 0); g->Add(m_ctSmooth, 0);
  box->Add(g, 0, wxEXPAND | wxALL, 4);
  box->Add(new wxStaticText(sb, wxID_ANY,
                            U8("L'isobathe de sécurité (rouge) suit la zone dangereuse.")),
           0, wxALL, 4);

  for (wxCheckBox* c : {m_ctOn, m_ctLabels, m_ctSmooth})
    c->Bind(wxEVT_CHECKBOX, &BathyDialog::OnContours, this);
  m_ctMode->Bind(wxEVT_CHOICE, &BathyDialog::OnContours, this);
  for (wxSpinCtrlDouble* c : {m_ctStep, m_ctMajor})
    c->Bind(wxEVT_SPINCTRLDOUBLE, [this](wxSpinDoubleEvent&) { wxCommandEvent e; OnContours(e); });
  m_ctColour->Bind(wxEVT_COLOURPICKER_CHANGED, [this](wxColourPickerEvent&) {
    wxCommandEvent e; OnContours(e);
  });
  return box;
}

void BathyDialog::OnContours(wxCommandEvent&) {
  if (m_updating) return;
  DisplaySettings& s = m_pi->Settings();
  s.contours = m_ctOn->GetValue();
  s.contourMode = m_ctMode->GetSelection();
  s.contourStep = (float)m_ctStep->GetValue();
  s.contourMajor = (float)m_ctMajor->GetValue();
  s.contourLabels = m_ctLabels->GetValue();
  s.contourSmooth = m_ctSmooth->GetValue();
  s.contourColor = FromWx(m_ctColour->GetColour());
  m_ctStep->Enable(s.contourMode == 0);
  ++s.contourVersion;
  m_pi->ViewChanged();
}

// ---------------------------------------------------------------- Affichage
wxSizer* BathyDialog::BuildDisplayBox(wxWindow* p) {
  auto* box = new wxStaticBoxSizer(wxVERTICAL, p, U8("Affichage"));
  wxWindow* sb = box->GetStaticBox();
  auto* g = new wxFlexGridSizer(2, 4, 8);
  g->AddGrowableCol(1);
  auto L = [&](const char* t) { return new wxStaticText(sb, wxID_ANY, U8(t)); };
  m_opacity = new wxSlider(sb, wxID_ANY, 85, 10, 100);
  m_shade = new wxCheckBox(sb, wxID_ANY, U8("Ombrage du relief"));
  m_shadeStrength = new wxSlider(sb, wxID_ANY, 60, 0, 100);
  m_exag = new wxSpinCtrlDouble(sb, wxID_ANY, "", wxDefaultPosition, wxDefaultSize,
                                wxSP_ARROW_KEYS, 0.5, 50, 3, 0.5);
  m_sound = new wxCheckBox(sb, wxID_ANY, U8("Sondes chiffrées"));
  m_decimals = new wxSpinCtrl(sb, wxID_ANY, "", wxDefaultPosition, wxDefaultSize,
                              wxSP_ARROW_KEYS, 0, 2, 1);
  m_spacing = new wxSlider(sb, wxID_ANY, 34, 16, 120);
  m_font = new wxSpinCtrl(sb, wxID_ANY, "", wxDefaultPosition, wxDefaultSize,
                          wxSP_ARROW_KEYS, 6, 20, 9);
  m_smooth = new wxCheckBox(sb, wxID_ANY, U8("Lissage (interpolation)"));
  m_legend = new wxCheckBox(sb, wxID_ANY, U8("Légende + lecture au curseur"));
  g->Add(L("Opacité"), 0, wxALIGN_CENTER_VERTICAL); g->Add(m_opacity, 0, wxEXPAND);
  g->Add(m_shade, 0, wxALIGN_CENTER_VERTICAL); g->Add(m_shadeStrength, 0, wxEXPAND);
  g->Add(L("Exagération verticale"), 0, wxALIGN_CENTER_VERTICAL); g->Add(m_exag, 0, wxEXPAND);
  m_shadeSmooth = new wxSpinCtrl(sb, wxID_ANY, "", wxDefaultPosition, wxDefaultSize,
                                 wxSP_ARROW_KEYS, 0, 8, 1);
  m_shadeSmooth->SetToolTip(U8("Adoucit le relief ombré (rayon en cellules, 0 = aucun). "
                               "Les couleurs, isobathes et sondes gardent les vraies profondeurs."));
  g->Add(L("Adoucir le relief"), 0, wxALIGN_CENTER_VERTICAL); g->Add(m_shadeSmooth, 0);
  g->Add(m_sound, 0, wxALIGN_CENTER_VERTICAL); g->Add(m_spacing, 0, wxEXPAND);
  g->Add(L("Décimales"), 0, wxALIGN_CENTER_VERTICAL); g->Add(m_decimals, 0);
  g->Add(L("Taille du texte"), 0, wxALIGN_CENTER_VERTICAL); g->Add(m_font, 0);
  g->Add(m_smooth, 0); g->Add(m_legend, 0);
  box->Add(g, 0, wxEXPAND | wxALL, 4);
  m_cursor = new wxStaticText(sb, wxID_ANY, U8("Curseur : —"));
  m_cursor->SetFont(m_cursor->GetFont().Bold());
  box->Add(m_cursor, 0, wxEXPAND | wxALL, 4);

  for (wxSlider* s : {m_opacity, m_shadeStrength, m_spacing})
    s->Bind(wxEVT_SLIDER, &BathyDialog::OnDisplay, this);
  for (wxCheckBox* c : {m_shade, m_sound, m_smooth, m_legend})
    c->Bind(wxEVT_CHECKBOX, &BathyDialog::OnDisplay, this);
  for (wxSpinCtrl* c : {m_decimals, m_font})
    c->Bind(wxEVT_SPINCTRL, [this](wxSpinEvent&) { wxCommandEvent e; OnDisplay(e); });
  m_shadeSmooth->Bind(wxEVT_SPINCTRL, [this](wxSpinEvent&) {
    if (m_updating) return;
    m_pi->Settings().shadeSmooth = m_shadeSmooth->GetValue();
    m_pi->ExaggerationChanged();  // recalcul de l'ombrage
  });
  m_exag->Bind(wxEVT_SPINCTRLDOUBLE, [this](wxSpinDoubleEvent&) {
    wxCommandEvent e; OnExaggeration(e);
  });
  return box;
}

void BathyDialog::OnDisplay(wxCommandEvent&) {
  if (m_updating) return;
  DisplaySettings& s = m_pi->Settings();
  bool colors = s.opacity != m_opacity->GetValue() || s.hillshade != m_shade->GetValue() ||
                (int)(s.shadeStrength * 100) != m_shadeStrength->GetValue();
  s.opacity = m_opacity->GetValue();
  s.hillshade = m_shade->GetValue();
  s.shadeStrength = m_shadeStrength->GetValue() / 100.f;
  s.soundings = m_sound->GetValue();
  s.decimals = m_decimals->GetValue();
  s.spacingPx = m_spacing->GetValue();
  s.fontSize = m_font->GetValue();
  s.smooth = m_smooth->GetValue();
  s.legend = m_legend->GetValue();
  if (colors) m_pi->ColorsChanged(); else m_pi->ViewChanged();
}

void BathyDialog::OnExaggeration(wxCommandEvent&) {
  if (m_updating) return;
  m_pi->Settings().exaggeration = (float)m_exag->GetValue();
  m_pi->ExaggerationChanged();
}

void BathyDialog::SetCursorText(const wxString& t) {
  if (!IsShown()) return;
  wxString txt = t.empty() ? U8("Curseur : —") : U8("Curseur : ") + t;
  txt.Replace("(a ", U8("(à "));
  if (m_cursor->GetLabel() != txt) m_cursor->SetLabel(txt);
}

void BathyDialog::OnClose(wxCloseEvent&) {
  Hide();
  m_pi->OnDialogClose();
}

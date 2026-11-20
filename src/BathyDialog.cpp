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
#include "BathyDialog.h"

#include <wx/display.h>
#include <wx/colordlg.h>
#include <wx/dir.h>
#include <wx/dirdlg.h>
#include <wx/textdlg.h>
#include <wx/filename.h>
#include <wx/tokenzr.h>
#include <wx/filedlg.h>
#include <wx/scrolwin.h>
#include <wx/statline.h>

#include <cmath>
#include <map>

#include "DataDialog.h"
#include "PersonalDepth_pi.h"
#include "core/Projection.h"

#define U8(s) wxString::FromUTF8(pd::tr(s))
#define W8(s) wxString::FromUTF8(s)  // données : pas de traduction

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
  // onglets : Données / Marée / Affichage ; chaque page défile si la fenêtre
  // est petite ; lecture au curseur sous les onglets
  m_tabs = new wxNotebook(this, wxID_ANY);
  using Builder = wxSizer* (BathyDialog::*)(wxWindow*);
  const std::pair<const char*, Builder> pages[] = {
      {"Données", &BathyDialog::BuildDataBox},
      {"Marée", &BathyDialog::BuildTideBox},
      {"Affichage", &BathyDialog::BuildDisplayBox}};
  for (const auto& pg : pages) {
    auto* page = new wxScrolledWindow(m_tabs, wxID_ANY);
    page->SetScrollRate(0, 12);
    auto* col = new wxBoxSizer(wxVERTICAL);
    wxSizer* box = (this->*pg.second)(page);
    if (auto* sbs = dynamic_cast<wxStaticBoxSizer*>(box))  // titre déjà porté par l'onglet
      sbs->GetStaticBox()->SetLabel(wxEmptyString);
    col->Add(box, 1, wxEXPAND | wxALL, 6);
    page->SetSizer(col);
    m_tabs->AddPage(page, U8(pg.first));
  }
  m_cursor = new wxStaticText(this, wxID_ANY, U8("Curseur : —"));
  m_cursor->SetFont(m_cursor->GetFont().Bold());
  auto* top = new wxBoxSizer(wxVERTICAL);
  top->Add(m_tabs, 1, wxEXPAND | wxALL, 4);
  top->Add(m_cursor, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, 8);
  SetSizer(top);
  SetSize(FromDIP(wxSize(480, 640)));
  int tab = m_pi->Settings().panelTab;
  if (tab >= (int)m_tabs->GetPageCount()) tab = (int)m_tabs->GetPageCount() - 1;  // onglets d'avant
  if (tab >= 0) m_tabs->SetSelection(tab);
  FitPages();
  // largeur changée : textes longs recoupés à la nouvelle largeur
  Bind(wxEVT_SIZE, [this](wxSizeEvent& e) {
    e.Skip();
    CallAfter([this] {
      if (m_tideInfo && !m_tideRaw.empty()) SetWrapped(m_tideInfo, m_tideRaw);
      if (m_dbStatus && !m_dbRaw.empty()) SetWrapped(m_dbStatus, m_dbRaw);
      if (m_imgStatus && !m_imgRaw.empty()) SetWrapped(m_imgStatus, m_imgRaw);
      if (m_datumInfo && !m_datumRaw.empty()) SetWrapped(m_datumInfo, m_datumRaw);
    });
  });
  m_tabs->Bind(wxEVT_NOTEBOOK_PAGE_CHANGED, [this](wxBookCtrlEvent& e) {
    m_pi->Settings().panelTab = e.GetSelection();
    e.Skip();
  });
  Bind(wxEVT_CLOSE_WINDOW, &BathyDialog::OnClose, this);
  RefreshUnits();  // libellés d'unité, puis valeurs
  RefreshDb();
}

// Taille du panneau : assez grande pour que chaque onglet (Données, Affichage…)
// tienne sans défilement, dans les limites de l'écran
void BathyDialog::SelectTab(int tab) {
  if (tab >= 0 && tab < (int)m_tabs->GetPageCount()) m_tabs->SetSelection(tab);
}

void BathyDialog::FitPages() {
  wxSize need(0, 0);
  for (size_t k = 0; k < m_tabs->GetPageCount(); ++k) {
    wxWindow* page = m_tabs->GetPage(k);
    if (page->GetSizer()) need.IncTo(page->GetSizer()->GetMinSize() + FromDIP(wxSize(8, 8)));
  }
  const wxSize nb = m_tabs->CalcSizeFromPage(need);
  wxSize target = ClientToWindowSize(wxSize(nb.x + FromDIP(8), nb.y + m_cursor->GetBestSize().y + FromDIP(20)));
  target.IncTo(GetSize());
  const int d = wxDisplay::GetFromWindow(this);
  const wxRect area = wxDisplay(d == wxNOT_FOUND ? 0 : (unsigned)d).GetClientArea();
  target.DecTo(area.GetSize());
  wxPoint pos = GetPosition();
  if (pos.x + target.x > area.GetRight()) pos.x = std::max(area.GetLeft(), area.GetRight() - target.x);
  if (pos.y + target.y > area.GetBottom()) pos.y = std::max(area.GetTop(), area.GetBottom() - target.y);
  SetSize(wxRect(pos, target));
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
  {
    auto* chartBtn = new wxButton(sb, wxID_ANY, U8("Courbe de marée…"));
    chartBtn->SetToolTip(U8("Hauteur d'eau en fonction du temps pour chaque point de marée utilisé"));
    chartBtn->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { m_pi->ShowTideChart(); });
    box->Add(chartBtn, 0, wxALL, 4);
  }

  const DisplaySettings& s = m_pi->Settings();
  m_tideMode->SetSelection(std::max(0, std::min(2, s.tideMode)));
  m_tideSrc->SetSelection(std::max(0, std::min(1, s.tideSource)));
  m_tideAtlas->SetValue(W8(s.tideAtlas.c_str()));
  m_tideZ0->SetValue(W8(s.tideZ0Files.c_str()));
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

// Texte de longueur variable dans un onglet : coupé à la largeur de la page,
// puis la page (fenêtre défilante) est réorganisée, sinon les contrôles placés
// dessous restent à leur ancienne place, cachés sous le texte.
void BathyDialog::SetWrapped(wxStaticText* w, const wxString& t) {
  wxWindow* page = w->GetParent();
  while (page && !wxDynamicCast(page, wxScrolledWindow)) page = page->GetParent();
  int width = page ? page->GetClientSize().x - FromDIP(40) : FromDIP(400);
  // onglet Affichage : le cadre est à droite des couches affichées (largeur
  // tirée de la page, pas du cadre, qui suit lui-même son texte)
  if (w == m_datumInfo && m_layersBox) width -= m_layersBox->GetSize().x + FromDIP(8);
  w->SetLabel(t);
  w->Wrap(std::max(FromDIP(200), width));
  RelayoutPage(w);
}

void BathyDialog::RelayoutPage(wxWindow* w) {
  wxWindow* page = w ? w->GetParent() : nullptr;
  while (page && !wxDynamicCast(page, wxScrolledWindow)) page = page->GetParent();
  if (auto* sw = wxDynamicCast(page, wxScrolledWindow)) {
    sw->Layout();
    sw->FitInside();
  }
  Layout();
}

void BathyDialog::SetTideInfo(const wxString& t) {
  if (m_tideInfo && m_tideRaw != t) { m_tideRaw = t; SetWrapped(m_tideInfo, t); }
}

// ------------------------------------------------------ base de bathymétrie
namespace {
struct CellItem : public wxClientData {
  explicit CellItem(const std::string& n) : name(n) {}
  std::string name;
};
wxString CellBytes(long long b) {
  if (b >= (1LL << 30)) return wxString::Format(U8("%.2f Go"), b / double(1LL << 30));
  if (b >= (1LL << 20)) return wxString::Format(U8("%.0f Mo"), b / double(1LL << 20));
  return wxString::Format(U8("%.0f ko"), b / 1024.0);
}
}  // namespace

// Onglet Données : en haut le dossier de la base (bathymétrie et imagerie),
// l'état et l'arrêt de l'opération en cours ; à gauche la bathymétrie et le
// trait de côte ; à droite l'imagerie et les surfaces de référence verticale
wxSizer* BathyDialog::BuildDataBox(wxWindow* p) {
  auto* col = new wxBoxSizer(wxVERTICAL);
  wxSizer* bathy = BuildDbBox(p);  // crée aussi le cadre du dossier (m_dbTopBox)
  col->Add(m_dbTopBox, 0, wxEXPAND | wxALL, 2);
  auto* cols = new wxBoxSizer(wxHORIZONTAL);
  auto* left = new wxBoxSizer(wxVERTICAL);
  auto* right = new wxBoxSizer(wxVERTICAL);
  left->Add(bathy, 1, wxEXPAND | wxALL, 2);
  left->Add(BuildLandBox(p), 0, wxEXPAND | wxALL, 2);
  right->Add(BuildImageryBox(p), 1, wxEXPAND);
  right->Add(BuildDatumBox(p), 0, wxEXPAND | wxALL, 2);
  cols->Add(left, 1, wxEXPAND);
  cols->Add(right, 1, wxEXPAND);
  col->Add(cols, 1, wxEXPAND);
  return col;
}

wxSizer* BathyDialog::BuildDbBox(wxWindow* p) {
  // dossier de la base, commun à la bathymétrie et à l'imagerie
  auto* top = new wxStaticBoxSizer(wxVERTICAL, p, U8("Dossier de la base (bathymétrie .pdb, imagerie .pdi)"));
  m_dbTopBox = top;
  wxWindow* tb = top->GetStaticBox();
  auto* r1 = new wxBoxSizer(wxHORIZONTAL);
  m_dbFolder = new wxTextCtrl(tb, wxID_ANY, W8(m_pi->Settings().dbFolder.c_str()), wxDefaultPosition,
                              wxDefaultSize, wxTE_PROCESS_ENTER);
  m_dbFolder->SetHint(U8("dossier de la base (fichiers PD_*.pdb, PD_*.pdi)"));
  m_dbFolderBtn = new wxButton(tb, wxID_ANY, U8("…"), wxDefaultPosition, wxSize(FromDIP(28), -1));
  m_dbStop = new wxButton(tb, wxID_ANY, U8("Arrêter"));
  m_dbStop->SetToolTip(U8("Arrêter l'opération en cours sur la base (import, nettoyage…). "
                          "Pour un import, les fichiers déjà terminés restent dans la base."));
  r1->Add(m_dbFolder, 1, wxALIGN_CENTER_VERTICAL | wxRIGHT, 2);
  r1->Add(m_dbFolderBtn, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 8);
  r1->Add(m_dbStop, 0, wxALIGN_CENTER_VERTICAL);
  top->Add(r1, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, 4);
  m_dbStatus = new wxStaticText(tb, wxID_ANY, "");
  top->Add(m_dbStatus, 0, wxEXPAND | wxALL, 4);

  auto* box = new wxStaticBoxSizer(wxVERTICAL, p, U8("Bathymétrie"));
  wxWindow* sb = box->GetStaticBox();

  // fichiers .pdb (cellules géographiques) : affichés ou non
  m_cells = new wxTreeListCtrl(sb, wxID_ANY, wxDefaultPosition, wxSize(-1, FromDIP(150)),
                               wxTL_MULTIPLE | wxTL_CHECKBOX);
  m_cells->AppendColumn(U8("Fichier .pdb"), FromDIP(170));
  m_cells->AppendColumn(U8("Taille"), FromDIP(70), wxALIGN_RIGHT);
  m_cells->AppendColumn(U8("Sources"), FromDIP(60), wxALIGN_RIGHT);
  m_cells->SetToolTip(U8("Case : afficher ou masquer la bathymétrie de ce fichier (sa cellule "
                         "géographique). Les sources et leur import se gèrent dans "
                         "« Gestion des données »."));
  box->Add(m_cells, 1, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, 4);

  auto* r3 = new wxBoxSizer(wxHORIZONTAL);
  m_dbManage = new wxButton(sb, wxID_ANY, U8("Gestion des données (import)…"));
  m_dbManage->SetToolTip(U8("Importer des fichiers, voir les sources importées et leurs "
                            "métadonnées, priorités, classes, nettoyage, découpage des fichiers."));
  m_cellAll = new wxButton(sb, wxID_ANY, U8("Tout"), wxDefaultPosition, wxSize(FromDIP(44), -1));
  m_cellNone = new wxButton(sb, wxID_ANY, U8("Aucun"), wxDefaultPosition, wxSize(FromDIP(52), -1));
  m_cellZoom = new wxButton(sb, wxID_ANY, U8("Centrer"));
  m_cellZoom->SetToolTip(U8("Centrer la carte sur les fichiers sélectionnés"));
  r3->Add(m_dbManage, 0, wxRIGHT, 8);
  r3->AddStretchSpacer();
  r3->Add(m_cellAll, 0, wxRIGHT, 2);
  r3->Add(m_cellNone, 0, wxRIGHT, 8);
  r3->Add(m_cellZoom, 0);
  box->Add(r3, 0, wxEXPAND | wxALL, 4);

  auto setFolder = [this]() {
    const std::string f = m_dbFolder->GetValue().Trim().Trim(false).ToUTF8().data();
    if (f == m_pi->Settings().dbFolder) return;
    if (m_pi->DbBusy()) { m_dbFolder->SetValue(W8(m_pi->Settings().dbFolder.c_str())); return; }
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

// ------------------------------------------------------ référence verticale
// Référence verticale des profondeurs affichées (onglet Affichage)
wxSizer* BathyDialog::BuildDispDatumBox(wxWindow* p) {
  auto* box = new wxStaticBoxSizer(wxVERTICAL, p, U8("Référence verticale des profondeurs"));
  wxWindow* sb = box->GetStaticBox();
  const DisplaySettings& s = m_pi->Settings();
  wxString refs[] = {U8("Zéro des cartes officiel du pays, comme les cartes marines"),
                     U8("Plus basse mer astronomique (LAT)"), U8("Niveau moyen (NM)"),
                     U8("Ellipsoïde WGS84 (hauteur du fond)")};
  m_dispDatum = new wxChoice(sb, wxID_ANY, wxDefaultPosition, wxDefaultSize, 4, refs);
  m_dispDatum->SetSelection(std::max(0, std::min(3, s.dispDatum)));
  m_dispDatum->SetToolTip(U8("Référence des profondeurs affichées (couleurs, sondes, isobathes). "
                             "La base range les profondeurs sous le niveau moyen ; chaque source est "
                             "ramenée à la référence choisie. Avec la marée : hauteur d'eau réelle "
                             "(zéro des cartes + marée)."));
  // liste sans largeur imposée par le libellé le plus long : le cadre suit la fenêtre
  m_dispDatum->SetMinSize(wxSize(FromDIP(200), -1));
  box->Add(m_dispDatum, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, 4);
  m_datumInfo = new wxStaticText(sb, wxID_ANY, "");
  box->Add(m_datumInfo, 0, wxEXPAND | wxALL, 4);
  m_dispDatum->Bind(wxEVT_CHOICE, [this](wxCommandEvent&) {
    m_pi->Settings().dispDatum = m_dispDatum->GetSelection();
    m_pi->DatumChanged();
    RefreshDatum();
  });
  return box;
}

wxSizer* BathyDialog::BuildDatumBox(wxWindow* p) {
  // fichiers des surfaces de référence (la référence affichée se choisit dans
  // l'onglet Affichage)
  auto* box = new wxStaticBoxSizer(wxVERTICAL, p, U8("Surfaces de référence verticale"));
  wxWindow* sb = box->GetStaticBox();
  const DisplaySettings& s = m_pi->Settings();
  auto* r2 = new wxBoxSizer(wxHORIZONTAL);
  m_mss = new wxTextCtrl(sb, wxID_ANY, W8(s.mssGrid.c_str()), wxDefaultPosition, wxDefaultSize,
                         wxTE_PROCESS_ENTER);
  m_mss->SetHint(U8("surface moyenne océanique : MSS_CNES_CLS_2025.pdgrid"));
  m_mss->SetToolTip(U8("Niveau moyen au-dessus de l'ellipsoïde hors France (hauteurs GNSS, affichage "
                       "à l'ellipsoïde). Fichier converti par tools/pdgrid_convert.py depuis la MSS "
                       "CNES_CLS d'AVISO (voir docs/external-data.md)."));
  m_mssBtn = new wxButton(sb, wxID_ANY, U8("…"), wxDefaultPosition, wxSize(FromDIP(28), -1));
  r2->Add(new wxStaticText(sb, wxID_ANY, U8("MSS")), 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 4);
  r2->Add(m_mss, 1, wxRIGHT, 2);
  r2->Add(m_mssBtn, 0);
  box->Add(r2, 0, wxEXPAND | wxLEFT | wxRIGHT, 4);
  // zéros des cartes officiels hors France : un dossier, reconnu tout seul
  auto* r4 = new wxBoxSizer(wxHORIZONTAL);
  m_national = new wxTextCtrl(sb, wxID_ANY, W8(s.nationalDir.c_str()), wxDefaultPosition, wxDefaultSize,
                              wxTE_PROCESS_ENTER);
  m_national->SetHint(U8("dossier des fichiers officiels (NOAA VDatum…)"));
  m_national->SetToolTip(U8("Zéro des cartes officiel des autres pays : déposer dans ce dossier les fichiers "
                            "téléchargés (régions NOAA VDatum des États-Unis…), le plugin les reconnaît et "
                            "les utilise là où ils existent ; ailleurs, plus basse mer de la marée FES. "
                            "Liens et licences : docs/external-data.md."));
  m_nationalBtn = new wxButton(sb, wxID_ANY, U8("…"), wxDefaultPosition, wxSize(FromDIP(28), -1));
  r4->Add(new wxStaticText(sb, wxID_ANY, U8("Pays")), 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 4);
  r4->Add(m_national, 1, wxRIGHT, 2);
  r4->Add(m_nationalBtn, 0);
  box->Add(r4, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, 4);
  auto* r3 = new wxBoxSizer(wxHORIZONTAL);
  m_datumUpdate = new wxButton(sb, wxID_ANY, U8("Mettre à jour les surfaces"));
  m_datumUpdate->SetToolTip(U8("Recalcule les surfaces de référence de la base avec les fichiers "
                               "actuels (BathyElli, FES2022, MSS, pays) et corrige les sources concernées. "
                               "À lancer après avoir ajouté ou changé un de ces fichiers."));
  m_datumMigrate = new wxButton(sb, wxID_ANY, U8("Migrer la base…"));
  m_datumMigrate->SetToolTip(U8("Base d'une version antérieure : profondeurs ramenées au niveau moyen "
                                "(copie de sauvegarde d'abord)."));
  r3->Add(m_datumUpdate, 0, wxRIGHT, 8);
  r3->Add(m_datumMigrate, 0);
  box->Add(r3, 0, wxALL, 4);

  auto setMss = [this]() {
    const std::string f = m_mss->GetValue().Trim().Trim(false).ToUTF8().data();
    if (f == m_pi->Settings().mssGrid) return;
    m_pi->Settings().mssGrid = f;
    m_pi->DatumChanged();
    RefreshDatum();
  };
  m_mss->Bind(wxEVT_TEXT_ENTER, [setMss](wxCommandEvent&) { setMss(); });
  m_mss->Bind(wxEVT_KILL_FOCUS, [setMss](wxFocusEvent& e) { setMss(); e.Skip(); });
  m_mssBtn->Bind(wxEVT_BUTTON, [this, setMss](wxCommandEvent&) {
    wxFileDialog fd(this, U8("Surface moyenne océanique (MSS)"), "", m_mss->GetValue(),
                    U8("Grille PersonalDepth (*.pdgrid)|*.pdgrid|Tous|*"), wxFD_OPEN | wxFD_FILE_MUST_EXIST);
    if (fd.ShowModal() != wxID_OK) return;
    m_mss->SetValue(fd.GetPath());
    setMss();
  });
  auto setNational = [this]() {
    const std::string f = m_national->GetValue().Trim().Trim(false).ToUTF8().data();
    if (f == m_pi->Settings().nationalDir) return;
    m_pi->Settings().nationalDir = f;
    m_pi->NationalChanged();
    RefreshDatum();
  };
  m_national->Bind(wxEVT_TEXT_ENTER, [setNational](wxCommandEvent&) { setNational(); });
  m_national->Bind(wxEVT_KILL_FOCUS, [setNational](wxFocusEvent& e) { setNational(); e.Skip(); });
  m_nationalBtn->Bind(wxEVT_BUTTON, [this, setNational](wxCommandEvent&) {
    wxDirDialog dd(this, U8("Fichiers officiels des pays"), m_national->GetValue());
    if (dd.ShowModal() != wxID_OK) return;
    m_national->SetValue(dd.GetPath());
    setNational();
  });
  m_datumUpdate->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) {
    m_pi->DbUpdateDatum();
    RefreshDb();
  });
  m_datumMigrate->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) {
    m_pi->DbMigrate(true);
    RefreshDb();
  });
  return box;
}

// ------------------------------------------------------------ trait de côte
wxSizer* BathyDialog::BuildLandBox(wxWindow* p) {
  auto* box = new wxStaticBoxSizer(wxVERTICAL, p, U8("Trait de côte"));
  wxWindow* sb = box->GetStaticBox();
  const DisplaySettings& s = m_pi->Settings();
  m_landOn = new wxCheckBox(sb, wxID_ANY, U8("Pas d'interpolation sur la terre"));
  m_landOn->SetValue(s.landMaskOn);
  m_landOn->SetToolTip(U8("Pas de surface interpolée sur la terre (îles, côte) : à l'import, les pixels interpolés situés sur la terre sont retirés ; les "
                          "sondes mesurées sont toujours gardées."));
  box->Add(m_landOn, 0, wxALL, 4);
  auto* r = new wxBoxSizer(wxHORIZONTAL);
  m_land = new wxTextCtrl(sb, wxID_ANY, W8(s.landMask.c_str()), wxDefaultPosition, wxDefaultSize,
                          wxTE_PROCESS_ENTER);
  m_land->SetHint(U8("dossier des fonds de carte OSMSHP ou GSHHG d'OpenCPN"));
  m_land->SetToolTip(U8("Polygones de terre téléchargés par le gestionnaire de cartes d'OpenCPN : "
                        "OSMSHP (basemap_*.shp, OpenStreetMap, conseillé) ou GSHHG (poly-*-1.dat, "
                        "plus grossier). La meilleure qualité présente est prise."));
  m_landBtn = new wxButton(sb, wxID_ANY, U8("…"), wxDefaultPosition, wxSize(FromDIP(28), -1));
  r->Add(new wxStaticText(sb, wxID_ANY, U8("Dossier")), 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 4);
  r->Add(m_land, 1, wxRIGHT, 2);
  r->Add(m_landBtn, 0);
  box->Add(r, 0, wxEXPAND | wxLEFT | wxRIGHT, 4);
  // traits de côte personnels (KML Google Earth, shapefile, GeoPackage)
  auto* r3 = new wxBoxSizer(wxHORIZONTAL);
  m_shore = new wxTextCtrl(sb, wxID_ANY, W8(s.shoreFiles.c_str()), wxDefaultPosition, wxDefaultSize,
                           wxTE_PROCESS_ENTER);
  m_shore->SetHint(U8("traits personnels .kml .kmz .shp .gpkg (séparer par ;)"));
  m_shore->SetToolTip(U8("Rivages dessinés ou extraits par l'utilisateur, en plus du fond de carte : "
                         "polygones (lac et ses îles, îles, côte) ou lignes. Le côté eau des polygones "
                         "est celui des sondes : un polygone qui entoure les sondes trace l'eau, un "
                         "polygone sans sondes trace la terre."));
  m_shoreBtn = new wxButton(sb, wxID_ANY, U8("…"), wxDefaultPosition, wxSize(FromDIP(28), -1));
  r3->Add(new wxStaticText(sb, wxID_ANY, U8("Personnels")), 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 4);
  r3->Add(m_shore, 1, wxRIGHT, 2);
  r3->Add(m_shoreBtn, 0);
  box->Add(r3, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, 4);
  m_shoreZero = new wxCheckBox(sb, wxID_ANY, U8("Lacs : rivage = isobathe 0"));
  m_shoreZero->SetValue(s.shoreZero);
  m_shoreZero->SetToolTip(U8("Plans d'eau non rattachés : rivage = isobathe 0 de l'interpolation. Sources « non rattachées » (lac, retenue) : à l'import, la surface "
                             "interpolée descend vers 0 m au rivage au lieu de s'arrêter à la dernière "
                             "sonde. Aucune sonde n'est ajoutée ; la distance D d'interpolation reste "
                             "la limite."));
  box->Add(m_shoreZero, 0, wxALL, 4);
  auto* r2 = new wxBoxSizer(wxHORIZONTAL);
  m_landInfo = new wxStaticText(sb, wxID_ANY, "");
  m_landApply = new wxButton(sb, wxID_ANY, U8("Appliquer aux sources"));
  m_landApply->SetToolTip(U8("Retire des sources déjà importées les pixels interpolés situés sur la "
                             "terre. Pour revenir en arrière : réimporter la source."));
  r2->Add(m_landInfo, 1, wxALIGN_CENTER_VERTICAL | wxRIGHT, 4);
  r2->Add(m_landApply, 0, wxALIGN_CENTER_VERTICAL);
  box->Add(r2, 0, wxEXPAND | wxALL, 4);
  auto setPath = [this]() {
    const std::string f = m_land->GetValue().Trim().Trim(false).ToUTF8().data();
    if (f == m_pi->Settings().landMask) return;
    m_pi->Settings().landMask = f;
    m_pi->SaveConfig();
    RefreshLand();
  };
  m_land->Bind(wxEVT_TEXT_ENTER, [setPath](wxCommandEvent&) { setPath(); });
  m_land->Bind(wxEVT_KILL_FOCUS, [setPath](wxFocusEvent& e) { setPath(); e.Skip(); });
  m_landBtn->Bind(wxEVT_BUTTON, [this, setPath](wxCommandEvent&) {
    wxDirDialog dd(this, U8("Dossier des fonds de carte OSMSHP ou GSHHG"), m_land->GetValue());
    if (dd.ShowModal() != wxID_OK) return;
    m_land->SetValue(dd.GetPath());
    setPath();
  });
  auto setShore = [this]() {
    const std::string f = m_shore->GetValue().Trim().Trim(false).ToUTF8().data();
    if (f == m_pi->Settings().shoreFiles) return;
    m_pi->Settings().shoreFiles = f;
    m_pi->SaveConfig();
    RefreshLand();
  };
  m_shore->Bind(wxEVT_TEXT_ENTER, [setShore](wxCommandEvent&) { setShore(); });
  m_shore->Bind(wxEVT_KILL_FOCUS, [setShore](wxFocusEvent& e) { setShore(); e.Skip(); });
  m_shoreBtn->Bind(wxEVT_BUTTON, [this, setShore](wxCommandEvent&) {
    wxFileDialog fd(this, U8("Traits de côte personnels"), "", "",
                    U8("Traits de côte (*.kml;*.kmz;*.shp;*.gpkg)|*.kml;*.KML;*.kmz;*.KMZ;*.shp;*.SHP;*.gpkg;*.GPKG|"
                       "Tous les fichiers|*"),
                    wxFD_OPEN | wxFD_FILE_MUST_EXIST | wxFD_MULTIPLE);
    if (fd.ShowModal() != wxID_OK) return;
    wxArrayString ps;
    fd.GetPaths(ps);
    wxString v = m_shore->GetValue().Trim().Trim(false);
    for (const wxString& f : ps)
      if (!v.Contains(f)) v += (v.empty() ? "" : ";") + f;
    m_shore->SetValue(v);
    setShore();
  });
  m_shoreZero->Bind(wxEVT_CHECKBOX, [this](wxCommandEvent&) {
    m_pi->Settings().shoreZero = m_shoreZero->GetValue();
    m_pi->SaveConfig();
  });
  m_landOn->Bind(wxEVT_CHECKBOX, [this](wxCommandEvent&) {
    m_pi->Settings().landMaskOn = m_landOn->GetValue();
    m_pi->SaveConfig();
    RefreshLand();
  });
  m_landApply->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) {
    if (wxMessageBox(U8("Retirer de toutes les sources les pixels interpolés situés sur la terre ?\n"
                        "Les sondes mesurées sont gardées. Pour retrouver l'interpolation retirée, "
                        "il faudra réimporter la source."),
                     "PersonalDepth", wxYES_NO | wxICON_QUESTION, this) != wxYES)
      return;
    m_pi->DbApplyLandMask();
    RefreshDb();
  });
  return box;
}

void BathyDialog::RefreshLand() {
  if (!m_landInfo) return;
  std::string name;
  const std::string file = m_pi->LandMaskFile(&name);
  const bool busy = m_pi->DbBusy();
  const auto cfg = m_pi->LandConfig();
  wxString used = file.empty() ? wxString() : W8(name.c_str());
  if (!cfg.custom.empty())
    used += (used.empty() ? wxString() : W8(" + ")) +
            wxString::Format(U8("%d trait(s) personnel(s)"), (int)cfg.custom.size());
  m_landInfo->SetLabel(!m_pi->Settings().landMaskOn ? U8("désactivé")
                       : used.empty() ? (m_pi->Settings().landMask.empty() ? U8("aucun dossier choisi")
                                                                           : U8("aucun trait de côte reconnu"))
                                      : U8("utilisé : ") + used);
  m_landApply->Enable(!busy && cfg.any() && !m_pi->DbSources().empty() && !m_pi->DbNeedsMigration());
  m_land->Enable(!busy);
  m_landBtn->Enable(!busy);
  m_shore->Enable(!busy);
  m_shoreBtn->Enable(!busy);
}

void BathyDialog::RefreshDatum() {
  if (!m_datumInfo) return;
  const bool busy = m_pi->DbBusy(), legacy = m_pi->DbNeedsMigration();
  const bool has = !m_pi->DbSources().empty();
  m_datumUpdate->Enable(!busy && has && !legacy);
  m_datumMigrate->Show(legacy);
  m_datumMigrate->Enable(!busy);
  const wxString t = m_pi->DatumInfo();
  if (t != m_datumRaw) { m_datumRaw = t; SetWrapped(m_datumInfo, t); }
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
        m_cells->AppendItem(m_cells->GetRootItem(), W8(c.name.c_str()), -1, -1, new CellItem(c.name));
    m_cells->SetItemText(it, 1, CellBytes(c.bytes));
    m_cells->SetItemText(it, 2, wxString::Format("%d", c.sources));
    m_cells->CheckItem(it, hidden.count(c.name) ? wxCHK_UNCHECKED : wxCHK_CHECKED);
    if (sel.count(c.name)) m_cells->Select(it);
  }
  UpdateDbButtons();
}

void BathyDialog::RefreshDb() {
  RefreshCells();
  RefreshDatum();
  RefreshLand();
  SetDbStatus(m_pi->DbStatus());
  RefreshImagery();
  if (m_data) m_data->RefreshDb();
}

void BathyDialog::SetDbStatus(const wxString& t) {
  if (m_dbStatus && m_dbRaw != t) { m_dbRaw = t; SetWrapped(m_dbStatus, t); }
  if (m_dbStop) UpdateDbButtons();
  if (m_imgStatus && m_imgRaw != t) { m_imgRaw = t; SetWrapped(m_imgStatus, t); }
  if (m_imgStop) UpdateImgButtons();
  if (m_data && m_data->IsShown()) m_data->SetDbStatus(t);
}

// ---------------------------------------------------------------- Palette
wxSizer* BathyDialog::BuildPaletteBox(wxWindow* p) {
  auto* box = new wxStaticBoxSizer(wxVERTICAL, p, U8("Palette et seuils de profondeur"));
  wxWindow* sb = box->GetStaticBox();
  auto* row = new wxBoxSizer(wxHORIZONTAL);
  m_preset = new wxChoice(sb, wxID_ANY);
  RefreshPresetList();
  wxString modes[] = {U8("Dégradé continu"), U8("Paliers")};
  m_mode = new wxRadioBox(sb, wxID_ANY, "", wxDefaultPosition, wxDefaultSize, 2, modes, 2);
  row->Add(m_preset, 1, wxALIGN_CENTER_VERTICAL | wxRIGHT, 6);
  row->Add(m_mode, 0);
  box->Add(row, 0, wxEXPAND | wxALL, 4);

  // Palette automatique : seuils d'après les profondeurs présentes
  auto* ar = new wxBoxSizer(wxHORIZONTAL);
  m_autoOn = new wxCheckBox(sb, wxID_ANY, U8("Auto"));
  m_autoOn->SetToolTip(U8("Palette automatique : seuils répartis entre la profondeur minimale et maximale, couleurs "
                          "reprises de la palette choisie. Modifier un seuil à la main "
                          "désactive le mode automatique."));
  wxString srcs[] = {U8("toutes les données"), U8("zone affichée")};
  m_autoSrc = new wxChoice(sb, wxID_ANY, wxDefaultPosition, wxDefaultSize, 2, srcs);
  m_autoSrc->SetToolTip(U8("Toutes les données : base et couches visibles. Zone affichée : "
                           "recalculé quand la carte se déplace."));
  m_autoCount = new wxSpinCtrl(sb, wxID_ANY, "", wxDefaultPosition, wxSize(FromDIP(60), -1),
                               wxSP_ARROW_KEYS, 2, 40, 10);
  m_autoNice = new wxCheckBox(sb, wxID_ANY, U8("arrondis"));
  m_autoNice->SetToolTip(U8("Valeurs rondes : seuils multiples de 1, 2, 2,5 ou 5 × 10ⁿ (sinon répartis exactement)"));
  ar->Add(m_autoOn, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 6);
  ar->Add(m_autoSrc, 1, wxALIGN_CENTER_VERTICAL | wxRIGHT, 6);
  ar->Add(new wxStaticText(sb, wxID_ANY, U8("seuils")), 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 4);
  ar->Add(m_autoCount, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 6);
  ar->Add(m_autoNice, 0, wxALIGN_CENTER_VERTICAL);
  box->Add(ar, 0, wxEXPAND | wxLEFT | wxRIGHT, 4);
  auto* br = new wxBoxSizer(wxHORIZONTAL);
  m_autoMinOn = new wxCheckBox(sb, wxID_ANY, U8("min"));
  m_autoMinOn->SetToolTip(U8("Min imposé : premier seuil fixé (ex. 0 m : palette sur la seule bathymétrie, "
                             "les altitudes gardent une seule couleur)"));
  m_autoMin = new wxSpinCtrlDouble(sb, wxID_ANY, "", wxDefaultPosition, wxSize(FromDIP(80), -1),
                                   wxSP_ARROW_KEYS, -500, 11000, 0, 0.5);
  m_autoMin->SetDigits(1);
  m_autoMaxOn = new wxCheckBox(sb, wxID_ANY, U8("max"));
  m_autoMaxOn->SetToolTip(U8("Max imposé : dernier seuil fixé : les fonds plus profonds gardent la dernière couleur"));
  m_autoMax = new wxSpinCtrlDouble(sb, wxID_ANY, "", wxDefaultPosition, wxSize(FromDIP(80), -1),
                                   wxSP_ARROW_KEYS, -500, 11000, 50, 0.5);
  m_autoMax->SetDigits(1);
  br->Add(m_autoMinOn, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 4);
  br->Add(m_autoMin, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 2);
  m_autoMinU = new wxStaticText(sb, wxID_ANY, "m");
  br->Add(m_autoMinU, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 12);
  br->Add(m_autoMaxOn, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 4);
  br->Add(m_autoMax, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 2);
  m_autoMaxU = new wxStaticText(sb, wxID_ANY, "m");
  br->Add(m_autoMaxU, 0, wxALIGN_CENTER_VERTICAL);
  box->Add(br, 0, wxLEFT | wxRIGHT | wxTOP, 4);
  // résumé de la palette automatique : une ligne, texte complet en bulle
  m_autoInfo = new wxStaticText(sb, wxID_ANY, "", wxDefaultPosition, wxDefaultSize,
                                wxST_ELLIPSIZE_END | wxST_NO_AUTORESIZE);
  m_autoInfo->SetMinSize(wxSize(FromDIP(100), -1));
  box->Add(m_autoInfo, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, 4);
  {
    const DisplaySettings& ds = m_pi->Settings();
    m_autoOn->SetValue(ds.autoPalette);
    m_autoSrc->SetSelection(std::max(0, std::min(1, ds.autoSource)));
    m_autoCount->SetValue(ds.autoCount);
    m_autoNice->SetValue(ds.autoNice);
    m_autoMinOn->SetValue(ds.autoMinOn);
    m_autoMaxOn->SetValue(ds.autoMaxOn);
  }
  m_autoMinOn->Bind(wxEVT_CHECKBOX, &BathyDialog::OnAuto, this);
  m_autoMaxOn->Bind(wxEVT_CHECKBOX, &BathyDialog::OnAuto, this);
  m_autoMin->Bind(wxEVT_SPINCTRLDOUBLE, [this](wxSpinDoubleEvent&) { wxCommandEvent e; OnAuto(e); });
  m_autoMax->Bind(wxEVT_SPINCTRLDOUBLE, [this](wxSpinDoubleEvent&) { wxCommandEvent e; OnAuto(e); });
  m_autoOn->Bind(wxEVT_CHECKBOX, &BathyDialog::OnAuto, this);
  m_autoSrc->Bind(wxEVT_CHOICE, &BathyDialog::OnAuto, this);
  m_autoNice->Bind(wxEVT_CHECKBOX, &BathyDialog::OnAuto, this);
  m_autoCount->Bind(wxEVT_SPINCTRL, [this](wxSpinEvent&) { wxCommandEvent e; OnAuto(e); });

  // ~5 lignes visibles : le tableau défile seul
  m_stops = new wxGrid(sb, wxID_ANY, wxDefaultPosition, wxSize(-1, FromDIP(130)));
  m_stops->CreateGrid(0, 2);
  m_stops->SetColLabelValue(0, U8("Seuil (m)"));
  m_stops->SetColLabelValue(1, U8("Couleur (double-clic)"));
  m_stops->HideRowLabels();
  m_stops->SetColSize(0, FromDIP(110));
  m_stops->SetColSize(1, FromDIP(170));
  box->Add(m_stops, 1, wxEXPAND | wxALL, 4);

  // boutons courts (texte complet en bulle) : deux lignes, la seconde
  // finie par la zone dangereuse
  auto* b = new wxBoxSizer(wxHORIZONTAL);
  auto* plus = new wxButton(sb, wxID_ANY, U8("+"), wxDefaultPosition, wxDefaultSize, wxBU_EXACTFIT);
  plus->SetToolTip(U8("Ajouter un seuil"));
  auto* minus = new wxButton(sb, wxID_ANY, U8("−"), wxDefaultPosition, wxDefaultSize, wxBU_EXACTFIT);
  minus->SetToolTip(U8("Retirer le seuil sélectionné"));
  auto* imp = new wxButton(sb, wxID_ANY, U8("Importer…"), wxDefaultPosition, wxDefaultSize, wxBU_EXACTFIT);
  imp->SetToolTip(U8("Importer une palette GMT (.cpt)"));
  auto* exp = new wxButton(sb, wxID_ANY, U8("Exporter…"), wxDefaultPosition, wxDefaultSize, wxBU_EXACTFIT);
  exp->SetToolTip(U8("Exporter la palette en GMT (.cpt)"));
  b->Add(plus, 0, wxRIGHT, 2); b->Add(minus, 0, wxRIGHT, 8);
  b->Add(imp, 0, wxRIGHT, 2); b->Add(exp, 0, wxRIGHT, 8);
  box->Add(b, 0, wxLEFT | wxRIGHT | wxTOP, 4);
  // Palettes de l'utilisateur : un fichier .cpt par palette dans un dossier
  auto* pb = new wxBoxSizer(wxHORIZONTAL);
  auto* psave = new wxButton(sb, wxID_ANY, U8("Enregistrer…"), wxDefaultPosition,
                             wxDefaultSize, wxBU_EXACTFIT);
  psave->SetToolTip(U8("Enregistre la palette affichée (seuils, couleurs, mode, zone dangereuse) "
                       "dans le dossier de palettes : elle apparaît ensuite dans la liste et dans "
                       "le menu du bandeau."));
  auto* pdir = new wxButton(sb, wxID_ANY, U8("Dossier…"), wxDefaultPosition,
                            wxDefaultSize, wxBU_EXACTFIT);
  pdir->SetToolTip(U8("Dossier de palettes : choisir le dossier des palettes (.cpt) : toutes les palettes proposées (sauf "
                      "ENC S-52) sont les fichiers de ce dossier, palettes d'origine comprises. On "
                      "peut les modifier, les renommer, les supprimer, y déposer des palettes GMT."));
  auto* prest = new wxButton(sb, wxID_ANY, U8("Restaurer"), wxDefaultPosition,
                             wxDefaultSize, wxBU_EXACTFIT);
  prest->SetToolTip(U8("Restaurer les palettes d'origine : réécrit dans le dossier les palettes livrées avec le plugin (remplace les "
                       "fichiers du même nom)."));
  b->Add(psave, 0);
  pb->Add(pdir, 0, wxRIGHT, 2); pb->Add(prest, 0, wxRIGHT, 8);
  pb->AddStretchSpacer();
  prest->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) {
    if (wxMessageBox(U8("Réécrire les palettes d'origine dans le dossier de palettes ? Les fichiers "
                        "du même nom sont remplacés."),
                     "PersonalDepth", wxYES_NO | wxICON_QUESTION, this) != wxYES)
      return;
    wxString e;
    if (!m_pi->WriteBuiltinPalettes(true, &e)) wxMessageBox(e, "PersonalDepth", wxICON_WARNING, this);
    RefreshPresetList();
  });
  psave->Bind(wxEVT_BUTTON, &BathyDialog::OnSavePalette, this);
  pdir->Bind(wxEVT_BUTTON, &BathyDialog::OnPaletteDir, this);

  wxSizer* s = pb;
  m_safeOn = new wxCheckBox(sb, wxID_ANY, U8("Danger <"));
  m_safeOn->SetToolTip(U8("Zone dangereuse : surcouche des fonds moins profonds que cette valeur "
                          "(tirant d'eau et marge). L'isobathe de sécurité (rouge) la suit."));
  m_safeDepth = new wxSpinCtrlDouble(sb, wxID_ANY, "", wxDefaultPosition, wxSize(FromDIP(70), -1),
                                     wxSP_ARROW_KEYS, -20, 200, 2, 0.1);
  m_safeDepth->SetDigits(1);
  m_safeColour = new wxColourPickerCtrl(sb, wxID_ANY, *wxRED);
  s->Add(m_safeOn, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 4);
  s->Add(m_safeDepth, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 2);
  m_safeU = new wxStaticText(sb, wxID_ANY, "m");
  s->Add(m_safeU, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 4);
  s->Add(m_safeColour, 0, wxALIGN_CENTER_VERTICAL);
  box->Add(pb, 0, wxEXPAND | wxALL, 4);

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
  m_stops->SetCellValue(r, 0, wxString::FromCDouble(st.depth * K(), 2));
  m_stops->SetCellBackgroundColour(r, 1, ToWx(st.rgba | 0xFF000000u));
  m_stops->SetCellValue(r, 1, wxString::Format("#%02X%02X%02X", R_(st.rgba), G_(st.rgba), B_(st.rgba)));
  int lum = (R_(st.rgba) * 299 + G_(st.rgba) * 587 + B_(st.rgba) * 114) / 1000;
  m_stops->SetCellTextColour(r, 1, lum > 128 ? *wxBLACK : *wxWHITE);
  m_stops->SetReadOnly(r, 1, true);
}

float BathyDialog::K() const {
  const float k = m_pi->Settings().depthK;
  return k > 0 ? k : 1.f;
}

// Valeur saisie (unité affichée) -> mètres ; si l'utilisateur ne l'a pas
// changée (même valeur à l'arrondi affiché), la valeur stockée est gardée
// telle quelle pour ne pas dériver à chaque aller-retour m <-> ft
float BathyDialog::FromUi(double ui, float storedM, int digits) const {
  const double p = std::pow(10.0, digits);
  if (std::fabs(ui - std::round(storedM * K() * p) / p) < 0.5 / p) return storedM;
  return (float)(ui / K());
}

void BathyDialog::RefreshUnits() {
  const DisplaySettings& s = m_pi->Settings();
  const wxString u = s.depthUnit;
  const float k = K();
  m_stops->SetColLabelValue(0, U8("Seuil (") + u + ")");
  for (wxStaticText* t : {m_autoMinU, m_autoMaxU, m_safeU})
    if (t) t->SetLabel(u);
  if (m_ctStepL) m_ctStepL->SetLabel(U8("Intervalle (") + u + ")");
  if (m_ctMajorL) m_ctMajorL->SetLabel(U8("Maîtresse (") + u + ")");
  m_autoMin->SetRange(-500 * k, 11000 * k);
  m_autoMax->SetRange(-500 * k, 11000 * k);
  m_safeDepth->SetRange(-20 * k, 200 * k);
  m_ctStep->SetRange(0.1, 100 * k);
  m_ctMajor->SetRange(0, 500 * k);
  RefreshPalette();
  if (m_data) m_data->RefreshDb();
  Layout();
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
    m_autoSrc->SetSelection(std::max(0, std::min(1, ds.autoSource)));
    m_autoSrc->Enable(ds.autoPalette);
    m_autoCount->Enable(ds.autoPalette);
    m_autoNice->Enable(ds.autoPalette);
    m_autoMinOn->Enable(ds.autoPalette);
    m_autoMaxOn->Enable(ds.autoPalette);
    m_autoMin->Enable(ds.autoPalette && ds.autoMinOn);
    m_autoMax->Enable(ds.autoPalette && ds.autoMaxOn);
    const wxString info = ds.autoPalette ? m_pi->AutoPaletteInfo() : wxString();
    if (m_autoInfo->GetLabel() != info) {
      m_autoInfo->SetLabel(info);
      m_autoInfo->SetToolTip(info);
    }
  }
  m_safeOn->SetValue(r.safetyOn);
  m_safeDepth->SetValue(r.safetyDepth * K());
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
  m_banner->SetValue(s.banner);
  m_bannerPos->SetSelection(std::max(0, std::min(3, s.bannerPos)));
  m_bannerPos->Enable(s.banner);
  m_bannerLock->SetValue(s.bannerLock);
  m_interpShow->SetValue(s.interpShow);
  m_interpDistinct->SetValue(s.interpDistinct);
  m_interpDistinct->Enable(s.interpShow);
  m_bannerLock->Enable(s.banner);
  m_ctOn->SetValue(s.contours);
  if (m_toolbarMode) m_toolbarMode->SetSelection(s.toolbarMode == 1 ? 1 : 0);
  m_ctMode->SetSelection(s.contourMode);
  m_ctStep->SetValue(s.contourStep * K());
  m_ctMajor->SetValue(s.contourMajor * K());
  m_autoMin->SetValue(s.autoMin * K());
  m_autoMax->SetValue(s.autoMax * K());
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
  s.autoMin = FromUi(m_autoMin->GetValue(), s.autoMin, 1);
  s.autoMax = FromUi(m_autoMax->GetValue(), s.autoMax, 1);
  if (s.autoPalette && !was) {  // couleurs de référence : préréglage choisi, sinon palette actuelle
    // couleurs de référence : dernier préréglage choisi, sinon palette affichée
    if (s.autoBase.empty() || !s.autoBaseFresh) s.autoBase = s.ramp.serialize();
    s.autoBaseFresh = true;
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
  m_pi->Settings().autoBaseFresh = false;  // seuils / couleurs modifiés à la main
  ColorRamp& r = m_pi->Settings().ramp;
  std::vector<ColorStop> st;
  for (int k = 0; k < m_stops->GetNumberRows(); ++k) {
    double d;
    if (!ParseNum(m_stops->GetCellValue(k, 0), d)) continue;
    // seuil saisi dans l'unité affichée ; une ligne inchangée garde sa valeur exacte en m
    const float old = k < (int)r.stops.size() ? r.stops[k].depth : (float)(d / K());
    st.push_back({k < (int)r.stops.size() ? FromUi(d, old, 2) : (float)(d / K()),
                  FromWx(m_stops->GetCellBackgroundColour(k, 1))});
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
  if (k <= 0 || k > (int)m_presetKeys.size()) return;
  m_pi->ApplyPreset(m_presetKeys[k - 1]);
}

void BathyDialog::SelectPreset(const std::string& name) {
  for (size_t k = 0; k < m_presetKeys.size(); ++k)
    if (m_presetKeys[k] == name) { m_preset->SetSelection((int)k + 1); return; }
}

void BathyDialog::RefreshPresetList() {
  const std::string cur = m_preset->GetSelection() > 0 &&
                                  m_preset->GetSelection() <= (int)m_presetKeys.size()
                              ? m_presetKeys[m_preset->GetSelection() - 1] : std::string();
  m_preset->Clear();
  m_presetKeys.clear();
  m_preset->Append(U8("(personnalisée)"));
  for (const auto& e : m_pi->PaletteList()) {  // ENC (S-52), puis palettes du dossier
    m_preset->Append(e.label);
    m_presetKeys.push_back(e.key);
  }
  m_preset->SetSelection(0);
  if (!cur.empty()) SelectPreset(cur);
}

void BathyDialog::OnSavePalette(wxCommandEvent&) {
  DisplaySettings& s = m_pi->Settings();
  wxString name = wxGetTextFromUser(U8("Nom de la palette"), U8("Enregistrer la palette"),
                                    "", this);
  name.Trim(true).Trim(false);
  if (name.empty()) return;
  for (wxUniChar bad : wxString("\\/:*?\"<>|")) name.Replace(wxString(bad), "_");
  const wxString dir = W8(s.paletteDir.c_str());
  if (!wxDirExists(dir) && !wxFileName::Mkdir(dir, wxS_DIR_DEFAULT, wxPATH_MKDIR_FULL)) {
    wxMessageBox(U8("Impossible de créer le dossier ") + dir, "PersonalDepth", wxICON_WARNING, this);
    return;
  }
  const wxString path = dir + wxFileName::GetPathSeparator() + name + ".cpt";
  if (wxFileExists(path) &&
      wxMessageBox(U8("Remplacer la palette existante ?"), "PersonalDepth", wxYES_NO | wxICON_QUESTION,
                   this) != wxYES)
    return;
  if (!s.ramp.saveCPT(path.ToUTF8().data())) {
    wxMessageBox(U8("Échec de l'écriture"), "PersonalDepth", wxICON_WARNING, this);
    return;
  }
  RefreshPresetList();
  SelectPreset(std::string("file:") + path.ToUTF8().data());
}

void BathyDialog::OnPaletteDir(wxCommandEvent&) {
  DisplaySettings& s = m_pi->Settings();
  wxDirDialog dd(this, U8("Dossier de palettes"), W8(s.paletteDir.c_str()));
  if (dd.ShowModal() != wxID_OK) return;
  s.paletteDir = dd.GetPath().ToUTF8().data();
  m_pi->SaveConfig();
  m_pi->WriteBuiltinPalettes(false);  // nouveau dossier : palettes d'origine ajoutées une fois
  RefreshPresetList();
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
  m_pi->Settings().autoBaseFresh = false;
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
  r.safetyDepth = FromUi(m_safeDepth->GetValue(), r.safetyDepth, 1);
  r.safetyColor = FromWx(m_safeColour->GetColour(), A_(r.safetyColor));
  m_pi->ColorsChanged();
}

// ---------------------------------------------------------------- Isobathes
// Isobathes et sondes chiffrées (onglet Affichage) : un cadre compact
wxSizer* BathyDialog::BuildChartTextBox(wxWindow* p) {
  auto* box = new wxStaticBoxSizer(wxVERTICAL, p, U8("Isobathes et sondes"));
  wxWindow* sb = box->GetStaticBox();
  auto L = [&](const char* t) { return new wxStaticText(sb, wxID_ANY, U8(t)); };
  const wxSize spinW(FromDIP(64), -1);
  m_ctOn = new wxCheckBox(sb, wxID_ANY, U8("Isobathes"));
  m_ctOn->SetToolTip(U8("Afficher les isobathes. L'isobathe de sécurité (rouge) suit la zone dangereuse."));
  m_ctColour = new wxColourPickerCtrl(sb, wxID_ANY, wxColour(25, 45, 95));
  m_ctColour->SetToolTip(U8("Couleur des isobathes"));
  wxString modes[] = {U8("Intervalle régulier"), U8("Seuils de la palette")};
  m_ctMode = new wxChoice(sb, wxID_ANY, wxDefaultPosition, wxDefaultSize, 2, modes);
  m_ctMode->SetToolTip(U8("Niveaux des isobathes"));
  m_ctStep = new wxSpinCtrlDouble(sb, wxID_ANY, "", wxDefaultPosition, spinW,
                                  wxSP_ARROW_KEYS, 0.1, 100, 1, 0.5);
  m_ctStep->SetDigits(1);
  m_ctMajor = new wxSpinCtrlDouble(sb, wxID_ANY, "", wxDefaultPosition, spinW,
                                   wxSP_ARROW_KEYS, 0, 500, 5, 1);
  m_ctMajor->SetDigits(1);
  m_ctMajor->SetToolTip(U8("Ligne épaisse toutes les N unités de profondeur (0 = aucune)"));
  m_ctLabels = new wxCheckBox(sb, wxID_ANY, U8("Valeurs"));
  m_ctLabels->SetToolTip(U8("Valeurs sur les lignes"));
  m_ctSmooth = new wxCheckBox(sb, wxID_ANY, U8("Lissage des lignes"));
  m_ctSmooth->SetToolTip(U8("Arrondit les angles du tracé (purement graphique)"));
  m_ctStepL = L("Intervalle (m)");
  m_ctMajorL = L("Maîtresse (m)");
  auto* r1 = new wxBoxSizer(wxHORIZONTAL);
  r1->Add(m_ctOn, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 4);
  r1->Add(m_ctColour, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 6);
  r1->Add(m_ctMode, 1, wxALIGN_CENTER_VERTICAL | wxRIGHT, 6);
  r1->Add(m_ctLabels, 0, wxALIGN_CENTER_VERTICAL);
  box->Add(r1, 0, wxEXPAND | wxALL, 4);
  auto* r2 = new wxBoxSizer(wxHORIZONTAL);
  r2->Add(m_ctStepL, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 4);
  r2->Add(m_ctStep, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 8);
  r2->Add(m_ctMajorL, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 4);
  r2->Add(m_ctMajor, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 8);
  r2->Add(m_ctSmooth, 0, wxALIGN_CENTER_VERTICAL);
  box->Add(r2, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, 4);

  for (wxCheckBox* c : {m_ctOn, m_ctLabels, m_ctSmooth})
    c->Bind(wxEVT_CHECKBOX, &BathyDialog::OnContours, this);
  m_ctMode->Bind(wxEVT_CHOICE, &BathyDialog::OnContours, this);
  for (wxSpinCtrlDouble* c : {m_ctStep, m_ctMajor})
    c->Bind(wxEVT_SPINCTRLDOUBLE, [this](wxSpinDoubleEvent&) { wxCommandEvent e; OnContours(e); });
  m_ctColour->Bind(wxEVT_COLOURPICKER_CHANGED, [this](wxColourPickerEvent&) {
    wxCommandEvent e; OnContours(e);
  });

  // sondes chiffrées
  m_sound = new wxCheckBox(sb, wxID_ANY, U8("Sondes"));
  m_sound->SetToolTip(U8("Afficher les sondes chiffrées (la plus faible par case d'écran, jamais "
                         "prise dans les zones interpolées)."));
  m_spacing = new wxSlider(sb, wxID_ANY, 34, 16, 120);
  m_spacing->SetToolTip(U8("Écart minimal entre deux sondes affichées (pixels d'écran)."));
  m_decimals = new wxSpinCtrl(sb, wxID_ANY, "", wxDefaultPosition, spinW, wxSP_ARROW_KEYS, 0, 2, 1);
  m_font = new wxSpinCtrl(sb, wxID_ANY, "", wxDefaultPosition, spinW, wxSP_ARROW_KEYS, 6, 20, 9);
  m_font->SetToolTip(U8("Taille des sondes et des valeurs des isobathes."));
  auto* r3 = new wxBoxSizer(wxHORIZONTAL);
  r3->Add(m_sound, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 8);
  r3->Add(L("Espacement"), 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 4);
  r3->Add(m_spacing, 1, wxALIGN_CENTER_VERTICAL);
  box->Add(r3, 0, wxEXPAND | wxLEFT | wxRIGHT, 4);
  auto* r4 = new wxBoxSizer(wxHORIZONTAL);
  r4->Add(L("Décimales"), 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 4);
  r4->Add(m_decimals, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 8);
  r4->Add(L("Taille du texte"), 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 4);
  r4->Add(m_font, 0, wxALIGN_CENTER_VERTICAL);
  box->Add(r4, 0, wxALL, 4);
  m_spacing->Bind(wxEVT_SLIDER, &BathyDialog::OnDisplay, this);
  m_sound->Bind(wxEVT_CHECKBOX, &BathyDialog::OnDisplay, this);
  for (wxSpinCtrl* c : {m_decimals, m_font})
    c->Bind(wxEVT_SPINCTRL, [this](wxSpinEvent&) { wxCommandEvent e; OnDisplay(e); });
  return box;
}

void BathyDialog::OnContours(wxCommandEvent&) {
  if (m_updating) return;
  DisplaySettings& s = m_pi->Settings();
  s.contours = m_ctOn->GetValue();
  s.contourMode = m_ctMode->GetSelection();
  s.contourStep = FromUi(m_ctStep->GetValue(), s.contourStep, 1);
  s.contourMajor = FromUi(m_ctMajor->GetValue(), s.contourMajor, 1);
  s.contourLabels = m_ctLabels->GetValue();
  s.contourSmooth = m_ctSmooth->GetValue();
  s.contourColor = FromWx(m_ctColour->GetColour());
  m_ctStep->Enable(s.contourMode == 0);
  ++s.contourVersion;
  m_pi->ViewChanged();
}

// ---------------------------------------------------------------- Affichage
// Onglet Affichage, sans défilement à sa taille normale : en haut, couches et
// référence verticale (communes) ; à gauche palette, isobathes et sondes ; à
// droite relief, imagerie, bandeau et curseur
wxSizer* BathyDialog::BuildDisplayBox(wxWindow* p) {
  auto* col = new wxBoxSizer(wxVERTICAL);
  BuildImageryDisplay(p);
  auto* top = new wxBoxSizer(wxHORIZONTAL);
  top->Add(m_layersBox, 0, wxEXPAND | wxALL, 2);
  top->Add(BuildDispDatumBox(p), 1, wxEXPAND | wxALL, 2);
  col->Add(top, 0, wxEXPAND);
  auto* cols = new wxBoxSizer(wxHORIZONTAL);
  auto* left = new wxBoxSizer(wxVERTICAL);
  auto* right = new wxBoxSizer(wxVERTICAL);
  cols->Add(left, 1, wxEXPAND);
  cols->Add(right, 1, wxEXPAND);
  col->Add(cols, 1, wxEXPAND);
  left->Add(BuildPaletteBox(p), 1, wxEXPAND | wxALL, 2);
  left->Add(BuildChartTextBox(p), 0, wxEXPAND | wxALL, 2);

  auto* box = new wxStaticBoxSizer(wxVERTICAL, p, U8("Bathymétrie : relief et lissage"));
  wxWindow* sb = box->GetStaticBox();
  auto L = [&](const char* t) { return new wxStaticText(sb, wxID_ANY, U8(t)); };
  const wxSize spinW(FromDIP(64), -1);
  m_opacity = new wxSlider(sb, wxID_ANY, 85, 10, 100);
  m_opacity->SetToolTip(U8("Opacité de la surface colorée sur la carte"));
  m_shade = new wxCheckBox(sb, wxID_ANY, U8("Ombrage"));
  m_shade->SetToolTip(U8("Ombrage du relief et son intensité"));
  m_shadeStrength = new wxSlider(sb, wxID_ANY, 60, 0, 100);
  m_exag = new wxSpinCtrlDouble(sb, wxID_ANY, "", wxDefaultPosition, spinW,
                                wxSP_ARROW_KEYS, 0.5, 50, 3, 0.5);
  m_exag->SetToolTip(U8("Exagération verticale : accentue le relief de l'ombrage"));
  m_shadeSmooth = new wxSpinCtrl(sb, wxID_ANY, "", wxDefaultPosition, spinW, wxSP_ARROW_KEYS, 0, 8, 1);
  m_shadeSmooth->SetToolTip(U8("Adoucit le relief ombré (rayon en cellules, 0 = aucun). "
                               "Les couleurs, isobathes et sondes gardent les vraies profondeurs."));
  m_smooth = new wxCheckBox(sb, wxID_ANY, U8("Lissage"));
  m_smooth->SetToolTip(U8("Lissage (interpolation) : couleurs fondues entre les mailles"));
  m_interpShow = new wxCheckBox(sb, wxID_ANY, U8("Zones interpolées de la base"));
  m_interpShow->SetToolTip(U8("Surface calculée entre les sondes d'un nuage de points à l'import "
                              "(levés en lignes, sondes de carte…). Décoché : seules les cases "
                              "mesurées sont affichées."));
  m_interpDistinct = new wxCheckBox(sb, wxID_ANY, U8("plus claires"));
  m_interpDistinct->SetToolTip(U8("Couleur des zones interpolées éclaircie pour les distinguer des "
                                  "zones mesurées. Les sondes affichées ne sont jamais interpolées."));
  auto* g = new wxFlexGridSizer(2, 4, 8);
  g->AddGrowableCol(1);
  g->Add(L("Opacité"), 0, wxALIGN_CENTER_VERTICAL); g->Add(m_opacity, 0, wxEXPAND);
  g->Add(m_shade, 0, wxALIGN_CENTER_VERTICAL); g->Add(m_shadeStrength, 0, wxEXPAND);
  box->Add(g, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, 4);
  auto* r = new wxBoxSizer(wxHORIZONTAL);
  r->Add(L("Exagération"), 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 4);
  r->Add(m_exag, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 8);
  r->Add(L("Adoucir"), 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 4);
  r->Add(m_shadeSmooth, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 8);
  r->Add(m_smooth, 0, wxALIGN_CENTER_VERTICAL);
  box->Add(r, 0, wxLEFT | wxRIGHT | wxTOP, 4);
  auto* ri = new wxBoxSizer(wxHORIZONTAL);
  ri->Add(m_interpShow, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 8);
  ri->Add(m_interpDistinct, 0, wxALIGN_CENTER_VERTICAL);
  box->Add(ri, 0, wxALL, 4);
  right->Add(box, 0, wxEXPAND | wxALL, 2);
  right->Add(m_imgRenderBox, 0, wxEXPAND | wxALL, 2);

  auto* com = new wxStaticBoxSizer(wxVERTICAL, p, U8("Bandeau et curseur"));
  wxWindow* cb = com->GetStaticBox();
  m_legend = new wxCheckBox(cb, wxID_ANY, U8("Lecture au curseur"));
  m_legend->SetToolTip(U8("Profondeur et source à côté du curseur"));
  m_banner = new wxCheckBox(cb, wxID_ANY, U8("Bandeau"));
  m_banner->SetToolTip(U8("Bandeau de palette sur la carte. "
                          "A / M : palette automatique ou manuelle ; Vue / Tout : seuils d'après la zone affichée "
                          "ou toutes les données ; clic sur une profondeur : "
                          "borne de la palette ; clic sur les couleurs : préréglages ; "
                          "petite rampe : bandeau horizontal ou vertical ; roue : ce panneau. "
                          "Imagerie affichée : seconde rampe (gris), bornes en % de l'étirement, A / M "
                          "et Vue / Tout du contraste, clic sur la rampe : palette et inversion."));
  m_bannerPos = new wxChoice(cb, wxID_ANY);
  for (const char* t : {"en bas, centré", "en haut, centré", "en bas à gauche", "libre (déplacé à la souris)"})
    m_bannerPos->Append(U8(t));
  m_bannerPos->SetToolTip(U8("Le bandeau se déplace aussi en le faisant glisser sur la carte."));
  m_bannerLock = new wxCheckBox(cb, wxID_ANY, U8("Verrouiller"));
  m_bannerLock->SetToolTip(U8("Verrouiller la position du bandeau"));
  auto* b1 = new wxBoxSizer(wxHORIZONTAL);
  b1->Add(m_banner, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 6);
  b1->Add(m_bannerPos, 1, wxALIGN_CENTER_VERTICAL);
  com->Add(b1, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, 4);
  auto* b2 = new wxBoxSizer(wxHORIZONTAL);
  b2->Add(m_bannerLock, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 12);
  b2->Add(m_legend, 0, wxALIGN_CENTER_VERTICAL);
  com->Add(b2, 0, wxLEFT | wxRIGHT | wxTOP, 4);
  // icône de la barre d'outils d'OpenCPN
  m_toolbarMode = new wxChoice(cb, wxID_ANY);
  m_toolbarMode->Append(U8("ouvre ce panneau"));
  m_toolbarMode->Append(U8("affiche / masque PersonalDepth"));
  m_toolbarMode->SetToolTip(U8("Icône de PersonalDepth dans la barre d'outils d'OpenCPN. « Affiche / masque » : "
                               "un clic montre ou cache la bathymétrie, l'imagerie et le bandeau ; ce panneau "
                               "s'ouvre alors par le clic droit sur la carte (PersonalDepth…), la roue du "
                               "bandeau ou Options › Plugins › Préférences."));
  auto* b3 = new wxBoxSizer(wxHORIZONTAL);
  b3->Add(new wxStaticText(cb, wxID_ANY, U8("Icône")), 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 6);
  b3->Add(m_toolbarMode, 1, wxALIGN_CENTER_VERTICAL);
  com->Add(b3, 0, wxEXPAND | wxALL, 4);
  m_toolbarMode->Bind(wxEVT_CHOICE, [this](wxCommandEvent&) {
    if (m_updating) return;
    m_pi->Settings().toolbarMode = m_toolbarMode->GetSelection() == 1 ? 1 : 0;
    m_pi->ToolbarModeChanged();
  });
  right->Add(com, 0, wxEXPAND | wxALL, 2);

  for (wxSlider* s : {m_opacity, m_shadeStrength})
    s->Bind(wxEVT_SLIDER, &BathyDialog::OnDisplay, this);
  for (wxCheckBox* c : {m_shade, m_smooth, m_legend, m_banner, m_bannerLock, m_interpShow, m_interpDistinct})
    c->Bind(wxEVT_CHECKBOX, &BathyDialog::OnDisplay, this);
  m_bannerPos->Bind(wxEVT_CHOICE, &BathyDialog::OnDisplay, this);
  m_shadeSmooth->Bind(wxEVT_SPINCTRL, [this](wxSpinEvent&) {
    if (m_updating) return;
    m_pi->Settings().shadeSmooth = m_shadeSmooth->GetValue();
    m_pi->ExaggerationChanged();  // recalcul de l'ombrage
  });
  m_exag->Bind(wxEVT_SPINCTRLDOUBLE, [this](wxSpinDoubleEvent&) {
    wxCommandEvent e; OnExaggeration(e);
  });
  return col;
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
  s.banner = m_banner->GetValue();
  s.bannerPos = m_bannerPos->GetSelection();
  s.bannerLock = m_bannerLock->GetValue();
  m_bannerPos->Enable(s.banner);
  m_bannerLock->Enable(s.banner);
  const bool interpChanged = s.interpShow != m_interpShow->GetValue();
  colors = colors || s.interpDistinct != m_interpDistinct->GetValue();
  s.interpShow = m_interpShow->GetValue();
  s.interpDistinct = m_interpDistinct->GetValue();
  m_interpDistinct->Enable(s.interpShow);
  if (interpChanged) m_pi->InterpDisplayChanged();
  else if (colors) m_pi->ColorsChanged();
  else m_pi->ViewChanged();
}

// ---------------------------------------------------------------- Imagerie
// Réflectivité / rétrodiffusion / mosaïques de sonar : base .pdi du dossier de
// la base, rendu en niveaux de gris réglable sans relire les fichiers
// Réglages d'affichage de l'imagerie (onglet Affichage) : couches affichées,
// ordre, rendu ; m_layersBox et m_imgRenderBox sont placés par BuildDisplayBox
void BathyDialog::BuildImageryDisplay(wxWindow* p) {
  DisplaySettings& s = m_pi->Settings();
  // Couches affichées et ordre
  auto* lay = new wxStaticBoxSizer(wxVERTICAL, p, U8("Couches affichées"));
  wxWindow* lb = lay->GetStaticBox();
  auto* lr = new wxBoxSizer(wxHORIZONTAL);
  m_bathyShow = new wxCheckBox(lb, wxID_ANY, U8("Bathymétrie"));
  m_imgShow = new wxCheckBox(lb, wxID_ANY, U8("Imagerie"));
  m_bathyShow->SetToolTip(U8("Couleurs de profondeur et ombrage (base et fichiers). Isobathes et sondes, si elles "
                              "sont cochées, restent affichées au-dessus de toutes les couches, même "
                              "couleurs masquées."));
  m_imgShow->SetToolTip(U8("Réflectivité, rétrodiffusion, mosaïques de sonar (fichiers .pdi de la base)."));
  lr->Add(m_bathyShow, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 12);
  lr->Add(m_imgShow, 0, wxALIGN_CENTER_VERTICAL);
  lay->Add(lr, 0, wxLEFT | wxRIGHT | wxTOP, 4);
  m_imgOrder = new wxChoice(lb, wxID_ANY);
  m_imgOrder->Append(U8("Imagerie sous la bathymétrie"));
  m_imgOrder->Append(U8("Imagerie au-dessus de la bathymétrie"));
  m_imgOrder->SetToolTip(U8("Couche du dessus : son opacité règle la transparence de la superposition. "
                            "Isobathes et sondes restent toujours au-dessus."));
  lay->Add(m_imgOrder, 0, wxEXPAND | wxALL, 4);
  m_layersBox = lay;

  // Rendu
  auto* ren = new wxStaticBoxSizer(wxVERTICAL, p, U8("Imagerie"));
  wxWindow* rb = ren->GetStaticBox();
  auto LR = [&](const char* t) { return new wxStaticText(rb, wxID_ANY, U8(t)); };
  m_imgPalette = new wxChoice(rb, wxID_ANY);
  for (const char* t : {"Niveaux de gris", "Cuivre", "Sépia"}) m_imgPalette->Append(U8(t));
  m_imgInvert = new wxCheckBox(rb, wxID_ANY, U8("Inverser"));
  m_imgInvert->SetToolTip(U8("Réflectivité forte en sombre"));
  m_imgOpacity = new wxSlider(rb, wxID_ANY, 100, 0, 100);
  m_imgMin = new wxSlider(rb, wxID_ANY, 0, 0, 99);
  m_imgMax = new wxSlider(rb, wxID_ANY, 100, 1, 100);
  m_imgMinV = new wxStaticText(rb, wxID_ANY, "100", wxDefaultPosition, wxDefaultSize, wxALIGN_RIGHT);
  m_imgMaxV = new wxStaticText(rb, wxID_ANY, "100", wxDefaultPosition, wxDefaultSize, wxALIGN_RIGHT);
  for (wxStaticText* t : {m_imgMinV, m_imgMaxV}) t->SetMinSize(t->GetBestSize());
  const wxString stretchTip = U8("En % de l'étirement de chaque image (0 % et 100 % : valeurs à 2 % et 98 % "
                                 "de l'image, mesurées à l'import). Les valeurs d'origine (dB, niveaux) "
                                 "sont conservées et lues au curseur.");
  m_imgMin->SetToolTip(stretchTip);
  m_imgMax->SetToolTip(stretchTip);
  m_imgGamma = new wxSpinCtrlDouble(rb, wxID_ANY, "", wxDefaultPosition, wxSize(FromDIP(64), -1),
                                    wxSP_ARROW_KEYS, 0.2, 5, 1, 0.1);
  m_imgGamma->SetDigits(1);
  m_imgGamma->SetToolTip(U8("Plus de 1 : tons moyens éclaircis ; moins de 1 : assombris."));
  m_imgSmooth = new wxCheckBox(rb, wxID_ANY, U8("Lissage"));
  m_imgSmooth->SetToolTip(U8("Interpolation entre pixels"));
  // contraste automatique (comme la palette automatique de la bathymétrie)
  m_imgAuto = new wxCheckBox(rb, wxID_ANY, U8("Contraste auto"));
  m_imgAuto->SetToolTip(U8("Contraste automatique : noir et blanc placés aux valeurs à 2 % et 98 % de la "
                           "zone affichée, ou étirement de chaque image mesuré à l'import (toutes les "
                           "images). Saisir une borne repasse en manuel. Aussi par les boutons A / M et "
                           "Vue / Tout du bandeau."));
  m_imgAutoSrc = new wxChoice(rb, wxID_ANY);
  m_imgAutoSrc->Append(U8("toutes les images"));
  m_imgAutoSrc->Append(U8("zone affichée"));
  auto* r1 = new wxBoxSizer(wxHORIZONTAL);
  r1->Add(LR("Palette"), 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 4);
  r1->Add(m_imgPalette, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 8);
  r1->Add(m_imgInvert, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 8);
  r1->Add(m_imgSmooth, 0, wxALIGN_CENTER_VERTICAL);
  ren->Add(r1, 0, wxLEFT | wxRIGHT | wxTOP, 4);
  auto* g = new wxFlexGridSizer(3, 4, 6);
  g->AddGrowableCol(1);
  g->Add(LR("Opacité"), 0, wxALIGN_CENTER_VERTICAL); g->Add(m_imgOpacity, 0, wxEXPAND); g->AddSpacer(0);
  g->Add(LR("Noir à (%)"), 0, wxALIGN_CENTER_VERTICAL); g->Add(m_imgMin, 0, wxEXPAND);
  g->Add(m_imgMinV, 0, wxALIGN_CENTER_VERTICAL);
  g->Add(LR("Blanc à (%)"), 0, wxALIGN_CENTER_VERTICAL); g->Add(m_imgMax, 0, wxEXPAND);
  g->Add(m_imgMaxV, 0, wxALIGN_CENTER_VERTICAL);
  ren->Add(g, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, 4);
  auto* r2 = new wxBoxSizer(wxHORIZONTAL);
  r2->Add(LR("Gamma"), 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 4);
  r2->Add(m_imgGamma, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 8);
  r2->Add(m_imgAuto, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 4);
  r2->Add(m_imgAutoSrc, 1, wxALIGN_CENTER_VERTICAL);
  ren->Add(r2, 0, wxEXPAND | wxALL, 4);
  m_imgRenderBox = ren;

  m_bathyShow->SetValue(s.bathyShow);
  m_imgShow->SetValue(s.imgShow);
  m_imgOrder->SetSelection(s.imgOnTop ? 1 : 0);
  m_imgPalette->SetSelection(s.imgPalette);
  m_imgInvert->SetValue(s.imgInvert);
  m_imgOpacity->SetValue(s.imgOpacity);
  m_imgMin->SetValue(s.imgMin);
  m_imgMax->SetValue(s.imgMax);
  m_imgGamma->SetValue(s.imgGamma);
  m_imgSmooth->SetValue(s.imgSmooth);
  RefreshImageryDisplay();
  auto shown = [this](wxCommandEvent&) {
    DisplaySettings& d = m_pi->Settings();
    d.bathyShow = m_bathyShow->GetValue();
    d.imgShow = m_imgShow->GetValue();
    d.imgOnTop = m_imgOrder->GetSelection() == 1;
    m_pi->LayersShownChanged();
  };
  m_bathyShow->Bind(wxEVT_CHECKBOX, shown);
  m_imgShow->Bind(wxEVT_CHECKBOX, shown);
  m_imgOrder->Bind(wxEVT_CHOICE, shown);
  for (wxSlider* sl : {m_imgOpacity, m_imgMin, m_imgMax})
    sl->Bind(wxEVT_SLIDER, [this](wxCommandEvent&) { OnImgDisplay(); });
  for (wxCheckBox* c : {m_imgInvert, m_imgSmooth})
    c->Bind(wxEVT_CHECKBOX, [this](wxCommandEvent&) { OnImgDisplay(); });
  m_imgPalette->Bind(wxEVT_CHOICE, [this](wxCommandEvent&) { OnImgDisplay(); });
  m_imgGamma->Bind(wxEVT_SPINCTRLDOUBLE, [this](wxSpinDoubleEvent&) { OnImgDisplay(); });
  m_imgAuto->Bind(wxEVT_CHECKBOX, [this](wxCommandEvent&) {
    DisplaySettings& d = m_pi->Settings();
    d.imgAuto = m_imgAuto->GetValue();
    m_pi->UpdateImgAuto(true);
    m_pi->ImageryChanged();
  });
  m_imgAutoSrc->Bind(wxEVT_CHOICE, [this](wxCommandEvent&) {
    DisplaySettings& d = m_pi->Settings();
    d.imgAutoSource = m_imgAutoSrc->GetSelection();
    m_pi->UpdateImgAuto(true);
    m_pi->ImageryChanged();
  });
}

// Valeurs affichées du rendu de l'imagerie (bandeau, contraste automatique)
void BathyDialog::RefreshImageryDisplay() {
  if (!m_imgPalette) return;
  const DisplaySettings& s = m_pi->Settings();
  m_updating = true;
  m_bathyShow->SetValue(s.bathyShow);
  m_imgShow->SetValue(s.imgShow);
  m_imgOrder->SetSelection(s.imgOnTop ? 1 : 0);
  m_imgPalette->SetSelection(s.imgPalette);
  m_imgInvert->SetValue(s.imgInvert);
  m_imgOpacity->SetValue(s.imgOpacity);
  m_imgMin->SetValue(s.imgMin);
  m_imgMax->SetValue(s.imgMax);
  m_imgMinV->SetLabel(wxString::Format("%d", (int)s.imgMin));
  m_imgMaxV->SetLabel(wxString::Format("%d", (int)s.imgMax));
  m_imgGamma->SetValue(s.imgGamma);
  m_imgSmooth->SetValue(s.imgSmooth);
  m_imgAuto->SetValue(s.imgAuto);
  m_imgAutoSrc->SetSelection(s.imgAutoSource == 1 ? 1 : 0);
  m_imgAutoSrc->Enable(s.imgAuto);
  m_updating = false;
  UpdateSonarOffsetFields();
}

// Décalage sonde / antenne : estimé -> champs grisés, valeurs du dernier
// journal importé ; saisi -> champs modifiables
void BathyDialog::UpdateSonarOffsetFields() {
  if (!m_imgOffAuto) return;
  const DisplaySettings& s = m_pi->Settings();
  const bool est = m_imgOffAuto->GetValue();
  m_imgOffFwd->Enable(!est);
  m_imgOffStbd->Enable(!est);
  if (est) {
    m_imgOffFwd->SetValue(s.imgSonarOffsetEstOk ? s.imgSonarOffsetEstFwd : 0.0);
    m_imgOffStbd->SetValue(s.imgSonarOffsetEstOk ? s.imgSonarOffsetEstStbd : 0.0);
  } else {
    m_imgOffFwd->SetValue(s.imgSonarOffsetFwd);
    m_imgOffStbd->SetValue(s.imgSonarOffsetStbd);
  }
  m_imgOffAuto->SetToolTip(
      U8("Estimé sur les lignes parcourues en sens opposés : une même tache du fond vue des deux sens est "
         "décalée de deux fois l'écart entre la sonde et l'antenne GPS. Les champs avant / tribord montrent "
         "alors le décalage obtenu sur le dernier journal importé ; sans allers-retours ni fond "
         "reconnaissable, ce sont les valeurs saisies (case décochée) qui s'appliquent.") +
      "\n" +
      (s.imgSonarOffsetEstOk
           ? wxString::Format(U8("Dernière estimation : %+.1f m vers l'avant, %+.1f m vers tribord."),
                              s.imgSonarOffsetEstFwd, s.imgSonarOffsetEstStbd)
           : U8("Pas encore d'estimation réussie.")));
}

wxSizer* BathyDialog::BuildImageryBox(wxWindow* p) {
  DisplaySettings& s = m_pi->Settings();
  auto* box = new wxBoxSizer(wxVERTICAL);

  // Images de la base (l'affichage se règle dans l'onglet Affichage)
  auto* src = new wxStaticBoxSizer(wxVERTICAL, p, U8("Imagerie"));
  wxWindow* ib = src->GetStaticBox();
  m_imgList = new wxListCtrl(ib, wxID_ANY, wxDefaultPosition, wxSize(-1, FromDIP(110)),
                             wxLC_REPORT);
  m_imgList->AppendColumn(U8("Image"), wxLIST_FORMAT_LEFT, FromDIP(150));
  m_imgList->AppendColumn(U8("Affichée"), wxLIST_FORMAT_LEFT, FromDIP(55));
  m_imgList->AppendColumn(U8("Valeurs"), wxLIST_FORMAT_LEFT, FromDIP(110));
  m_imgList->AppendColumn(U8("Pixel"), wxLIST_FORMAT_RIGHT, FromDIP(55));
  m_imgList->AppendColumn(U8("Date"), wxLIST_FORMAT_LEFT, FromDIP(75));
  src->Add(m_imgList, 1, wxEXPAND | wxALL, 4);
  auto* br = new wxBoxSizer(wxHORIZONTAL);
  m_imgAdd = new wxButton(ib, wxID_ANY, U8("Importer…"));
  m_imgAdd->SetToolTip(U8("GeoTIFF de réflectivité ou de rétrodiffusion (dB, niveaux de gris 8/16 bits, "
                          "palette) : mosaïques Ifremer, Infomar, EMODnet… Système de coordonnées lu "
                          "dans le fichier.\nJournaux Lowrance / Simrad / B&G .sl2 / .sl3 : sonar latéral "
                          "(StructureScan, SideImaging) assemblé en mosaïque."));
  m_imgVis = new wxButton(ib, wxID_ANY, U8("Afficher / masquer"));
  m_imgZoom = new wxButton(ib, wxID_ANY, U8("Zoom"));
  m_imgDel = new wxButton(ib, wxID_ANY, U8("Retirer"));
  br->Add(m_imgAdd, 0, wxRIGHT, 8);
  br->AddStretchSpacer();
  for (wxButton* b : {m_imgVis, m_imgZoom}) br->Add(b, 0, wxRIGHT, 2);
  br->Add(m_imgDel, 0);
  src->Add(br, 0, wxEXPAND | wxALL, 4);
  // réglages lus à l'import seulement : réimporter pour les appliquer à une
  // image déjà dans la base (l'affichage se règle dans l'onglet Affichage)
  auto* rr = new wxBoxSizer(wxHORIZONTAL);
  auto* rl = new wxStaticText(ib, wxID_ANY, U8("Réglages d'import"));
  rl->SetFont(rl->GetFont().Bold());
  rl->SetToolTip(U8("Fond, pixel, égalisation et décalage du sonar : appliqués à l'import. Pour une image déjà "
                    "importée, la sélectionner et cliquer sur Réimporter. Palette, contraste et gamma se "
                    "règlent dans l'onglet Affichage et s'appliquent tout de suite."));
  m_imgReimp = new wxButton(ib, wxID_ANY, U8("Réimporter la sélection"));
  m_imgReimp->SetToolTip(U8("Relit les fichiers d'origine des images sélectionnées avec les réglages d'import "
                            "ci-dessous ; la nouvelle image remplace l'ancienne (même date, même classe)."));
  rr->Add(rl, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 8);
  rr->AddStretchSpacer();
  rr->Add(m_imgReimp, 0, wxALIGN_CENTER_VERTICAL);
  src->Add(rr, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, 4);
  auto* bg = new wxBoxSizer(wxHORIZONTAL);
  bg->Add(new wxStaticText(ib, wxID_ANY, U8("Fond hors levé")), 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 6);
  m_imgBg = new wxChoice(ib, wxID_ANY);
  for (const char* t : {"détection automatique", "aucun", "valeur :"}) m_imgBg->Append(U8(t));
  m_imgBg->SetToolTip(U8("Mosaïques sur fond uniforme (blanc 255 ou noir 0 autour du levé) : ces pixels "
                         "sont écartés. Automatique : valeur extrême présente sur une grande partie "
                         "du bord de l'image."));
  m_imgBgValue = new wxSpinCtrlDouble(ib, wxID_ANY, "", wxDefaultPosition, wxSize(FromDIP(90), -1),
                                      wxSP_ARROW_KEYS, -1e6, 1e6, 255, 1);
  bg->Add(m_imgBg, 0, wxRIGHT, 4);
  bg->Add(m_imgBgValue, 0);
  src->Add(bg, 0, wxLEFT | wxRIGHT, 4);
  // journaux de sondeur : mosaïque du sonar latéral
  auto* ss = new wxBoxSizer(wxHORIZONTAL);
  ss->Add(new wxStaticText(ib, wxID_ANY, U8("Sonar : pixel (m)")), 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 6);
  m_imgSonarPix = new wxSpinCtrlDouble(ib, wxID_ANY, "", wxDefaultPosition, wxSize(FromDIP(80), -1),
                                       wxSP_ARROW_KEYS, 0, 5, 0, 0.05);
  m_imgSonarPix->SetDigits(2);
  m_imgSonarPix->SetToolTip(U8("Pas de la mosaïque des journaux .sl2 / .sl3. 0 = automatique (portée / 400, "
                               "de 0,1 à 1 m). Élargi tout seul si le levé est très étendu (mémoire)."));
  m_imgSonarEq = new wxCheckBox(ib, wxID_ANY, U8("Égaliser"));
  m_imgSonarEq->SetToolTip(U8("Même luminosité moyenne du centre au bord de la fauchée, malgré les changements "
                              "de gain ou de portée ; le bruit au-delà de la portée utile est coupé."));
  ss->Add(m_imgSonarPix, 0, wxRIGHT, 8);
  ss->Add(m_imgSonarEq, 0, wxALIGN_CENTER_VERTICAL);
  src->Add(ss, 0, wxLEFT | wxRIGHT | wxTOP, 4);
  // position de la sonde par rapport à l'antenne GPS
  auto* so = new wxBoxSizer(wxHORIZONTAL);
  m_imgOffAuto = new wxCheckBox(ib, wxID_ANY, U8("Sonde / GPS estimé"));
  m_imgOffFwd = new wxSpinCtrlDouble(ib, wxID_ANY, "", wxDefaultPosition, wxSize(FromDIP(70), -1),
                                     wxSP_ARROW_KEYS, -30, 30, 0, 0.1);
  m_imgOffFwd->SetDigits(1);
  m_imgOffFwd->SetToolTip(U8("Sonde par rapport à l'antenne GPS, vers l'avant (m). Négatif : sonde en "
                             "arrière, cas habituel d'une sonde au tableau arrière et d'un GPS intégré à "
                             "l'écran."));
  m_imgOffStbd = new wxSpinCtrlDouble(ib, wxID_ANY, "", wxDefaultPosition, wxSize(FromDIP(70), -1),
                                      wxSP_ARROW_KEYS, -10, 10, 0, 0.1);
  m_imgOffStbd->SetDigits(1);
  m_imgOffStbd->SetToolTip(U8("Sonde par rapport à l'antenne GPS, vers tribord (m). Négatif : vers bâbord."));
  so->Add(m_imgOffAuto, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 8);
  so->Add(new wxStaticText(ib, wxID_ANY, U8("avant")), 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 4);
  so->Add(m_imgOffFwd, 0, wxRIGHT, 6);
  so->Add(new wxStaticText(ib, wxID_ANY, U8("tribord")), 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 4);
  so->Add(m_imgOffStbd, 0);
  src->Add(so, 0, wxALL, 4);
  box->Add(src, 1, wxEXPAND | wxALL, 2);

  // valeurs
  m_imgBg->SetSelection(s.imgBgMode);
  m_imgBgValue->SetValue(s.imgBgValue);
  m_imgBgValue->Enable(s.imgBgMode == 2);
  m_imgSonarPix->SetValue(s.imgSonarPixel);
  m_imgSonarEq->SetValue(s.imgSonarEqualize);
  m_imgOffAuto->SetValue(s.imgSonarOffsetAuto);
  UpdateSonarOffsetFields();

  // événements
  auto bgChanged = [this](wxCommandEvent&) {
    DisplaySettings& d = m_pi->Settings();
    d.imgBgMode = m_imgBg->GetSelection();
    d.imgBgValue = m_imgBgValue->GetValue();
    m_imgBgValue->Enable(d.imgBgMode == 2);
    m_pi->SaveConfig();
  };
  m_imgBg->Bind(wxEVT_CHOICE, bgChanged);
  auto sonarChanged = [this]() {
    DisplaySettings& d = m_pi->Settings();
    d.imgSonarPixel = m_imgSonarPix->GetValue();
    d.imgSonarEqualize = m_imgSonarEq->GetValue();
    d.imgSonarOffsetAuto = m_imgOffAuto->GetValue();
    if (!d.imgSonarOffsetAuto) {  // champs affichant les valeurs saisies
      d.imgSonarOffsetFwd = m_imgOffFwd->GetValue();
      d.imgSonarOffsetStbd = m_imgOffStbd->GetValue();
    }
    m_pi->SaveConfig();
  };
  m_imgSonarPix->Bind(wxEVT_SPINCTRLDOUBLE, [sonarChanged](wxSpinDoubleEvent&) { sonarChanged(); });
  m_imgSonarEq->Bind(wxEVT_CHECKBOX, [sonarChanged](wxCommandEvent&) { sonarChanged(); });
  m_imgOffAuto->Bind(wxEVT_CHECKBOX, [this, sonarChanged](wxCommandEvent&) {
    m_pi->Settings().imgSonarOffsetAuto = m_imgOffAuto->GetValue();
    UpdateSonarOffsetFields();  // avant la lecture des champs
    sonarChanged();
  });
  for (wxSpinCtrlDouble* sp : {m_imgOffFwd, m_imgOffStbd})
    sp->Bind(wxEVT_SPINCTRLDOUBLE, [sonarChanged](wxSpinDoubleEvent&) { sonarChanged(); });
  m_imgBgValue->Bind(wxEVT_SPINCTRLDOUBLE, [bgChanged](wxSpinDoubleEvent&) {
    wxCommandEvent e;
    bgChanged(e);
  });
  m_imgList->Bind(wxEVT_LIST_ITEM_SELECTED, [this](wxListEvent&) { UpdateImgButtons(); });
  m_imgList->Bind(wxEVT_LIST_ITEM_DESELECTED, [this](wxListEvent&) { UpdateImgButtons(); });
  m_imgList->Bind(wxEVT_LIST_ITEM_ACTIVATED, [this](wxListEvent&) { m_pi->ImgZoomTo(SelectedImages()); });
  m_imgAdd->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) {
    if (m_pi->Settings().dbFolder.empty()) {
      wxMessageBox(U8("Choisissez d'abord le dossier de la base (onglet Données)."), "PersonalDepth",
                   wxOK | wxICON_INFORMATION, this);
      return;
    }
    wxFileDialog dlg(this, U8("Imagerie du fond (GeoTIFF, journaux de sondeur)"), "", "",
                     U8("Imagerie (*.tif;*.tiff;*.sl2;*.sl3)|*.tif;*.tiff;*.TIF;*.TIFF;*.sl2;*.sl3;*.SL2;*.SL3|"
                        "GeoTIFF (*.tif;*.tiff)|*.tif;*.tiff;*.TIF;*.TIFF|"
                        "Journaux de sondeur (*.sl2;*.sl3)|*.sl2;*.sl3;*.SL2;*.SL3|Tous les fichiers (*.*)|*.*"),
                     wxFD_OPEN | wxFD_FILE_MUST_EXIST | wxFD_MULTIPLE);
    if (dlg.ShowModal() != wxID_OK) return;
    wxArrayString paths;
    dlg.GetPaths(paths);
    std::vector<std::string> files;
    for (const auto& f : paths) files.push_back(f.ToUTF8().data());
    m_pi->ImgImport(files, std::string());
    UpdateImgButtons();
  });
  m_imgReimp->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) {
    const auto ids = SelectedImages();
    if (ids.empty()) return;
    std::string missing;
    const bool started = m_pi->ImgReimport(ids, missing);
    if (!missing.empty())
      wxMessageBox(U8("Fichier d'origine introuvable, image gardée telle quelle :\n") + W8(missing.c_str()),
                   "PersonalDepth", wxOK | wxICON_WARNING, this);
    (void)started;
    UpdateImgButtons();
  });
  m_imgDel->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) {
    const auto ids = SelectedImages();
    if (ids.empty()) return;
    if (wxMessageBox(wxString::Format(U8("Retirer %d image(s) de la base ? Les fichiers d'origine ne sont "
                                         "pas modifiés."), (int)ids.size()),
                     "PersonalDepth", wxYES_NO | wxICON_QUESTION, this) != wxYES)
      return;
    m_pi->ImgRemove(ids);
    UpdateImgButtons();
  });
  m_imgVis->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) {
    const auto ids = SelectedImages();
    if (ids.empty()) return;
    bool anyHidden = false;
    for (const auto& d : m_pi->ImgSources())
      if (std::find(ids.begin(), ids.end(), d.id) != ids.end() && !d.visible) anyHidden = true;
    m_pi->ImgSetVisible(ids, anyHidden);
    UpdateImgButtons();
  });
  m_imgZoom->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { m_pi->ImgZoomTo(SelectedImages()); });
  return box;
}

void BathyDialog::OnImgDisplay() {
  if (m_updating) return;
  DisplaySettings& d = m_pi->Settings();
  d.imgPalette = m_imgPalette->GetSelection();
  d.imgInvert = m_imgInvert->GetValue();
  d.imgOpacity = m_imgOpacity->GetValue();
  if (d.imgMin != m_imgMin->GetValue() || d.imgMax != m_imgMax->GetValue()) d.imgAuto = false;  // borne réglée : manuel
  d.imgMin = m_imgMin->GetValue();
  d.imgMax = m_imgMax->GetValue();
  if (d.imgMax <= d.imgMin) {  // bornes croisées : l'autre curseur suit
    if (wxWindow::FindFocus() == m_imgMin) { d.imgMax = d.imgMin + 1; m_imgMax->SetValue(d.imgMax); }
    else { d.imgMin = d.imgMax - 1; m_imgMin->SetValue(d.imgMin); }
  }
  d.imgGamma = (float)m_imgGamma->GetValue();
  d.imgSmooth = m_imgSmooth->GetValue();
  m_pi->ImageryChanged();
}

std::vector<int64_t> BathyDialog::SelectedImages() const {
  std::vector<int64_t> ids;
  if (!m_imgList) return ids;
  const auto& src = m_pi->ImgSources();
  long i = -1;
  while ((i = m_imgList->GetNextItem(i, wxLIST_NEXT_ALL, wxLIST_STATE_SELECTED)) != -1)
    if (i >= 0 && (size_t)i < src.size()) ids.push_back(src[(size_t)i].id);
  return ids;
}

void BathyDialog::UpdateImgButtons() {
  if (!m_imgList) return;
  const bool busy = m_pi->DbBusy(), sel = !SelectedImages().empty();
  m_imgAdd->Enable(!busy);
  m_imgDel->Enable(!busy && sel);
  m_imgReimp->Enable(!busy && sel);
  m_imgVis->Enable(!busy && sel);
  m_imgZoom->Enable(sel);
  if (m_imgStop) m_imgStop->Enable(busy);
}

void BathyDialog::RefreshImagery() {
  if (!m_imgList) return;
  std::set<int64_t> sel;
  for (int64_t id : SelectedImages()) sel.insert(id);
  m_imgList->DeleteAllItems();
  long row = 0;
  for (const auto& d : m_pi->ImgSources()) {
    const long it = m_imgList->InsertItem(row, W8(d.name.c_str()));
    m_imgList->SetItem(it, 1, d.visible ? U8("oui") : U8("non"));
    wxString v = wxString::Format(W8("%.4g – %.4g"), d.vlo, d.vhi);
    if (!d.vunit.empty()) v += " " + U8(d.vunit.c_str());
    m_imgList->SetItem(it, 2, v);
    m_imgList->SetItem(it, 3, d.res > 0 ? wxString::Format(d.res < 10 ? "%.1f m" : "%.0f m", d.res) : wxString());
    m_imgList->SetItem(it, 4, W8(d.date.c_str()));
    if (sel.count(d.id)) m_imgList->SetItemState(it, wxLIST_STATE_SELECTED, wxLIST_STATE_SELECTED);
    ++row;
  }
  UpdateImgButtons();
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

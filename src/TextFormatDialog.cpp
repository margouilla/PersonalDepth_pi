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

#include "TextFormatDialog.h"

#include <wx/filedlg.h>
#include <wx/grid.h>
#include <wx/listctrl.h>

#include <algorithm>
#include <ctime>
#include <fstream>

#include "core/FileUtil.h"
#include "core/Lang.h"

#define U8(s) wxString::FromUTF8(pd::tr(s))
#define W8(s) wxString::FromUTF8(s)  // données : pas de traduction

using namespace pd;

namespace {
const char kSeps[] = {0, ' ', ',', ';', '\t'};
const wxColour kColX(173, 216, 255), kColY(182, 234, 182), kColZ(255, 214, 153),
    kColDate(222, 204, 255), kColTime(255, 204, 224), kSkipBg(240, 240, 240),
    kSkipFg(150, 150, 150), kBadBg(235, 80, 80);
const size_t kPreviewLines = 80;
const int kMaxCols = 40;

bool IsComment(const std::string& l) {
  size_t i = l.find_first_not_of(" \t");
  return i == std::string::npos || l[i] == '#' || l[i] == '%';
}
}  // namespace

wxString TextFormatDialog::Describe(const TextFormat& f) {
  if (!f.custom) return wxString();
  wxString s = wxString::Format(U8("X : col. %d, Y : col. %d, Z : col. %d"), f.colX, f.colY, f.colZ);
  if (f.colDate) s += wxString::Format(U8(", date : col. %d"), f.colDate);
  if (f.colTime) s += wxString::Format(U8(", heure : col. %d"), f.colTime);
  if (!f.xFmt.empty()) s += U8(" ; format X : ") + W8(f.xFmt.c_str());
  if (!f.yFmt.empty()) s += U8(" ; format Y : ") + W8(f.yFmt.c_str());
  if (!f.dateFmt.empty()) s += U8(" ; date : ") + W8(f.dateFmt.c_str());
  if (!f.timeFmt.empty()) s += U8(" ; heure : ") + W8(f.timeFmt.c_str());
  if (f.skipLines) s += wxString::Format(U8(" ; %d ligne(s) d'entête"), f.skipLines);
  return s;
}

TextFormatDialog::TextFormatDialog(wxWindow* parent, const std::string& path, const TextFormat& init,
                                   int zSign, bool zFeet)
    : wxDialog(parent, wxID_ANY, U8("Format du fichier texte"), wxDefaultPosition, wxDefaultSize,
               wxDEFAULT_DIALOG_STYLE | wxRESIZE_BORDER),
      m_path(path),
      m_zSignV(zSign),
      m_zFeetV(zFeet) {
  std::string err;
  if (!path.empty()) ReadTextHead(path, kPreviewLines, m_lines, err);
  m_fmt = init.custom ? init : GuessTextFormat(m_lines);
  m_fmt.custom = true;
  for (const auto& l : m_lines) m_nCols = std::max(m_nCols, (int)SplitFields(l, m_fmt).size());

  auto* col = new wxBoxSizer(wxVERTICAL);
  // ---- lecture des lignes
  auto* top = new wxFlexGridSizer(4, 6, 12);
  m_skip = new wxSpinCtrl(this, wxID_ANY, "", wxDefaultPosition, wxSize(FromDIP(70), -1),
                          wxSP_ARROW_KEYS, 0, 100000, 0);
  wxString seps[] = {U8("Automatique"), U8("Espaces / tabulations"), U8("Virgule"), U8("Point-virgule"),
                     U8("Tabulation")};
  m_sep = new wxChoice(this, wxID_ANY, wxDefaultPosition, wxDefaultSize, 5, seps);
  wxString decs[] = {U8("Point (12.5)"), U8("Virgule (12,5)")};
  m_dec = new wxChoice(this, wxID_ANY, wxDefaultPosition, wxDefaultSize, 2, decs);
  wxString orders[] = {U8("Automatique"), U8("Année-mois-jour"), U8("Jour/mois/année"),
                       U8("Mois/jour/année")};
  m_dateOrder = new wxChoice(this, wxID_ANY, wxDefaultPosition, wxDefaultSize, 4, orders);
  auto L = [this](const char* t) { return new wxStaticText(this, wxID_ANY, U8(t)); };
  top->Add(L("Lignes d'entête à ignorer"), 0, wxALIGN_CENTER_VERTICAL);
  top->Add(m_skip, 0);
  top->Add(L("Séparateur de colonnes"), 0, wxALIGN_CENTER_VERTICAL);
  top->Add(m_sep, 0, wxEXPAND);
  top->Add(L("Séparateur décimal"), 0, wxALIGN_CENTER_VERTICAL);
  top->Add(m_dec, 0, wxEXPAND);
  top->Add(L("Ordre des dates"), 0, wxALIGN_CENTER_VERTICAL);
  top->Add(m_dateOrder, 0, wxEXPAND);
  auto* topRow = new wxBoxSizer(wxHORIZONTAL);
  topRow->Add(top, 1, wxEXPAND);
  auto* tools = new wxBoxSizer(wxVERTICAL);
  auto* guess = new wxButton(this, wxID_ANY, U8("Détection automatique"));
  auto* save = new wxButton(this, wxID_ANY, U8("Enregistrer le format…"));
  auto* load = new wxButton(this, wxID_ANY, U8("Charger un format…"));
  tools->Add(guess, 0, wxEXPAND | wxBOTTOM, 4);
  tools->Add(save, 0, wxEXPAND | wxBOTTOM, 4);
  tools->Add(load, 0, wxEXPAND | wxBOTTOM, 4);
  auto* help = new wxButton(this, wxID_ANY, U8("Aide sur les formats…"));
  tools->Add(help, 0, wxEXPAND);
  help->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { ShowFormatHelp(this); });
  topRow->Add(tools, 0, wxLEFT, 12);
  col->Add(topRow, 0, wxEXPAND | wxALL, 8);

  // ---- rôle des colonnes
  auto* rb = new wxStaticBoxSizer(wxHORIZONTAL, this, U8("Colonnes"));
  // chaque rôle : colonne, puis gabarit facultatif (X, Y, date, heure) ou
  // convention et unité (Z)
  wxWindow* sbw = rb->GetStaticBox();
  auto role = [&](const char* name, const wxColour& c, wxChoice*& ch) {
    auto* t = new wxStaticText(sbw, wxID_ANY, U8(name), wxDefaultPosition, wxDefaultSize,
                               wxALIGN_CENTER_HORIZONTAL);
    t->SetBackgroundColour(c);
    ch = new wxChoice(sbw, wxID_ANY);
    auto* v = new wxBoxSizer(wxVERTICAL);
    v->Add(t, 0, wxEXPAND | wxBOTTOM, 2);
    v->Add(ch, 0, wxEXPAND);
    rb->Add(v, 1, wxALL, 4);
    return v;
  };
  auto pattern = [&](wxBoxSizer* v, wxTextCtrl*& tc, const char* hint, const char* tip) {
    tc = new wxTextCtrl(sbw, wxID_ANY, "");
    tc->SetHint(U8(hint));
    tc->SetToolTip(U8(tip));
    v->Add(tc, 0, wxEXPAND | wxTOP, 4);
  };
  const char* coordTip =
      "Gabarit de la coordonnée en degrés, minutes, secondes : D M S X (ex. « D M S.S X », "
      "« DDMM.MX »). Vide : nombre décimal. Voir « Aide sur les formats ».";
  pattern(role("X — longitude / Est", kColX, m_x), m_xFmt, "format : nombre", coordTip);
  pattern(role("Y — latitude / Nord", kColY, m_y), m_yFmt, "format : nombre", coordTip);
  {
    wxBoxSizer* v = role("Z — profondeur", kColZ, m_z);
    wxString signs[] = {U8("Sens : automatique"), U8("Profondeurs (positives vers le bas)"),
                        U8("Altitudes (positives vers le haut)")};
    m_zSign = new wxChoice(sbw, wxID_ANY, wxDefaultPosition, wxDefaultSize, 3, signs);
    m_zSign->SetSelection(std::max(0, std::min(2, m_zSignV)));
    m_zSign->SetToolTip(U8("Sens des valeurs Z du fichier. Automatique : altitudes si la majorité des "
                           "valeurs est négative."));
    wxString units[] = {U8("Mètres"), U8("Pieds")};
    m_zUnit = new wxChoice(sbw, wxID_ANY, wxDefaultPosition, wxDefaultSize, 2, units);
    m_zUnit->SetSelection(m_zFeetV ? 1 : 0);
    m_zUnit->SetToolTip(U8("Unité des valeurs Z du fichier (la base est toujours en mètres)."));
    v->Add(m_zSign, 0, wxEXPAND | wxTOP, 4);
    v->Add(m_zUnit, 0, wxEXPAND | wxTOP, 4);
  }
  pattern(role("Date (ou date et heure)", kColDate, m_date), m_dateFmt, "format : automatique",
          "Gabarit de la date : y M d h m s z AP (ex. « yyyy/M/d », « d/M/y h:m:s », « MMM d, y »). "
          "Vide : reconnaissance automatique. Voir « Aide sur les formats ».");
  pattern(role("Heure", kColTime, m_time), m_timeFmt, "format : automatique",
          "Gabarit de l'heure (ex. « h:m:s.s », « hhmmss », « h:m:sAP »). Vide : reconnaissance "
          "automatique. Voir « Aide sur les formats ».");
  col->Add(rb, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, 8);

  // ---- aperçu
  m_grid = new wxGrid(this, wxID_ANY, wxDefaultPosition, FromDIP(wxSize(760, 300)));
  m_grid->CreateGrid((int)std::max<size_t>(1, m_lines.size()), std::max(1, std::min(kMaxCols, m_nCols)));
  m_grid->EnableEditing(false);
  m_grid->EnableDragGridSize(false);
  m_grid->SetDefaultCellOverflow(false);
  m_grid->SetRowLabelSize(FromDIP(48));
  m_grid->SetColLabelSize(FromDIP(22));
  col->Add(new wxStaticText(this, wxID_ANY, U8("Aperçu des premières lignes du fichier :")), 0,
           wxLEFT | wxRIGHT, 8);
  col->Add(m_grid, 1, wxEXPAND | wxALL, 8);
  m_sample = new wxStaticText(this, wxID_ANY, "");
  m_status = new wxStaticText(this, wxID_ANY, "");
  col->Add(m_sample, 0, wxEXPAND | wxLEFT | wxRIGHT, 8);
  col->Add(m_status, 0, wxEXPAND | wxALL, 8);

  auto* btns = new wxStdDialogButtonSizer();
  m_ok = new wxButton(this, wxID_OK, U8("OK"));
  btns->AddButton(m_ok);
  btns->AddButton(new wxButton(this, wxID_CANCEL, U8("Annuler")));
  btns->Realize();
  col->Add(btns, 0, wxEXPAND | wxALL, 8);
  SetSizerAndFit(col);

  FillColumnChoices();
  ToControls();
  RefreshPreview();

  auto changed = [this](wxCommandEvent&) {
    if (m_updating) return;
    FromControls();
    RefreshPreview();
  };
  for (wxChoice* c : {m_sep, m_dec, m_dateOrder, m_x, m_y, m_z, m_date, m_time})
    c->Bind(wxEVT_CHOICE, changed);
  for (wxChoice* c : {m_zSign, m_zUnit})
    c->Bind(wxEVT_CHOICE, [this](wxCommandEvent&) {
      m_zSignV = m_zSign->GetSelection();
      m_zFeetV = m_zUnit->GetSelection() == 1;
      RefreshPreview();
    });
  for (wxTextCtrl* tc : {m_xFmt, m_yFmt, m_dateFmt, m_timeFmt})
    tc->Bind(wxEVT_TEXT, [this](wxCommandEvent&) {
      if (m_updating) return;
      FromControls();
      RefreshPreview();
    });
  m_skip->Bind(wxEVT_SPINCTRL, [this](wxSpinEvent&) {
    if (m_updating) return;
    FromControls();
    RefreshPreview();
  });
  // séparateur ou décimale changés : le nombre de colonnes peut changer
  for (wxChoice* c : {m_sep, m_dec})
    c->Bind(wxEVT_CHOICE, [this](wxCommandEvent& e) {
      FromControls();
      m_nCols = 0;
      for (const auto& l : m_lines) m_nCols = std::max(m_nCols, (int)SplitFields(l, m_fmt).size());
      FillColumnChoices();
      ToControls();
      RefreshPreview();
      e.Skip(false);
    });
  guess->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) {
    m_fmt = GuessTextFormat(m_lines);
    m_nCols = 0;
    for (const auto& l : m_lines) m_nCols = std::max(m_nCols, (int)SplitFields(l, m_fmt).size());
    FillColumnChoices();
    ToControls();
    RefreshPreview();
  });
  save->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) {
    wxFileDialog fd(this, U8("Enregistrer le format"), "", "format.pdfmt",
                    U8("Format PersonalDepth (*.pdfmt)|*.pdfmt"), wxFD_SAVE | wxFD_OVERWRITE_PROMPT);
    if (fd.ShowModal() != wxID_OK) return;
    std::ofstream o(U8Path(fd.GetPath().ToStdString(wxConvUTF8)));
    o << m_fmt.serialize() << "\n";
    if (!o) wxMessageBox(U8("Écriture impossible"), U8("Format du fichier texte"), wxICON_ERROR, this);
  });
  load->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) {
    wxFileDialog fd(this, U8("Charger un format"), "", "",
                    U8("Format PersonalDepth (*.pdfmt)|*.pdfmt"), wxFD_OPEN | wxFD_FILE_MUST_EXIST);
    if (fd.ShowModal() != wxID_OK) return;
    std::ifstream in(U8Path(fd.GetPath().ToStdString(wxConvUTF8)));
    std::string s;
    std::getline(in, s);
    TextFormat f;
    if (!f.deserialize(s) || !f.custom) {
      wxMessageBox(U8("Fichier de format illisible"), U8("Format du fichier texte"), wxICON_ERROR, this);
      return;
    }
    m_fmt = f;
    m_nCols = 0;
    for (const auto& l : m_lines) m_nCols = std::max(m_nCols, (int)SplitFields(l, m_fmt).size());
    FillColumnChoices();
    ToControls();
    RefreshPreview();
  });
  SetSize(FromDIP(wxSize(820, 640)));
  CentreOnParent();
}

// Listes des colonnes : « 3 — 48.377952072 » (valeur de la 1re ligne de données)
void TextFormatDialog::FillColumnChoices() {
  m_updating = true;
  std::vector<std::string> first;
  for (size_t i = (size_t)m_fmt.skipLines; i < m_lines.size(); ++i)
    if (!IsComment(m_lines[i])) { first = SplitFields(m_lines[i], m_fmt); break; }
  const int n = std::max({m_nCols, m_fmt.colX, m_fmt.colY, m_fmt.colZ, m_fmt.colDate, m_fmt.colTime, 3});
  for (wxChoice* c : {m_x, m_y, m_z, m_date, m_time}) {
    c->Clear();
    if (c == m_date || c == m_time) c->Append(U8("— (aucune)"));
    for (int k = 1; k <= n; ++k) {
      wxString s = wxString::Format(U8("Colonne %d"), k);
      if (k <= (int)first.size()) s += W8(" — ") + W8(first[k - 1].substr(0, 24).c_str());
      c->Append(s);
    }
  }
  m_updating = false;
}

void TextFormatDialog::ToControls() {
  m_updating = true;
  m_skip->SetValue(m_fmt.skipLines);
  int si = 0;
  for (int k = 0; k < 5; ++k) if (kSeps[k] == m_fmt.sep) si = k;
  m_sep->SetSelection(si);
  m_dec->SetSelection(m_fmt.decimal == ',' ? 1 : 0);
  m_dateOrder->SetSelection(std::max(0, std::min(3, m_fmt.dateOrder)));
  auto setc = [](wxChoice* c, int v, bool optional) {
    const int i = optional ? v : v - 1;
    if (i >= 0 && i < (int)c->GetCount()) c->SetSelection(i);
    else c->SetSelection(optional ? 0 : wxNOT_FOUND);
  };
  setc(m_x, m_fmt.colX, false);
  setc(m_y, m_fmt.colY, false);
  setc(m_z, m_fmt.colZ, false);
  setc(m_date, m_fmt.colDate, true);
  setc(m_time, m_fmt.colTime, true);
  m_xFmt->ChangeValue(W8(m_fmt.xFmt.c_str()));
  m_yFmt->ChangeValue(W8(m_fmt.yFmt.c_str()));
  m_dateFmt->ChangeValue(W8(m_fmt.dateFmt.c_str()));
  m_timeFmt->ChangeValue(W8(m_fmt.timeFmt.c_str()));
  m_updating = false;
}

void TextFormatDialog::FromControls() {
  m_fmt.custom = true;
  m_fmt.skipLines = m_skip->GetValue();
  m_fmt.sep = kSeps[std::max(0, m_sep->GetSelection())];
  m_fmt.decimal = m_dec->GetSelection() == 1 ? ',' : '.';
  m_fmt.dateOrder = std::max(0, m_dateOrder->GetSelection());
  m_fmt.colX = m_x->GetSelection() + 1;
  m_fmt.colY = m_y->GetSelection() + 1;
  m_fmt.colZ = m_z->GetSelection() + 1;
  m_fmt.colDate = std::max(0, m_date->GetSelection());
  m_fmt.colTime = std::max(0, m_time->GetSelection());
  m_fmt.xFmt = m_xFmt->GetValue().Trim().Trim(false).ToStdString(wxConvUTF8);
  m_fmt.yFmt = m_yFmt->GetValue().Trim().Trim(false).ToStdString(wxConvUTF8);
  m_fmt.dateFmt = m_dateFmt->GetValue().Trim().Trim(false).ToStdString(wxConvUTF8);
  m_fmt.timeFmt = m_timeFmt->GetValue().Trim().Trim(false).ToStdString(wxConvUTF8);
}

void TextFormatDialog::RefreshPreview() {
  const TextFormat& f = m_fmt;
  const int nc = std::max(1, std::min(kMaxCols, std::max(m_nCols, 1)));
  m_grid->BeginBatch();
  if (m_grid->GetNumberCols() < nc) m_grid->AppendCols(nc - m_grid->GetNumberCols());
  if (m_grid->GetNumberCols() > nc) m_grid->DeleteCols(nc, m_grid->GetNumberCols() - nc);
  for (int c = 0; c < nc; ++c) {
    wxString lab = wxString::Format("%d", c + 1);
    if (c + 1 == f.colX) lab += W8(" · X");
    if (c + 1 == f.colY) lab += W8(" · Y");
    if (c + 1 == f.colZ) lab += W8(" · Z");
    if (c + 1 == f.colDate) lab += U8(" · date");
    if (c + 1 == f.colTime) lab += U8(" · heure");
    m_grid->SetColLabelValue(c, lab);
  }
  int data = 0, good = 0;
  wxString sample;
  for (size_t r = 0; r < m_lines.size() && (int)r < m_grid->GetNumberRows(); ++r) {
    const std::string& line = m_lines[r];
    const bool skipped = (int)r < f.skipLines || IsComment(line);
    std::vector<std::string> v = SplitFields(line, f);
    double x = 0, y = 0, z = 0;
    long long t = 0;
    bool ok = !skipped && (int)v.size() >= std::max({f.colX, f.colY, f.colZ});
    if (!skipped) ++data;
    auto fieldOk = [&](int c) -> bool {  // c : 1-based
      if (c < 1 || c > (int)v.size()) return false;
      double d;
      if (c == f.colX) return ParseFieldCoord(v[c - 1], f, false, d);
      if (c == f.colY) return ParseFieldCoord(v[c - 1], f, true, d);
      if (c == f.colZ) return ParseFieldNumber(v[c - 1], f.decimal, d);
      if (c == f.colDate) {
        const std::string tm = (f.colTime && f.colTime <= (int)v.size()) ? v[f.colTime - 1] : "";
        return ParseFieldDateTime(v[c - 1], tm, f, t);
      }
      if (c == f.colTime) return f.colDate > 0 && f.colDate <= (int)v.size();
      return true;
    };
    for (int c = 0; c < nc; ++c) {
      const wxString txt = c < (int)v.size() ? W8(v[c].c_str()) : wxString();
      m_grid->SetCellValue((int)r, c, txt);
      wxColour bg = *wxWHITE, fg = *wxBLACK;
      if (skipped) {
        bg = kSkipBg;
        fg = kSkipFg;
      } else {
        const int k = c + 1;
        if (k == f.colX) bg = kColX;
        else if (k == f.colY) bg = kColY;
        else if (k == f.colZ) bg = kColZ;
        else if (k == f.colDate) bg = kColDate;
        else if (k == f.colTime) bg = kColTime;
        const bool role = k == f.colX || k == f.colY || k == f.colZ || k == f.colDate || k == f.colTime;
        if (role && !fieldOk(k)) {
          bg = kBadBg;
          fg = *wxWHITE;
          ok = false;
        }
      }
      m_grid->SetCellBackgroundColour((int)r, c, bg);
      m_grid->SetCellTextColour((int)r, c, fg);
    }
    if (ok) {
      ParseFieldCoord(v[f.colX - 1], f, false, x);
      ParseFieldCoord(v[f.colY - 1], f, true, y);
      ParseFieldNumber(v[f.colZ - 1], f.decimal, z);
      ++good;
      if (sample.empty()) {
        sample = wxString::Format(U8("1re sonde : X = %.8g   Y = %.8g   Z = %.4g"), x, y, z);
        if (m_zSignV != 0) {  // profondeur stockée (avant transformation verticale)
          const double dz = (m_zSignV == 2 ? -z : z) * (m_zFeetV ? 0.3048 : 1.0);
          sample += wxString::Format(U8(" → profondeur %.3f m"), dz);
        }
        if (f.colDate && t) {
          const time_t tt = (time_t)t;
          char b[32];
          std::strftime(b, sizeof b, "%Y-%m-%d %H:%M:%S", std::gmtime(&tt));
          sample += W8("   — ") + W8(b) + " UTC";
        }
      }
    }
  }
  m_grid->AutoSizeColumns(false);
  m_grid->EndBatch();
  m_sample->SetLabel(sample);
  if (m_lines.empty())
    m_status->SetLabel(U8("Aucun fichier à prévisualiser : le format sera appliqué à l'import."));
  else
    m_status->SetLabel(wxString::Format(U8("Aperçu : %d ligne(s) de données lisible(s) sur %d. "
                                           "Cases rouges : valeur illisible pour ce rôle."),
                                        good, data));
  m_status->SetForegroundColour(good > 0 || m_lines.empty() ? *wxBLACK : wxColour(200, 0, 0));
  m_ok->Enable(good > 0 || m_lines.empty());
  Layout();
}

// ------------------------------------------------------- aide sur les gabarits
void TextFormatDialog::ShowFormatHelp(wxWindow* parent) {
  wxDialog d(parent, wxID_ANY, U8("Aide sur les formats"), wxDefaultPosition, wxDefaultSize,
             wxDEFAULT_DIALOG_STYLE | wxRESIZE_BORDER);
  auto* col = new wxBoxSizer(wxVERTICAL);
  auto table = [&](const char* title, std::initializer_list<const char*> heads,
                   std::initializer_list<std::initializer_list<const char*>> rows, int h) {
    auto* t = new wxStaticText(&d, wxID_ANY, U8(title));
    t->SetFont(t->GetFont().Bold());
    col->Add(t, 0, wxLEFT | wxRIGHT | wxTOP, 8);
    auto* lc = new wxListCtrl(&d, wxID_ANY, wxDefaultPosition, wxSize(-1, d.FromDIP(h)),
                              wxLC_REPORT | wxLC_SINGLE_SEL | wxLC_HRULES | wxLC_VRULES);
    int c = 0;
    for (const char* hd : heads) lc->AppendColumn(U8(hd), wxLIST_FORMAT_LEFT, d.FromDIP(c ? 190 : 170)), ++c;
    long r = 0;
    for (const auto& row : rows) {
      int k = 0;
      for (const char* cell : row) {
        if (k == 0) lc->InsertItem(r, W8(cell));
        else lc->SetItem(r, k, k == (int)row.size() - 1 ? U8(cell) : W8(cell));
        ++k;
      }
      ++r;
    }
    col->Add(lc, 1, wxEXPAND | wxALL, 8);
  };
  table("Date et heure — champs", {"Caractère", "Champ"},
        {{"y", "Année (yy : 2 chiffres)"}, {"M", "Mois (majuscule ; MMM : nom du mois)"},
         {"d", "Jour"}, {"h", "Heure (0-24)"}, {"m", "Minute (minuscule)"},
         {"s", "Secondes (s.s : avec décimales)"}, {"z", "Millisecondes"}, {"AP", "AM / PM"}},
        150);
  table("Date et heure — exemples", {"Texte", "Format", "Remarque"},
        {{"02/11/1983", "M/d/y", "11 février 1983 (attention à l'ordre jour / mois)"},
         {"02/11/1983", "d/M/y", "2 novembre 1983"},
         {"2021/03/25 08:10:23.000", "y/M/d h:m:s.s", "Date et heure dans la même colonne"},
         {"02/11/1983 8:07:34PM", "M/d/y h:m:sAP", "Heure sur 24 h sauf champ AM/PM"},
         {"02/11/1983 8:07:34:076", "M/d/y h:m:s:z", "z : millièmes de seconde"},
         {"02/11/83 8:07", "M/d/yy h:m", "yy : 1970-2069"},
         {"February 11, 1983", "MMM d, y", "Nom du mois (anglais ou français) ou abréviation"},
         {"1983-Feb-11 8:07:34", "y-MMM-d h:m:s", "Abréviation à 3 lettres"},
         {"19830211080734", "yyyyMMddhhmmss", "Lettres répétées collées : largeur fixe"},
         {"1983y02M11d 8h07m34s", "y\\yM\\Md\\d h\\hm\\ms\\s", "\\ : caractère suivant littéral"},
         {"08:10:23.000", "h:m:s.s", "Colonne Heure séparée"}},
        230);
  table("Coordonnées X / Y — champs", {"Caractère", "Champ"},
        {{"D", "Degrés (D.D : avec décimales)"}, {"M", "Minutes (M.M : avec décimales)"},
         {"S", "Secondes (S.S : avec décimales)"}, {"X", "Hémisphère N, S, E, W (ou O)"}},
        110);
  table("Coordonnées X / Y — exemples", {"Texte", "Format", "Remarque"},
        {{"45.95536", "D.D", "Degrés décimaux (comme le champ vide)"},
         {"-66 38.90778", "D M.M", "Degrés et minutes décimales"},
         {"45 57 19.36", "D M S.S", "Degrés, minutes, secondes décimales"},
         {"66 38' 54.47\" W", "D M' S.S\" X", "La ponctuation doit figurer dans le format"},
         {"45.95536 S", "D.D X", "Hémisphères sud et ouest : valeur négative"},
         {"-66.648463 W", "D.D X", "Nombre négatif dans un hémisphère négatif : positif"},
         {"4822.6771N", "DDMM.MX", "Format NMEA : largeur fixe"},
         {"45D57M19.36S", "D\\DM\\MS.S\\S", "\\ : caractère suivant littéral"}},
        210);
  auto* btns = new wxStdDialogButtonSizer();
  btns->AddButton(new wxButton(&d, wxID_OK, U8("Fermer")));
  btns->Realize();
  col->Add(btns, 0, wxEXPAND | wxALL, 8);
  d.SetSizerAndFit(col);
  d.SetSize(d.FromDIP(wxSize(700, 820)));
  d.CentreOnParent();
  d.ShowModal();
}

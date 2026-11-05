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

// Fenêtre « Courbe de marée » : hauteur d'eau en fonction du temps pour
// chaque point de marée utilisé (station OpenCPN ou points FES2022), légende
// à cocher, bornes automatiques (un cycle autour de l'instant choisi) ou
// saisies, marqueur de l'instant choisi.
#pragma once
#include <wx/wx.h>
#include <wx/datectrl.h>
#include <wx/dateevt.h>
#include <wx/spinctrl.h>
#include <wx/timectrl.h>

#include <ctime>
#include <string>
#include <vector>

#include "PersonalDepth_pi.h"

class TideChartPanel;

class TideChartDialog : public wxDialog {
 public:
  TideChartDialog(wxWindow* parent, PersonalDepth_pi* pi);
  // nouvel instant, nouveaux points ou autre source de marée
  void TideUpdated();

  // données lues par le panneau de tracé
  struct Series {
    TideChartPoint pt;
    std::vector<double> h;  // unité de hauteur affichée, NaN si absente
    bool visible = true;
    wxColour colour;
  };
  std::vector<Series> m_series;
  std::vector<time_t> m_t;
  time_t m_x0 = 0, m_x1 = 0, m_marker = 0;
  double m_y0 = 0, m_y1 = 1;
  wxString m_unit = "m", m_message, m_source;

 private:
  void Recompute(bool keepVisibility);
  void AutoX();
  void AutoY();
  void FillBoundsControls();
  void OnBoundsEdited();
  void OnAuto();

  PersonalDepth_pi* m_pi;
  TideChartPanel* m_panel = nullptr;
  wxCheckBox* m_auto = nullptr;
  wxDatePickerCtrl *m_d0 = nullptr, *m_d1 = nullptr;
  wxTimePickerCtrl *m_h0 = nullptr, *m_h1 = nullptr;
  wxSpinCtrlDouble *m_ymin = nullptr, *m_ymax = nullptr;
  wxStaticText* m_yUnit = nullptr;
  bool m_updating = false;
  std::string m_key;  // points + source : recalcul si différent
};

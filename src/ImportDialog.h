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

// Fenêtre d'import dans la base : fichiers, lecture des colonnes, convention
// de signe, système de coordonnées, référence verticale des Z et
// transformation vers le zéro hydrographique, classe. Les réglages sont
// mémorisés pour l'import suivant.
#pragma once
#include <wx/wx.h>
#include <wx/combobox.h>
#include <wx/spinctrl.h>

#include <string>
#include <vector>

#include "core/BathyLayer.h"
#include "core/VerticalRef.h"

class PersonalDepth_pi;

class ImportDialog : public wxDialog {
 public:
  ImportDialog(wxWindow* parent, PersonalDepth_pi* pi);
  // Valides après ShowModal() == wxID_OK
  const std::vector<pd::LayerParams>& Params() const { return m_out; }
  const std::string& Group() const { return m_group; }

 private:
  wxSizer* BuildFiles(wxWindow* p);
  wxSizer* BuildReading(wxWindow* p);
  wxSizer* BuildCrs(wxWindow* p);
  wxSizer* BuildVref(wxWindow* p);
  void AddPaths(const wxArrayString& paths);
  void UpdateCrsControls();
  void UpdateVrefControls();
  void UpdateSummary();
  bool CurrentCrs(std::string& def, std::string& name);
  bool CurrentVref(pd::VerticalRefParams& v);
  void OnCrsChoice(wxCommandEvent& ev);
  void OnOk(wxCommandEvent&);

  PersonalDepth_pi* m_pi;
  std::vector<pd::LayerParams> m_out;
  std::string m_group;
  // Fichiers
  wxListBox* m_files = nullptr;
  wxStaticText* m_count = nullptr;
  // Lecture
  wxChoice *m_order = nullptr, *m_sign = nullptr;
  wxSpinCtrlDouble* m_offset = nullptr;
  wxCheckBox* m_cache = nullptr;
  wxCheckBox *m_nodataOn = nullptr, *m_boundsOn = nullptr;
  wxComboBox* m_nodata = nullptr;
  wxSpinCtrlDouble *m_boundMin = nullptr, *m_boundMax = nullptr;
  // Coordonnées
  wxChoice* m_crs = nullptr;
  wxSpinCtrl* m_zone = nullptr;
  wxChoice* m_hemi = nullptr;
  wxTextCtrl* m_crsCustom = nullptr;
  wxCheckBox* m_prj = nullptr;
  // Référence verticale
  wxChoice* m_vref = nullptr;
  wxTextCtrl *m_vrFrance = nullptr, *m_vrZh = nullptr, *m_vrGeoid = nullptr, *m_vrDirect = nullptr;
  wxButton *m_vrFranceBtn = nullptr, *m_vrZhBtn = nullptr, *m_vrGeoidBtn = nullptr,
           *m_vrDirectBtn = nullptr;
  wxCheckBox* m_vrInvert = nullptr;
  wxStaticText* m_vrHelp = nullptr;
  // Classement
  wxComboBox* m_class = nullptr;
};

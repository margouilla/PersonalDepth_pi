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

// Fenêtre « Gestion des données » : import dans la base, liste des sources
// importées et de leurs métadonnées, priorité, classes, nettoyage, retrait,
// découpage des fichiers .pdb. Non modale, ouverte depuis le panneau principal.
#pragma once
#include <wx/wx.h>
#include <wx/combobox.h>
#include <wx/treelist.h>

#include <set>
#include <string>
#include <vector>

class PersonalDepth_pi;

class DataDialog : public wxDialog {
 public:
  DataDialog(wxWindow* parent, PersonalDepth_pi* pi);
  void RefreshDb();                    // sources et découpage
  void SetDbStatus(const wxString& t);  // opération en cours / dernier résultat

 private:
  std::vector<int64_t> SelectedSources() const;
  void UpdateButtons();
  void UpdateDetails();
  void UpdateSplitInfo();
  void OnImport();
  void OnClean();
  void OnResplit();

  PersonalDepth_pi* m_pi;
  wxChoice* m_prio = nullptr;
  wxStaticText* m_split = nullptr;
  wxButton* m_splitBtn = nullptr;
  wxTreeListCtrl* m_tree = nullptr;
  wxTextCtrl* m_details = nullptr;
  wxButton *m_import = nullptr, *m_up = nullptr, *m_down = nullptr, *m_del = nullptr,
           *m_clean = nullptr, *m_zoom = nullptr, *m_setClass = nullptr, *m_stop = nullptr;
  wxComboBox* m_class = nullptr;
  wxStaticText* m_status = nullptr;
  wxGauge* m_gauge = nullptr;
  std::set<std::string> m_collapsed;  // classes repliées dans la liste
};

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

// Fenêtre « Format du fichier texte » : lignes d'entête, séparateurs et rôle
// des colonnes (X, Y, Z, date, heure), avec aperçu coloré des premières
// lignes du fichier.
#pragma once
#include <wx/wx.h>
#include <wx/spinctrl.h>

#include <string>
#include <vector>

#include "core/PointCloud.h"

class wxGrid;

class TextFormatDialog : public wxDialog {
 public:
  // `path` : fichier d'exemple (aperçu) ; `init` non personnalisé -> format deviné
  // zSign : 0 automatique, 1 profondeurs positives, 2 altitudes ; zFeet : Z en pieds
  // water : fichier de hauteurs d'eau (date, heure, hauteur ; pas de X / Y) ;
  // zSign vaut alors 0 (profondeur = sonde − hauteur) ou 1 (sonde + hauteur)
  TextFormatDialog(wxWindow* parent, const std::string& path, const pd::TextFormat& init,
                   int zSign, bool zFeet, bool water = false);
  pd::TextFormat Format() const { return m_fmt; }
  int ZSign() const { return m_zSignV; }
  bool ZFeet() const { return m_zFeetV; }
  // Résumé d'un format (« X : col. 4, Y : col. 3, Z : col. 5, date… »)
  static wxString Describe(const pd::TextFormat& f);
  // Aide sur les gabarits de date / heure et de coordonnées
  static void ShowFormatHelp(wxWindow* parent);

 private:
  void FromControls();
  void ToControls();
  void RefreshPreview();
  void FillColumnChoices();

  std::string m_path;
  std::vector<std::string> m_lines;
  pd::TextFormat m_fmt;
  int m_nCols = 0;
  bool m_updating = false;
  wxSpinCtrl* m_skip = nullptr;
  wxChoice *m_sep = nullptr, *m_dec = nullptr, *m_dateOrder = nullptr;
  wxChoice *m_x = nullptr, *m_y = nullptr, *m_z = nullptr, *m_date = nullptr, *m_time = nullptr;
  wxTextCtrl *m_xFmt = nullptr, *m_yFmt = nullptr, *m_dateFmt = nullptr, *m_timeFmt = nullptr;  // gabarits
  wxChoice *m_zSign = nullptr, *m_zUnit = nullptr;  // convention et unité de Z
  wxChoice* m_tz = nullptr;                         // fuseau horaire des dates
  int m_zSignV = 0;
  bool m_zFeetV = false;
  bool m_water = false;  // fichier de hauteurs d'eau
  wxGrid* m_grid = nullptr;
  wxStaticText *m_status = nullptr, *m_sample = nullptr;
  wxButton* m_ok = nullptr;
};

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

// Panneau de contrôle non modal : couches, palette, seuils, affichage.
// Chaque modification est appliquée immédiatement à la carte.
#pragma once
#include <wx/wx.h>
#include <wx/clrpicker.h>
#include <wx/grid.h>
#include <wx/combobox.h>
#include <wx/treelist.h>

#include <set>
#include <string>
#include <vector>
#include <wx/spinctrl.h>
#include <wx/datectrl.h>
#include <wx/dateevt.h>
#include <wx/timectrl.h>

#include "core/BathyLayer.h"
#include "core/VerticalRef.h"

class PersonalDepth_pi;
class DataDialog;

class BathyDialog : public wxDialog {
 public:
  BathyDialog(wxWindow* parent, PersonalDepth_pi* pi);
  void RefreshLayers() {}  // (couches fichiers supprimées : tout passe par la base)
  void RefreshPalette();  // recharge la grille des seuils depuis les réglages
  void RefreshUnits();    // unités d'affichage d'OpenCPN changées (m / ft)
  void SelectPreset(const std::string& name);  // préréglage choisi ailleurs (bandeau)
  void SetCursorText(const wxString& t);
  void SetTideInfo(const wxString& t);
  void RefreshDb();                       // liste des sources de la base
  void SetDbStatus(const wxString& t);

 private:
  wxSizer* BuildPaletteBox(wxWindow* p);
  wxSizer* BuildDisplayBox(wxWindow* p);
  wxSizer* BuildContourBox(wxWindow* p);
  wxSizer* BuildTideBox(wxWindow* p);
  wxSizer* BuildDbBox(wxWindow* p);
  std::vector<std::string> SelectedCells() const;
  void RefreshCells();
  void UpdateDbButtons();
  void OnTide(wxCommandEvent&);
  void UpdateTideControls();
  void OnContours(wxCommandEvent&);
  void ApplyPaletteFromUI();
  void FillStopRow(int row);
  // profondeurs : stockées en m, affichées dans l'unité d'OpenCPN
  float K() const;
  float FromUi(double ui, float storedM, int digits) const;  // garde la valeur si inchangée
  void OnAuto(wxCommandEvent&);
  void StopAuto();  // modification manuelle des seuils : la palette redevient fixe

  void OnPreset(wxCommandEvent&);
  void OnMode(wxCommandEvent&);
  void OnStopEdited(wxGridEvent&);
  void OnStopColour(wxGridEvent&);
  void OnAddStop(wxCommandEvent&);
  void OnDelStop(wxCommandEvent&);
  void OnImportCPT(wxCommandEvent&);
  void OnExportCPT(wxCommandEvent&);
  void OnSafety(wxCommandEvent&);
  void OnDisplay(wxCommandEvent&);
  void OnExaggeration(wxCommandEvent&);
  void OnClose(wxCloseEvent&);

  PersonalDepth_pi* m_pi;
  bool m_updating = false;
  // Base de bathymétrie : fichiers .pdb affichés ; le reste dans DataDialog
  wxTextCtrl* m_dbFolder = nullptr;
  wxButton* m_dbFolderBtn = nullptr;
  wxTreeListCtrl* m_cells = nullptr;
  wxButton *m_dbManage = nullptr, *m_cellAll = nullptr, *m_cellNone = nullptr,
           *m_cellZoom = nullptr, *m_dbStop = nullptr;
  wxStaticText* m_dbStatus = nullptr;
  DataDialog* m_data = nullptr;  // fenêtre « Gestion des données » (enfant)
  // Marée
  wxChoice *m_tideMode, *m_tideSrc;
  wxTextCtrl *m_tideAtlas, *m_tideZ0;
  wxButton *m_tideAtlasBtn, *m_tideZ0Btn;
  wxCheckBox* m_tideBE;
  wxDatePickerCtrl* m_tideDate;
  wxTimePickerCtrl* m_tideTime;
  wxSlider* m_tideShift;
  wxStaticText* m_tideShiftLbl = nullptr;
  wxStaticText* m_tideInfo = nullptr;
  // Palette
  wxChoice* m_preset;
  wxCheckBox *m_autoOn = nullptr, *m_autoNice = nullptr;
  wxChoice* m_autoSrc = nullptr;
  wxSpinCtrl* m_autoCount = nullptr;
  wxStaticText* m_autoInfo = nullptr;
  wxStaticText *m_autoMinU = nullptr, *m_autoMaxU = nullptr, *m_safeU = nullptr,
               *m_ctStepL = nullptr, *m_ctMajorL = nullptr;
  wxCheckBox *m_autoMinOn = nullptr, *m_autoMaxOn = nullptr;
  wxSpinCtrlDouble *m_autoMin = nullptr, *m_autoMax = nullptr;
  wxRadioBox* m_mode;
  wxGrid* m_stops;
  wxCheckBox* m_safeOn;
  wxSpinCtrlDouble* m_safeDepth;
  wxColourPickerCtrl* m_safeColour;
  // Affichage
  wxSlider *m_opacity, *m_shadeStrength, *m_spacing;
  wxCheckBox *m_shade, *m_sound, *m_smooth, *m_legend, *m_banner;
  wxChoice* m_bannerPos;
  wxSpinCtrlDouble* m_exag;
  wxSpinCtrl *m_decimals, *m_font, *m_shadeSmooth;
  wxStaticText* m_cursor;
  // Isobathes
  wxCheckBox *m_ctOn, *m_ctLabels, *m_ctSmooth;
  wxChoice* m_ctMode;
  wxSpinCtrlDouble *m_ctStep, *m_ctMajor;
  wxColourPickerCtrl* m_ctColour;
};

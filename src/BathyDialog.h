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
#include <wx/notebook.h>
#include <wx/combobox.h>
#include <wx/treelist.h>
#include <wx/listctrl.h>

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
  void SelectTab(int tab);  // 0 Données, 1 Marée, 2 Affichage
  void RefreshPalette();  // recharge la grille des seuils depuis les réglages
  void RefreshUnits();    // unités d'affichage d'OpenCPN changées (m / ft)
  void SelectPreset(const std::string& name);  // préréglage choisi ailleurs (bandeau)
  void RefreshPresetList();                    // préréglages + dossier de palettes
  void SetCursorText(const wxString& t);
  void SetTideInfo(const wxString& t);
  void RefreshDb();                       // liste des sources de la base
  void RefreshDatum();                    // référence verticale (texte, boutons)
  void SetDbStatus(const wxString& t);

 private:
  wxSizer* BuildPaletteBox(wxWindow* p);
  wxSizer* BuildDisplayBox(wxWindow* p);
  void SetWrapped(wxStaticText* w, const wxString& t);  // texte coupé + page réorganisée
  void RelayoutPage(wxWindow* w);                       // page d'onglet contenant w
  wxString m_tideRaw, m_dbRaw;                          // textes avant coupure
  wxSizer* BuildChartTextBox(wxWindow* p);  // isobathes + sondes chiffrées (onglet Affichage)
  wxSizer* BuildTideBox(wxWindow* p);
  wxSizer* BuildDataBox(wxWindow* p);  // onglet Données : bathymétrie et imagerie
  wxSizer* BuildDbBox(wxWindow* p);    // cadre Bathymétrie (et cadre du dossier, m_dbTopBox)
  wxSizer* m_dbTopBox = nullptr;
  wxSizer* BuildDatumBox(wxWindow* p);  // référence verticale de l'affichage
  wxSizer* BuildDispDatumBox(wxWindow* p);  // référence verticale affichée (onglet Affichage)
  wxSizer* BuildLandBox(wxWindow* p);   // trait de côte (pas d'interpolation sur la terre)
  wxSizer* BuildImageryBox(wxWindow* p);  // imagerie du fond : images de la base, import
  void BuildImageryDisplay(wxWindow* p);  // couches affichées et rendu de l'imagerie (onglet Affichage)
  wxStaticBoxSizer *m_layersBox = nullptr, *m_imgRenderBox = nullptr;
  wxCheckBox* m_imgAuto = nullptr;
  wxChoice* m_imgAutoSrc = nullptr;
 public:
  void RefreshImageryDisplay();           // rendu de l'imagerie (bandeau, contraste automatique)
 private:
  void RefreshImagery();                  // liste des images de la base
  void UpdateImgButtons();
  void OnImgDisplay();
  std::vector<int64_t> SelectedImages() const;
  wxCheckBox *m_bathyShow = nullptr, *m_imgShow = nullptr, *m_imgInvert = nullptr, *m_imgSmooth = nullptr;
  wxChoice *m_imgOrder = nullptr, *m_imgPalette = nullptr, *m_imgBg = nullptr;
  wxSlider *m_imgOpacity = nullptr, *m_imgMin = nullptr, *m_imgMax = nullptr;
  wxStaticText *m_imgMinV = nullptr, *m_imgMaxV = nullptr;  // valeurs des bornes noir / blanc
  wxSpinCtrlDouble *m_imgGamma = nullptr, *m_imgBgValue = nullptr, *m_imgSonarPix = nullptr;
  wxCheckBox *m_imgSonarEq = nullptr, *m_imgOffAuto = nullptr;
  wxSpinCtrlDouble *m_imgOffFwd = nullptr, *m_imgOffStbd = nullptr;
  void UpdateSonarOffsetFields();
  wxListCtrl* m_imgList = nullptr;
  wxButton *m_imgAdd = nullptr, *m_imgDel = nullptr, *m_imgVis = nullptr, *m_imgZoom = nullptr,
           *m_imgStop = nullptr, *m_imgReimp = nullptr;
  wxStaticText* m_imgStatus = nullptr;
  wxString m_imgRaw;
  void RefreshLand();
  wxTextCtrl* m_land = nullptr;
  wxButton *m_landBtn = nullptr, *m_landApply = nullptr;
  wxCheckBox* m_landOn = nullptr;
  wxStaticText* m_landInfo = nullptr;
  wxTextCtrl* m_national = nullptr;
  wxButton* m_nationalBtn = nullptr;
  wxTextCtrl* m_shore = nullptr;
  wxButton* m_shoreBtn = nullptr;
  wxCheckBox* m_shoreZero = nullptr;
  wxChoice* m_dispDatum = nullptr;
  wxTextCtrl* m_mss = nullptr;
  wxButton *m_mssBtn = nullptr, *m_datumUpdate = nullptr, *m_datumMigrate = nullptr;
  wxStaticText* m_datumInfo = nullptr;
  wxString m_datumRaw;
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
  void OnSavePalette(wxCommandEvent&);   // enregistrer dans le dossier de palettes
  void OnPaletteDir(wxCommandEvent&);    // choisir le dossier de palettes
  std::vector<std::string> m_presetKeys;
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
  wxCheckBox* m_bannerLock;
  wxCheckBox *m_interpShow, *m_interpDistinct;  // zones interpolées de la base
  wxSpinCtrlDouble* m_exag;
  wxSpinCtrl *m_decimals, *m_font, *m_shadeSmooth;
  wxNotebook* m_tabs = nullptr;
  void FitPages();  // panneau assez grand pour chaque onglet, sans défilement
  wxChoice* m_toolbarMode = nullptr;  // icône de la barre d'outils : panneau / affiche-masque
  wxStaticText* m_cursor;
  // Isobathes
  wxCheckBox *m_ctOn, *m_ctLabels, *m_ctSmooth;
  wxChoice* m_ctMode;
  wxSpinCtrlDouble *m_ctStep, *m_ctMajor;
  wxColourPickerCtrl* m_ctColour;
};

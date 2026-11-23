// PersonalDepth_pi - choix d'un marégraphe en ligne (REFMAR…) : téléchargement
// des hauteurs d'eau d'une période pour l'import, ou choix de la station de
// la source de marée « marégraphe »
// Copyright (C) 2026 Marcaurelio Franzetti
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <wx/wx.h>

#include <vector>

#include "core/TideGauge.h"

class PersonalDepth_pi;
class wxListCtrl;
class wxDatePickerCtrl;

class GaugeDialog : public wxDialog {
 public:
  enum Mode { Download, Pick };
  GaugeDialog(wxWindow* parent, PersonalDepth_pi* pi, Mode mode, const std::string& providerId = "refmar",
              const std::string& selectedId = std::string());
  // Valides après ShowModal() == wxID_OK
  const wxString& Folder() const { return m_folder; }        // Download : dossier des fichiers
  const pd::GaugeStation& Station() const { return m_station; }

 private:
  bool LoadStations(bool refresh);
  void Fill();
  int SelectedIndex() const;
  void OnOk();

  PersonalDepth_pi* m_pi;
  Mode m_mode;
  const pd::GaugeProvider* m_prov = nullptr;
  std::string m_selectedId;
  std::vector<pd::GaugeStation> m_all;
  std::vector<size_t> m_shown;  // index dans m_all des lignes affichées
  double m_lat = 0, m_lon = 0;
  bool m_hasCenter = false;
  wxChoice* m_provChoice = nullptr;
  wxTextCtrl* m_search = nullptr;
  wxListCtrl* m_list = nullptr;
  wxDatePickerCtrl *m_from = nullptr, *m_to = nullptr;
  wxStaticText* m_lic = nullptr;
  wxString m_folder;
  pd::GaugeStation m_station;
};

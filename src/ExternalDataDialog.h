// PersonalDepth_pi - données externes : catalogue, licences et téléchargement
// guidé (BathyElli, RAF20, RAC23…), pages des données à compte (FES2022,
// MSS), conversion de leurs fichiers netCDF et choix des fichiers convertis
// Copyright (C) 2026 Marcaurelio Franzetti
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <wx/wx.h>

#include "core/Catalog.h"
#include "core/NcConvert.h"

class PersonalDepth_pi;
class wxListCtrl;
class wxListEvent;
class wxHyperlinkCtrl;

class ExternalDataDialog : public wxDialog {
 public:
  ExternalDataDialog(wxWindow* parent, PersonalDepth_pi* pi);
  // Réglages modifiés (fichiers des références, marée, MSS, pays) : le
  // panneau relit ses champs
  bool Changed() const { return m_changed; }

 private:
  void Fill();
  void ShowDetails();
  int Selected() const;
  std::string Installed(const pd::CatalogItem& it) const;  // fichier trouvé, vide sinon
  bool Install(const pd::CatalogItem& it, bool askLicence, bool& aborted);
  void InstallMissing();
  void ChooseFile(const pd::CatalogItem& it);
  void Convert(const pd::CatalogItem& it);  // fichier du producteur -> .pdtide / .pdgrid
  bool AskGridOptions(const std::string& stem, pd::PdgridOptions& opt);
  void UpdateCatalog();
  void VerifyFiles();  // empreintes SHA-256 des fichiers installés
  void AfterFranceChange(int n);

  PersonalDepth_pi* m_pi;
  pd::Catalog m_cat;
  wxStaticText* m_dir = nullptr;
  wxListCtrl* m_list = nullptr;
  wxStaticText* m_desc = nullptr;
  wxStaticText* m_lic = nullptr;
  wxHyperlinkCtrl* m_page = nullptr;
  wxHyperlinkCtrl* m_licLink = nullptr;
  wxButton *m_get = nullptr, *m_choose = nullptr, *m_convert = nullptr, *m_all = nullptr;
  bool m_changed = false;
};

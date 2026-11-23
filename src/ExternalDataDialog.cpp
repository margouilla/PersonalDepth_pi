// PersonalDepth_pi - données externes : catalogue, licences et téléchargement guidé
// Copyright (C) 2026 Marcaurelio Franzetti
// SPDX-License-Identifier: GPL-2.0-or-later
#include "ExternalDataDialog.h"

#include <wx/dirdlg.h>
#include <wx/filedlg.h>
#include <wx/filename.h>
#include <wx/hyperlink.h>
#include <wx/listctrl.h>

#include "PersonalDepth_pi.h"
#include "core/Archive.h"
#include "core/FileUtil.h"
#include "core/Lang.h"
#include "core/Sha256.h"

#define U8(s) wxString::FromUTF8(pd::tr(s))
#define W8(s) wxString::FromUTF8(s)  // données : pas de traduction

using namespace pd;

namespace {
std::string LangCode() { return IsEnglish() ? "en" : IsRussian() ? "ru" : "fr"; }

wxString SizeText(long long n) {
  if (n <= 0) return wxEmptyString;
  if (n < 1024 * 1024) return wxString::Format("%lld ", (n + 1023) / 1024) + U8("Ko");
  return wxString::Format("%.1f ", n / (1024.0 * 1024.0)) + U8("Mo");
}

wxString CachePath(const wxString& dir) { return dir + wxFileName::GetPathSeparator() + "catalog.json"; }
}  // namespace

ExternalDataDialog::ExternalDataDialog(wxWindow* parent, PersonalDepth_pi* pi)
    : wxDialog(parent, wxID_ANY, U8("Données externes"), wxDefaultPosition, wxDefaultSize,
               wxDEFAULT_DIALOG_STYLE | wxRESIZE_BORDER),
      m_pi(pi) {
  m_cat = catalog::Load(CachePath(m_pi->ExternalDir()).ToStdString(wxConvUTF8));
  auto* v = new wxBoxSizer(wxVERTICAL);
  auto* intro = new wxStaticText(
      this, wxID_ANY,
      U8("Fichiers de référence utilisés par PersonalDepth (zéro des cartes, niveau moyen, géoïde, marée). "
         "Ceux dont la licence le permet se téléchargent ici en un clic ; les autres demandent un compte "
         "chez leur producteur : le bouton ouvre la page qui explique comment les obtenir."));
  intro->Wrap(FromDIP(640));
  v->Add(intro, 0, wxALL, 8);
  // dossier des références (le même que « Dossier France » de l'import)
  auto* dr = new wxBoxSizer(wxHORIZONTAL);
  dr->Add(new wxStaticText(this, wxID_ANY, U8("Dossier :")), 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 6);
  m_dir = new wxStaticText(this, wxID_ANY, m_pi->ExternalDir(), wxDefaultPosition, wxDefaultSize,
                           wxST_ELLIPSIZE_MIDDLE);
  dr->Add(m_dir, 1, wxALIGN_CENTER_VERTICAL | wxRIGHT, 6);
  auto* chdir = new wxButton(this, wxID_ANY, U8("Changer…"), wxDefaultPosition, wxDefaultSize, wxBU_EXACTFIT);
  auto* opdir = new wxButton(this, wxID_ANY, U8("Ouvrir"), wxDefaultPosition, wxDefaultSize, wxBU_EXACTFIT);
  dr->Add(chdir, 0, wxRIGHT, 2);
  dr->Add(opdir, 0);
  v->Add(dr, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, 8);

  m_list = new wxListCtrl(this, wxID_ANY, wxDefaultPosition, wxSize(FromDIP(660), FromDIP(190)),
                          wxLC_REPORT | wxLC_SINGLE_SEL);
  m_list->AppendColumn(U8("Donnée"), wxLIST_FORMAT_LEFT, FromDIP(240));
  m_list->AppendColumn(U8("Producteur"), wxLIST_FORMAT_LEFT, FromDIP(120));
  m_list->AppendColumn(U8("Taille"), wxLIST_FORMAT_RIGHT, FromDIP(70));
  m_list->AppendColumn(U8("État"), wxLIST_FORMAT_LEFT, FromDIP(200));
  v->Add(m_list, 1, wxEXPAND | wxLEFT | wxRIGHT, 8);

  auto* det = new wxStaticBoxSizer(wxVERTICAL, this, U8("Détails"));
  wxWindow* sb = det->GetStaticBox();
  m_desc = new wxStaticText(sb, wxID_ANY, "");
  m_lic = new wxStaticText(sb, wxID_ANY, "");
  m_lic->SetForegroundColour(wxColour(90, 90, 90));
  m_page = new wxHyperlinkCtrl(sb, wxID_ANY, U8("Page de la donnée"), "https://github.com/margouilla/PersonalDepth_pi");
  m_licLink = new wxHyperlinkCtrl(sb, wxID_ANY, U8("Texte de la licence"), "https://github.com/margouilla/PersonalDepth_pi");
  det->Add(m_desc, 0, wxEXPAND | wxALL, 4);
  det->Add(m_lic, 0, wxEXPAND | wxLEFT | wxRIGHT, 4);
  auto* links = new wxBoxSizer(wxHORIZONTAL);
  links->Add(m_page, 0, wxRIGHT, 16);
  links->Add(m_licLink, 0);
  det->Add(links, 0, wxALL, 4);
  v->Add(det, 0, wxEXPAND | wxALL, 8);

  auto* br = new wxBoxSizer(wxHORIZONTAL);
  m_get = new wxButton(this, wxID_ANY, U8("Télécharger"));
  m_choose = new wxButton(this, wxID_ANY, U8("Choisir le fichier…"));
  m_all = new wxButton(this, wxID_ANY, U8("Tout ce qui manque (France)"));
  m_all->SetToolTip(U8("Télécharge les fichiers libres de France absents du dossier : BathyElli ZH et NM, "
                       "RAF20, RAC23."));
  auto* ver = new wxButton(this, wxID_ANY, U8("Vérifier les fichiers"));
  ver->SetToolTip(U8("Recontrôle les empreintes SHA-256 des fichiers installés : un fichier abîmé ou "
                     "remplacé par une autre version est signalé."));
  auto* upd = new wxButton(this, wxID_ANY, U8("Mettre à jour la liste"));
  upd->SetToolTip(U8("Récupère la liste à jour des données (adresses, nouvelles données) sur le dépôt "
                     "du plugin."));
  br->Add(m_get, 0, wxRIGHT, 4);
  br->Add(m_choose, 0, wxRIGHT, 4);
  br->Add(m_all, 0, wxRIGHT, 12);
  br->AddStretchSpacer();
  br->Add(ver, 0, wxRIGHT, 4);
  br->Add(upd, 0, wxRIGHT, 4);
  br->Add(new wxButton(this, wxID_CLOSE, U8("Fermer")), 0);
  v->Add(br, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, 8);
  SetEscapeId(wxID_CLOSE);
  SetSizerAndFit(v);
  CentreOnParent();

  m_list->Bind(wxEVT_LIST_ITEM_SELECTED, [this](wxListEvent&) { ShowDetails(); });
  m_list->Bind(wxEVT_LIST_ITEM_ACTIVATED, [this](wxListEvent&) {
    const int i = Selected();
    if (i < 0) return;
    const CatalogItem& it = m_cat.items[(size_t)i];
    if (it.downloadable()) {
      bool ab = false;
      if (Install(it, true, ab) && it.use == "france") AfterFranceChange(1);
      Fill();
    } else if (!it.page.empty()) {
      wxLaunchDefaultBrowser(W8(it.page.c_str()));
    }
  });
  m_get->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) {
    const int i = Selected();
    if (i < 0) return;
    const CatalogItem& it = m_cat.items[(size_t)i];
    if (!it.downloadable()) {
      if (!it.page.empty()) wxLaunchDefaultBrowser(W8(it.page.c_str()));
      return;
    }
    bool ab = false;
    if (Install(it, true, ab) && it.use == "france") AfterFranceChange(1);
    Fill();
  });
  m_choose->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) {
    const int i = Selected();
    if (i >= 0) ChooseFile(m_cat.items[(size_t)i]);
  });
  m_all->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { InstallMissing(); });
  upd->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { UpdateCatalog(); });
  ver->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { VerifyFiles(); });
  chdir->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) {
    wxDirDialog dd(this, U8("Dossier des données externes (références)"), m_pi->ExternalDir());
    if (dd.ShowModal() != wxID_OK) return;
    m_pi->Settings().vrefFranceDir = dd.GetPath().ToUTF8().data();
    m_pi->SaveConfig();
    m_dir->SetLabel(m_pi->ExternalDir());
    m_changed = true;
    AfterFranceChange(0);
    Fill();
  });
  opdir->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { wxLaunchDefaultApplication(m_pi->ExternalDir()); });
  Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { EndModal(wxID_CLOSE); }, wxID_CLOSE);
  Fill();
  if (m_list->GetItemCount()) m_list->SetItemState(0, wxLIST_STATE_SELECTED, wxLIST_STATE_SELECTED);
  ShowDetails();
}

int ExternalDataDialog::Selected() const {
  return (int)m_list->GetNextItem(-1, wxLIST_NEXT_ALL, wxLIST_STATE_SELECTED);
}

std::string ExternalDataDialog::Installed(const CatalogItem& it) const {
  const DisplaySettings& s = m_pi->Settings();
  auto exists = [](const std::string& f) { return !f.empty() && wxFileExists(W8(f.c_str())); };
  if (it.use == "tide") return exists(s.tideAtlas) ? s.tideAtlas : std::string();
  if (it.use == "mss") return exists(s.mssGrid) ? s.mssGrid : std::string();
  if (it.use == "national") return catalog::FindFile(s.nationalDir, it.check);
  return catalog::FindFile(s.vrefFranceDir, it.check);
}

void ExternalDataDialog::Fill() {
  const int sel = std::max(0, Selected());
  m_list->DeleteAllItems();
  const std::string lang = LangCode();
  bool missing = false;
  for (size_t i = 0; i < m_cat.items.size(); ++i) {
    const CatalogItem& it = m_cat.items[i];
    const long r = m_list->InsertItem((long)i, W8(it.title(lang).c_str()));
    m_list->SetItem(r, 1, W8(it.producer.c_str()));
    m_list->SetItem(r, 2, SizeText(it.size));
    const bool ok = !Installed(it).empty();
    wxString st;
    if (ok) st = U8("✓ installé");
    else if (it.downloadable()) st = U8("à télécharger");
    else st = U8("compte ou site du producteur");
    m_list->SetItem(r, 3, st);
    if (ok) m_list->SetItemTextColour(r, wxColour(0, 110, 0));
    if (!ok && it.downloadable() && it.use == "france") missing = true;
  }
  m_all->Enable(missing);
  if (sel < m_list->GetItemCount()) m_list->SetItemState(sel, wxLIST_STATE_SELECTED, wxLIST_STATE_SELECTED);
  ShowDetails();
}

void ExternalDataDialog::ShowDetails() {
  const int i = Selected();
  const bool any = i >= 0 && (size_t)i < m_cat.items.size();
  m_get->Enable(any);
  m_choose->Enable(false);
  if (!any) {
    m_desc->SetLabel("");
    m_lic->SetLabel("");
    m_page->Hide();
    m_licLink->Hide();
    return;
  }
  const CatalogItem& it = m_cat.items[(size_t)i];
  wxString d = W8(it.description(LangCode()).c_str());
  const std::string f = Installed(it);
  if (!f.empty()) d += "\n" + U8("Fichier : ") + W8(f.c_str());
  m_desc->SetLabel(d);
  m_desc->Wrap(FromDIP(620));
  wxString l = U8("Licence : ") + W8(it.licence.c_str());
  if (!it.citation.empty()) l += "\n" + U8("Citer : ") + W8(it.citation.c_str());
  m_lic->SetLabel(l);
  m_lic->Wrap(FromDIP(620));
  m_page->Show(!it.page.empty());
  if (!it.page.empty()) m_page->SetURL(W8(it.page.c_str()));
  m_licLink->Show(!it.licenceUrl.empty());
  if (!it.licenceUrl.empty()) m_licLink->SetURL(W8(it.licenceUrl.c_str()));
  if (it.downloadable())
    m_get->SetLabel(f.empty() ? U8("Télécharger") : U8("Télécharger de nouveau"));
  else
    m_get->SetLabel(U8("Ouvrir la page"));
  m_get->Enable(it.downloadable() || !it.page.empty());
  m_choose->Enable(it.use == "tide" || it.use == "mss" || it.use == "national");
  m_choose->SetLabel(it.use == "national" ? U8("Choisir le dossier…") : U8("Choisir le fichier…"));
  // la fenêtre grandit si le texte ne tient plus (jamais ne rétrécit)
  if (GetSizer()) {
    const wxSize best = GetSizer()->GetMinSize() + (GetSize() - GetClientSize()), cur = GetSize();
    if (best.x > cur.x || best.y > cur.y) SetSize(wxSize(std::max(best.x, cur.x), std::max(best.y, cur.y)));
  }
  Layout();
}

bool ExternalDataDialog::Install(const CatalogItem& it, bool askLicence, bool& aborted) {
  aborted = false;
  const std::string lang = LangCode();
  if (askLicence) {
    wxString msg = W8(it.title(lang).c_str()) + " (" + W8(it.producer.c_str()) + ")\n\n" + U8("Licence : ") +
                   W8(it.licence.c_str());
    if (!it.citation.empty()) msg += "\n" + U8("Citer : ") + W8(it.citation.c_str());
    if (it.size > 0) msg += "\n" + U8("Taille : ") + SizeText(it.size);
    msg += "\n\n" + U8("Le fichier est téléchargé depuis le site du producteur :") + "\n" + W8(it.url.c_str());
    wxMessageDialog q(this, msg, U8("Télécharger"), wxYES_NO | wxICON_QUESTION);
    q.SetYesNoLabels(U8("Accepter et télécharger"), U8("Annuler"));
    if (q.ShowModal() != wxID_YES) { aborted = true; return false; }
  }
  const wxString sep = wxFileName::GetPathSeparator();
  wxString dir = m_pi->ExternalDir();
  if (!it.subdir.empty()) dir += sep + W8(it.subdir.c_str());
  const wxString dest = dir + sep + W8(it.file.c_str());
  wxString err;
  if (!m_pi->Download(W8(it.url.c_str()), dest, U8("Données externes"), W8(it.title(lang).c_str()), this, err,
                      &aborted)) {
    if (!aborted) wxMessageBox(err, "PersonalDepth", wxOK | wxICON_WARNING, this);
    return false;
  }
  const std::string d8 = dest.ToStdString(wxConvUTF8);
  // contrôle : taille et empreinte annoncées par le catalogue
  if (!it.sha256.empty()) {
    const std::string h = Sha256::OfFile(d8);
    if (h != it.sha256) {
      wxMessageDialog q(this,
                        U8("Le fichier téléchargé ne correspond pas à celui du catalogue (empreinte SHA-256 "
                           "différente) : le producteur l'a peut-être mis à jour, ou le téléchargement est "
                           "abîmé. « Mettre à jour la liste » récupère les dernières empreintes.\n\nGarder ce "
                           "fichier quand même ?"),
                        "PersonalDepth", wxYES_NO | wxNO_DEFAULT | wxICON_WARNING);
      if (q.ShowModal() != wxID_YES) {
        RemoveFile(d8);
        return false;
      }
    }
  }
  if (it.extract) {
    std::vector<std::string> written;
    std::string e;
    if (!ExtractArchive(d8, dir.ToStdString(wxConvUTF8), nullptr, false, written, e)) {
      wxMessageBox(W8(e.c_str()), "PersonalDepth", wxOK | wxICON_WARNING, this);
      return false;
    }
    RemoveFile(d8);  // archive téléchargée par le plugin : remplacée par son contenu
  }
  m_changed = true;
  return true;
}

void ExternalDataDialog::InstallMissing() {
  int n = 0;
  for (const auto& it : m_cat.items) {
    if (it.use != "france" || !it.downloadable() || !Installed(it).empty()) continue;
    bool ab = false;
    // licences de France : toutes sous Licence Ouverte ; une question par donnée
    if (Install(it, true, ab)) ++n;
    else if (ab) break;
  }
  Fill();
  if (n) AfterFranceChange(n);
}

void ExternalDataDialog::AfterFranceChange(int n) {
  m_pi->DatumChanged();
  if (n > 0 && m_pi->HasDbSources() && !m_pi->DbNeedsMigration()) {
    if (wxMessageBox(U8("Fichiers installés. Recalculer maintenant les surfaces de référence de la base et "
                        "corriger les sources concernées (« Mettre à jour les surfaces ») ?"),
                     "PersonalDepth", wxYES_NO | wxICON_QUESTION, this) == wxYES)
      m_pi->DbUpdateDatum();
  }
}

void ExternalDataDialog::ChooseFile(const CatalogItem& it) {
  DisplaySettings& s = m_pi->Settings();
  if (it.use == "national") {
    wxDirDialog dd(this, U8("Fichiers officiels des pays"), W8(s.nationalDir.c_str()));
    if (dd.ShowModal() != wxID_OK) return;
    s.nationalDir = dd.GetPath().ToUTF8().data();
    m_pi->NationalChanged();
  } else {
    const bool tide = it.use == "tide";
    wxFileDialog fd(this, W8(it.title(LangCode()).c_str()), "", W8((tide ? s.tideAtlas : s.mssGrid).c_str()),
                    tide ? U8("Atlas FES2022 (*.pdtide)|*.pdtide|Tous|*") : U8("Grille PersonalDepth (*.pdgrid)|*.pdgrid|Tous|*"),
                    wxFD_OPEN | wxFD_FILE_MUST_EXIST);
    if (fd.ShowModal() != wxID_OK) return;
    if (tide) {
      s.tideAtlas = fd.GetPath().ToUTF8().data();
      m_pi->TideChanged();
    } else {
      s.mssGrid = fd.GetPath().ToUTF8().data();
    }
    m_pi->DatumChanged();
  }
  m_changed = true;
  Fill();
}

void ExternalDataDialog::VerifyFiles() {
  const std::string lang = LangCode();
  const DisplaySettings& st = m_pi->Settings();
  wxString report;
  int bad = 0, checked = 0;
  wxBusyCursor busy;
  for (const auto& it : m_cat.items) {
    // dossier où la donnée est cherchée (comme pour l'état « installé »)
    std::string dir = st.vrefFranceDir;
    if (it.use == "national") dir = st.nationalDir;
    if (it.use == "tide" || it.use == "mss") {
      const std::string f = it.use == "tide" ? st.tideAtlas : st.mssGrid;
      if (f.empty() || !wxFileExists(W8(f.c_str()))) continue;
      report += wxString::Format("– %s : %s\n", W8(it.title(lang).c_str()),
                                 U8("fichier converti par l'utilisateur, pas d'empreinte de référence"));
      continue;
    }
    if (Installed(it).empty()) continue;  // pas installée : rien à vérifier
    for (const auto& c : catalog::Verify(it, dir)) {
      wxString line;
      const wxString what = W8(it.title(lang).c_str()) + " — " + W8(c.name.c_str());
      switch (c.status) {
        case catalog::FileCheck::Ok: line = "✓ " + what + U8(" : conforme"); ++checked; break;
        case catalog::FileCheck::Different:
          line = "✗ " + what + U8(" : empreinte différente (fichier abîmé ou autre version) — Télécharger de nouveau");
          ++bad;
          break;
        case catalog::FileCheck::Missing: line = "✗ " + what + U8(" : absent"); ++bad; break;
        case catalog::FileCheck::NoFingerprint: line = "– " + what + U8(" : pas d'empreinte de référence"); break;
      }
      report += line + "\n";
    }
  }
  if (report.empty()) report = U8("Aucun fichier installé à vérifier.");
  wxString head = bad ? wxString::Format(U8("%d problème(s) trouvé(s)."), bad)
                      : wxString::Format(U8("%d fichier(s) conforme(s)."), checked);
  wxMessageBox(head + "\n\n" + report, U8("Vérifier les fichiers"), wxOK | (bad ? wxICON_WARNING : wxICON_INFORMATION),
               this);
}

void ExternalDataDialog::UpdateCatalog() {
  const wxString cache = CachePath(m_pi->ExternalDir());
  const wxString tmp = cache + ".new";
  wxString err;
  if (!m_pi->Download(W8(catalog::kRemoteUrl), tmp, U8("Données externes"), U8("Liste des données"), this, err)) {
    wxMessageBox(err, "PersonalDepth", wxOK | wxICON_WARNING, this);
    return;
  }
  std::string s;
  {
    FILE* f = OpenFile(tmp.ToStdString(wxConvUTF8), "rb");
    if (f) {
      char b[8192];
      size_t n;
      while ((n = std::fread(b, 1, sizeof b, f)) > 0) s.append(b, n);
      std::fclose(f);
    }
  }
  Catalog c;
  std::string e;
  if (!catalog::Parse(s, c, e)) {
    wxRemoveFile(tmp);
    wxMessageBox(U8("Liste des données illisible : ") + W8(e.c_str()), "PersonalDepth", wxOK | wxICON_WARNING, this);
    return;
  }
  if (c.version <= m_cat.version) {
    wxRemoveFile(tmp);
    wxMessageBox(wxString::Format(U8("La liste est déjà à jour (version %d du %s)."), m_cat.version,
                                  W8(m_cat.date.c_str())),
                 "PersonalDepth", wxOK | wxICON_INFORMATION, this);
    return;
  }
  if (wxFileExists(cache)) wxRemoveFile(cache);
  wxRenameFile(tmp, cache);
  m_cat = c;
  Fill();
  wxMessageBox(wxString::Format(U8("Liste des données mise à jour (version %d du %s)."), c.version,
                                W8(c.date.c_str())),
               "PersonalDepth", wxOK | wxICON_INFORMATION, this);
}

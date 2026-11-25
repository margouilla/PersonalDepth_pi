// PersonalDepth_pi - données externes : catalogue, licences et téléchargement guidé
// Copyright (C) 2026 Marcaurelio Franzetti
// SPDX-License-Identifier: GPL-2.0-or-later
#include "ExternalDataDialog.h"

#include <wx/dirdlg.h>
#include <wx/filedlg.h>
#include <wx/filename.h>
#include <wx/hyperlink.h>
#include <wx/listctrl.h>
#include <wx/progdlg.h>
#include <wx/radiobox.h>

#include <atomic>
#include <chrono>
#include <filesystem>
#include <mutex>
#include <thread>

#include "PersonalDepth_pi.h"
#include "core/Archive.h"
#include "core/FileUtil.h"
#include "core/Lang.h"
#include "core/NcConvert.h"
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
  m_convert = new wxButton(this, wxID_ANY, U8("Convertir…"));
  m_convert->SetToolTip(U8("Convertit le fichier téléchargé chez le producteur (dossier FES2022b, surface "
                           "moyenne .nc, grille du zéro des cartes d'un pays) au format compact du plugin, puis "
                           "l'utilise. Une seule fois ; une conversion interrompue reprend où elle s'était "
                           "arrêtée."));
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
  br->Add(m_convert, 0, wxRIGHT, 4);
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
  m_convert->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) {
    const int i = Selected();
    if (i >= 0) Convert(m_cat.items[(size_t)i]);
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
  m_convert->Enable(false);
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
  m_convert->Enable(!it.convert.empty());
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

namespace {
namespace fs = std::filesystem;

// Fichier de sortie : `base`, sauf s'il existe déjà (fichier peut-être en
// service) : « base (2) »… ; une conversion interrompue (dossier .parts) est
// reprise sous son nom
std::string OutputName(const fs::path& dir, const std::string& stem, const std::string& ext) {
  std::error_code ec;
  std::vector<fs::path> cand;
  for (int k = 1; k <= 20; ++k)
    cand.push_back(dir / pd::U8Path(k == 1 ? stem + ext : stem + " (" + std::to_string(k) + ")" + ext));
  for (const auto& c : cand)
    if (fs::is_directory(fs::path(c.u8string() + ".parts"), ec)) return c.u8string();
  for (const auto& c : cand)
    if (!fs::exists(c, ec)) return c.u8string();
  return cand.back().u8string();
}

wxString Duration(double s) {
  if (s < 90) return wxString::Format("%.0f s", s);
  return wxString::Format("%.0f min", s / 60.0);
}
}  // namespace

void ExternalDataDialog::Convert(const CatalogItem& it) {
  DisplaySettings& s = m_pi->Settings();
  const bool fes = it.convert == "fes", cd = it.convert == "cd";
  PdgridOptions opt;
  if (cd) opt.var.clear();  // première grille lat / lon du fichier
  std::string in;
  if (fes) {
    wxDirDialog dd(this, U8("Dossier FES2022b téléchargé chez AVISO (il contient ocean_tide_extrapolated)"),
                   m_pi->ExternalDir(), wxDD_DIR_MUST_EXIST);
    if (dd.ShowModal() != wxID_OK) return;
    in = FindFesFolder(dd.GetPath().ToStdString(wxConvUTF8));
    if (in.empty()) {
      wxMessageBox(U8("Aucun fichier m2_fes2022.nc dans ce dossier ni dans son sous-dossier "
                      "ocean_tide_extrapolated. Choisir le dossier fes2022b téléchargé chez AVISO."),
                   "PersonalDepth", wxOK | wxICON_WARNING, this);
      return;
    }
  } else if (!cd) {
    wxFileDialog fd(this, U8("Surface moyenne océanique téléchargée chez AVISO (.nc)"), "", "",
                    U8("netCDF (*.nc)|*.nc|Tous|*"), wxFD_OPEN | wxFD_FILE_MUST_EXIST);
    if (fd.ShowModal() != wxID_OK) return;
    in = fd.GetPath().ToStdString(wxConvUTF8);
  } else {
    wxFileDialog fd(this, U8("Grille du zéro des cartes fournie par le service hydrographique"), "", "",
                    U8("Grilles (*.tif;*.tiff;*.nc;*.gtx;*.asc)|*.tif;*.tiff;*.nc;*.gtx;*.asc|Tous|*"),
                    wxFD_OPEN | wxFD_FILE_MUST_EXIST);
    if (fd.ShowModal() != wxID_OK) return;
    in = fd.GetPath().ToStdString(wxConvUTF8);
    if (!AskGridOptions(pd::U8Path(in).stem().u8string(), opt)) return;
    if (s.nationalDir.empty() || !wxDirExists(W8(s.nationalDir.c_str()))) {
      wxDirDialog dd(this, U8("Dossier « Pays » des fichiers officiels (la grille convertie y sera rangée)"),
                     m_pi->ExternalDir());
      if (dd.ShowModal() != wxID_OK) return;
      s.nationalDir = dd.GetPath().ToUTF8().data();
      m_pi->SaveConfig();
    }
  }
  const fs::path outDir = pd::U8Path(cd ? s.nationalDir : m_pi->ExternalDir().ToStdString(wxConvUTF8));
  std::error_code ec;
  fs::create_directories(outDir, ec);
  const std::string stem = fes ? std::string("FES2022b_ocean") : pd::U8Path(in).stem().u8string();
  const std::string out = OutputName(outDir, stem, fes ? ".pdtide" : ".pdgrid");
  // place : fichier final + parties (≈ 2 × 230 Mo pour FES, 2 × 125 Mo pour la MSS)
  const uint64_t need = (fes ? 520ull : cd ? 64ull : 280ull) << 20;
  const fs::space_info sp = fs::space(outDir, ec);
  if (!ec && sp.available < need) {
    wxMessageBox(wxString::Format(U8("Place insuffisante dans le dossier des données externes : il faut environ "
                                     "%llu Mo libres."),
                                  (unsigned long long)(need >> 20)),
                 "PersonalDepth", wxOK | wxICON_WARNING, this);
    return;
  }
  // conversion en tâche de fond ; la fenêtre de progression reste réactive
  std::atomic<double> frac{0.0};
  std::atomic<bool> cancel{false}, done{false};
  std::mutex mu;
  std::string step, err;
  int rc = -1;
  const auto t0 = std::chrono::steady_clock::now();
  std::thread worker([&] {
    ConvertProgress prog = [&](double f, const std::string& st) {
      frac = f;
      {
        std::lock_guard<std::mutex> lk(mu);
        step = st;
      }
      return !cancel.load();
    };
    try {
      if (fes) rc = ConvertFesToPdtide(in, out, prog, err);
      else rc = ConvertToPdgrid(in, out, opt, prog, err);
    } catch (const std::exception& e) {
      rc = -1;
      err = e.what();
    }
    done = true;
  });
  {
    const wxString title = W8(it.title(LangCode()).c_str());
    wxProgressDialog dlg(U8("Conversion"), title + "\n" + W8(out.c_str()), 1000, this,
                        wxPD_APP_MODAL | wxPD_CAN_ABORT | wxPD_ELAPSED_TIME | wxPD_REMAINING_TIME);
    while (!done) {
      wxMilliSleep(150);
      wxString st;
      {
        std::lock_guard<std::mutex> lk(mu);
        st = W8(step.c_str());
      }
      const int v = std::min(999, (int)(frac.load() * 1000));
      wxString msg = title;
      if (!st.empty()) msg += W8(" — ") + (fes ? U8("onde ") + st : st);
      if (!dlg.Update(v, msg)) cancel = true;
    }
  }
  worker.join();
  const double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
  if (rc == 0) {
    wxMessageBox(U8("Conversion interrompue. Relancer « Convertir… » avec le même fichier ou dossier : elle "
                    "reprendra où elle s'était arrêtée."),
                 "PersonalDepth", wxOK | wxICON_INFORMATION, this);
    return;
  }
  if (rc < 0) {
    wxMessageBox(U8("Conversion impossible :") + "\n" + W8(err.c_str()), "PersonalDepth", wxOK | wxICON_WARNING,
                 this);
    return;
  }
  if (fes) {
    s.tideAtlas = out;
    m_pi->TideChanged();
  } else if (!cd) {
    s.mssGrid = out;
  }
  m_changed = true;
  Fill();
  wxMessageBox(wxString::Format(U8("Conversion terminée en %s. Le plugin utilise maintenant :"), Duration(secs)) +
                   "\n" + W8(out.c_str()),
               "PersonalDepth", wxOK | wxICON_INFORMATION, this);
  if (cd) m_pi->NationalChanged();  // relit le dossier « Pays », met à jour les surfaces de la base
  else AfterFranceChange(1);        // marée (LAT) ou MSS : proposer la mise à jour des surfaces de la base
}

// Grille de zéro des cartes : nom affiché et grandeur des valeurs
bool ExternalDataDialog::AskGridOptions(const std::string& stem, PdgridOptions& opt) {
  wxDialog d(this, wxID_ANY, U8("Grille du zéro des cartes"));
  auto* v = new wxBoxSizer(wxVERTICAL);
  auto* g = new wxFlexGridSizer(2, 6, 8);
  g->AddGrowableCol(1);
  g->Add(new wxStaticText(&d, wxID_ANY, U8("Nom affiché :")), 0, wxALIGN_CENTER_VERTICAL);
  auto* name = new wxTextCtrl(&d, wxID_ANY, W8(stem.c_str()), wxDefaultPosition, wxSize(FromDIP(260), -1));
  name->SetToolTip(U8("Nom donné dans le cadre des surfaces de référence et sous le curseur (ex. « VORF CD »)."));
  g->Add(name, 1, wxEXPAND);
  v->Add(g, 0, wxEXPAND | wxALL, 10);
  const wxString choices[] = {
      U8("zéro des cartes par rapport au niveau moyen (valeurs négatives, quelques mètres)"),
      U8("hauteur du zéro des cartes au-dessus de l'ellipsoïde (dizaines de mètres)")};
  auto* q = new wxRadioBox(&d, wxID_ANY, U8("Les valeurs de la grille donnent"), wxDefaultPosition, wxDefaultSize, 2,
                           choices, 1, wxRA_SPECIFY_COLS);
  v->Add(q, 0, wxEXPAND | wxLEFT | wxRIGHT, 10);
  auto* neg = new wxCheckBox(&d, wxID_ANY, U8("Inverser le signe (la grille donne le niveau moyen au-dessus du zéro "
                                              "des cartes, en valeurs positives)"));
  v->Add(neg, 0, wxALL, 10);
  auto* hint = new wxStaticText(&d, wxID_ANY,
                                U8("GeoTIFF, netCDF, .gtx ou .asc en longitude / latitude (WGS84 ou ETRS89). "
                                   "Une grille en projection se convertit d'abord en longitude / latitude "
                                   "(QGIS, GDAL)."));
  hint->Wrap(FromDIP(440));
  hint->SetForegroundColour(wxColour(90, 90, 90));
  v->Add(hint, 0, wxLEFT | wxRIGHT | wxBOTTOM, 10);
  v->Add(d.CreateSeparatedButtonSizer(wxOK | wxCANCEL), 0, wxEXPAND | wxALL, 8);
  d.SetSizerAndFit(v);
  d.CentreOnParent();
  if (d.ShowModal() != wxID_OK) return false;
  opt.name = name->GetValue().ToStdString(wxConvUTF8);
  opt.quantity = q->GetSelection() == 0 ? "CD/MSL" : "CD/WGS84";
  opt.negate = neg->GetValue();
  return true;
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
      report += W8("– ") + wxString::Format("%s : %s\n", W8(it.title(lang).c_str()),
                                 U8("fichier converti, pas d'empreinte de référence"));
      continue;
    }
    if (Installed(it).empty()) continue;  // pas installée : rien à vérifier
    for (const auto& c : catalog::Verify(it, dir)) {
      wxString line;
      const wxString what = W8(it.title(lang).c_str()) + W8(" — ") + W8(c.name.c_str());
      switch (c.status) {
        case catalog::FileCheck::Ok: line = W8("✓ ") + what + U8(" : conforme"); ++checked; break;
        case catalog::FileCheck::Different:
          line = W8("✗ ") + what + U8(" : empreinte différente (fichier abîmé ou autre version) — Télécharger de nouveau");
          ++bad;
          break;
        case catalog::FileCheck::Missing: line = W8("✗ ") + what + U8(" : absent"); ++bad; break;
        case catalog::FileCheck::NoFingerprint: line = W8("– ") + what + U8(" : pas d'empreinte de référence"); break;
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

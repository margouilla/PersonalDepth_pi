// PersonalDepth_pi - choix d'un marégraphe en ligne (REFMAR…)
// Copyright (C) 2026 Marcaurelio Franzetti
// SPDX-License-Identifier: GPL-2.0-or-later
#include "GaugeDialog.h"

#include <wx/datectrl.h>
#include <wx/dateevt.h>
#include <wx/filename.h>
#include <wx/listctrl.h>

#include <algorithm>
#include <cmath>
#include <ctime>

#include "GaugeManager.h"
#include "PersonalDepth_pi.h"
#include "core/FileUtil.h"
#include "core/Lang.h"
#include "core/PointCloud.h"

#define U8(s) wxString::FromUTF8(pd::tr(s))
#define W8(s) wxString::FromUTF8(s)  // données : pas de traduction

using namespace pd;

namespace {
std::string ReadText(const wxString& path) {
  std::string s;
  FILE* f = OpenFile(path.ToStdString(wxConvUTF8), "rb");
  if (!f) return s;
  char b[16384];
  size_t n;
  while ((n = std::fread(b, 1, sizeof b, f)) > 0) s.append(b, n);
  std::fclose(f);
  return s;
}

void WriteText(const wxString& path, const std::string& s) {
  FILE* f = OpenFile(path.ToStdString(wxConvUTF8), "wb");
  if (!f) return;
  std::fwrite(s.data(), 1, s.size(), f);
  std::fclose(f);
}

long long Utc(const wxDateTime& d) {  // date choisie (jour civil) -> minuit UTC
  long long t = 0;
  ParseDateTime(d.Format("%Y-%m-%d").ToStdString(), "", 1, t);
  return t;
}
}  // namespace

GaugeDialog::GaugeDialog(wxWindow* parent, PersonalDepth_pi* pi, Mode mode, const std::string& providerId,
                         const std::string& selectedId)
    : wxDialog(parent, wxID_ANY, U8("Marégraphes en ligne"), wxDefaultPosition, wxDefaultSize,
               wxDEFAULT_DIALOG_STYLE | wxRESIZE_BORDER),
      m_pi(pi),
      m_mode(mode),
      m_selectedId(selectedId) {
  m_prov = FindGaugeProvider(providerId);
  if (!m_prov) m_prov = GaugeProviders().front();
  m_hasCenter = m_pi->ViewCenter(m_lat, m_lon);
  auto* v = new wxBoxSizer(wxVERTICAL);
  auto* intro = new wxStaticText(
      this, wxID_ANY,
      mode == Download
          ? U8("Hauteurs d'eau mesurées par les marégraphes, au zéro des cartes, en UTC. Choisir le marégraphe le plus "
               "proche du levé et la période des sondes : les fichiers sont téléchargés une fois, puis lus "
               "directement par l'import.")
          : U8("Marégraphe dont la hauteur d'eau mesurée est ajoutée aux profondeurs (onglet Marée). Choisir le plus "
               "proche de la zone affichée."));
  intro->Wrap(FromDIP(560));
  v->Add(intro, 0, wxALL, 8);
  // réseau (un seul pour l'instant : REFMAR, France)
  auto* pr = new wxBoxSizer(wxHORIZONTAL);
  pr->Add(new wxStaticText(this, wxID_ANY, U8("Réseau")), 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 6);
  m_provChoice = new wxChoice(this, wxID_ANY);
  for (const auto* p : GaugeProviders())
    m_provChoice->Append(W8((p->title() + " — " + p->region()).c_str()));
  for (size_t i = 0; i < GaugeProviders().size(); ++i)
    if (GaugeProviders()[i] == m_prov) m_provChoice->SetSelection((int)i);
  m_provChoice->Enable(GaugeProviders().size() > 1);
  pr->Add(m_provChoice, 1, wxRIGHT, 12);
  pr->Add(new wxStaticText(this, wxID_ANY, U8("Rechercher")), 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 6);
  m_search = new wxTextCtrl(this, wxID_ANY);
  pr->Add(m_search, 1);
  v->Add(pr, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, 8);
  m_list = new wxListCtrl(this, wxID_ANY, wxDefaultPosition, wxSize(FromDIP(560), FromDIP(240)),
                          wxLC_REPORT | wxLC_SINGLE_SEL);
  m_list->AppendColumn(U8("Marégraphe"), wxLIST_FORMAT_LEFT, FromDIP(220));
  m_list->AppendColumn(U8("Distance"), wxLIST_FORMAT_RIGHT, FromDIP(80));
  m_list->AppendColumn(U8("Réseau"), wxLIST_FORMAT_LEFT, FromDIP(110));
  m_list->AppendColumn(U8("État"), wxLIST_FORMAT_LEFT, FromDIP(80));
  v->Add(m_list, 1, wxEXPAND | wxLEFT | wxRIGHT, 8);

  if (mode == Download) {
    auto* dr = new wxBoxSizer(wxHORIZONTAL);
    const wxDateTime today = wxDateTime::Today();
    m_from = new wxDatePickerCtrl(this, wxID_ANY, today - wxDateSpan::Days(30));
    m_to = new wxDatePickerCtrl(this, wxID_ANY, today);
    m_to->SetRange(wxDefaultDateTime, today);
    dr->Add(new wxStaticText(this, wxID_ANY, U8("Période du")), 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 6);
    dr->Add(m_from, 0, wxRIGHT, 6);
    dr->Add(new wxStaticText(this, wxID_ANY, U8("au")), 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 6);
    dr->Add(m_to, 0, wxRIGHT, 6);
    dr->Add(new wxStaticText(this, wxID_ANY, U8("(inclus, UTC)")), 0, wxALIGN_CENTER_VERTICAL);
    v->Add(dr, 0, wxALL, 8);
  }
  m_lic = new wxStaticText(this, wxID_ANY, "");
  m_lic->SetForegroundColour(wxColour(90, 90, 90));
  v->Add(m_lic, 0, wxLEFT | wxRIGHT | wxTOP, 8);

  auto* br = new wxBoxSizer(wxHORIZONTAL);
  auto* refresh = new wxButton(this, wxID_ANY, U8("Actualiser la liste"));
  br->Add(refresh, 0);
  br->AddStretchSpacer();
  auto* ok = new wxButton(this, wxID_OK, mode == Download ? U8("Télécharger") : U8("Choisir"));
  br->Add(ok, 0, wxRIGHT, 4);
  br->Add(new wxButton(this, wxID_CANCEL, U8("Annuler")), 0);
  v->Add(br, 0, wxEXPAND | wxALL, 8);

  auto showLicence = [this]() {
    wxString l = wxString::Format(U8("Données %s, %s : citer « %s » si vous diffusez un résultat. Mesures brutes "
                                     "récentes non validées."),
                                  W8(m_prov->title().c_str()), W8(m_prov->licence().c_str()),
                                  W8(m_prov->citation().c_str()));
    m_lic->SetLabel(l);
    m_lic->Wrap(FromDIP(560));
  };
  showLicence();
  SetSizerAndFit(v);
  CentreOnParent();

  m_search->Bind(wxEVT_TEXT, [this](wxCommandEvent&) { Fill(); });
  m_provChoice->Bind(wxEVT_CHOICE, [this, showLicence](wxCommandEvent&) {
    const int i = m_provChoice->GetSelection();
    if (i >= 0 && (size_t)i < GaugeProviders().size()) m_prov = GaugeProviders()[(size_t)i];
    showLicence();
    if (LoadStations(false)) Fill();
  });
  refresh->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) {
    if (LoadStations(true)) Fill();
  });
  ok->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { OnOk(); });
  m_list->Bind(wxEVT_LIST_ITEM_ACTIVATED, [this](wxListEvent&) { OnOk(); });
  CallAfter([this] {
    if (LoadStations(false)) Fill();
  });
}

bool GaugeDialog::LoadStations(bool refresh) {
  wxString e;
  if (m_pi->Gauges().LoadStationsNow(*m_prov, refresh, this, e)) {
    m_all = *m_pi->Gauges().Stations(*m_prov, false);
    return true;
  }
  m_all.clear();
  wxMessageBox(e.empty() ? U8("Liste des marégraphes indisponible") : e, "PersonalDepth", wxOK | wxICON_WARNING, this);
  return false;
}

void GaugeDialog::Fill() {
  const wxString q = m_search->GetValue().Lower().Trim().Trim(false);
  std::vector<std::pair<double, size_t>> rows;
  for (size_t i = 0; i < m_all.size(); ++i) {
    const auto& s = m_all[i];
    if (!q.empty() && !W8(s.label().c_str()).Lower().Contains(q) && !W8(s.name.c_str()).Lower().Contains(q))
      continue;
    const double d = m_hasCenter ? gauge::DistanceKm(m_lon, m_lat, s.lon, s.lat) : 0;
    rows.emplace_back(m_hasCenter ? d : (double)i, i);
  }
  std::sort(rows.begin(), rows.end());
  m_list->DeleteAllItems();
  m_shown.clear();
  long sel = 0;
  for (const auto& r : rows) {
    const auto& s = m_all[r.second];
    const long k = m_list->InsertItem((long)m_shown.size(), W8(s.label().c_str()));
    m_list->SetItem(k, 1, m_hasCenter ? wxString::Format("%.0f km", r.first) : wxString());
    m_list->SetItem(k, 2, W8(s.network.c_str()));
    m_list->SetItem(k, 3, W8(s.state.c_str()));
    if (!m_selectedId.empty() && s.id == m_selectedId) sel = k;
    m_shown.push_back(r.second);
  }
  if (!m_shown.empty()) {
    m_list->SetItemState(sel, wxLIST_STATE_SELECTED, wxLIST_STATE_SELECTED);
    m_list->EnsureVisible(sel);
  }
}

int GaugeDialog::SelectedIndex() const {
  const long sel = m_list->GetNextItem(-1, wxLIST_NEXT_ALL, wxLIST_STATE_SELECTED);
  if (sel < 0 || (size_t)sel >= m_shown.size()) return -1;
  return (int)m_shown[(size_t)sel];
}

void GaugeDialog::OnOk() {
  const int i = SelectedIndex();
  if (i < 0) {
    wxMessageBox(U8("Choisir un marégraphe dans la liste."), "PersonalDepth", wxOK | wxICON_INFORMATION, this);
    return;
  }
  m_station = m_all[(size_t)i];
  if (m_mode == Pick) {
    EndModal(wxID_OK);
    return;
  }
  const GaugeStation& st = m_station;
  const long long t0 = Utc(m_from->GetValue()), t1 = Utc(m_to->GetValue()) + 86400;  // dernier jour inclus
  if (t1 <= t0) {
    wxMessageBox(U8("La fin de la période doit suivre son début."), "PersonalDepth", wxOK | wxICON_WARNING, this);
    return;
  }
  const auto chunks = m_prov->chunks(st.id, t0, t1, m_prov->maxChunkDays());
  if (chunks.size() > 60 &&
      wxMessageBox(wxString::Format(U8("Période longue : %d téléchargements d'un mois. Continuer ?"), (int)chunks.size()),
                   "PersonalDepth", wxYES_NO | wxICON_QUESTION, this) != wxYES)
    return;
  const wxString folder = m_pi->Gauges().StationDir(*m_prov, st), sep = wxFileName::GetPathSeparator();
  const long long now = (long long)std::time(nullptr);
  int got = 0, kept = 0;
  for (size_t k = 0; k < chunks.size(); ++k) {
    const auto& c = chunks[k];
    const wxString f = folder + sep + W8(c.file.c_str());
    // tranche déjà là et terminée depuis plus de 2 jours : gardée (les
    // données récentes, brutes, sont reprises)
    if (wxFileExists(f) && c.t1 + 2 * 86400 < now && m_prov->hasData(ReadText(f))) { ++kept; continue; }
    wxString err;
    bool aborted = false;
    const wxString msg = W8(st.label().c_str()) + wxString::Format(" : %d / %d", (int)k + 1, (int)chunks.size());
    if (!m_pi->Download(W8(c.url.c_str()), f, W8(m_prov->title().c_str()), msg, this, err, &aborted)) {
      if (!aborted) wxMessageBox(err, "PersonalDepth", wxOK | wxICON_WARNING, this);
      return;
    }
    const std::string text = ReadText(f);
    if (m_prov->hasData(text)) {
      ++got;
      const std::string stored = m_prov->store(text);  // entête du fournisseur au besoin
      if (stored != text) WriteText(f, stored);
    } else {
      RemoveFile(f.ToStdString(wxConvUTF8));  // pas de mesure sur la tranche
    }
  }
  if (got + kept == 0) {
    wxMessageBox(U8("Aucune mesure de ce marégraphe sur la période choisie."), "PersonalDepth",
                 wxOK | wxICON_INFORMATION, this);
    return;
  }
  m_folder = folder;
  EndModal(wxID_OK);
}

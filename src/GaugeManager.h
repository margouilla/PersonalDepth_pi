// PersonalDepth_pi - marégraphes en ligne : listes des stations et mesures
// gardées dans le dossier des références, téléchargées en tâche de fond
// (onglet Marée) ou avec la fenêtre de progression (import)
// Copyright (C) 2026 Marcaurelio Franzetti
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <wx/wx.h>

#include <deque>
#include <functional>
#include <map>
#include <string>
#include <vector>

#include "core/TideGauge.h"

class PersonalDepth_pi;
class OCPN_downloadEvent;

class GaugeManager : public wxEvtHandler {
 public:
  explicit GaugeManager(PersonalDepth_pi* pi);
  ~GaugeManager() override;
  void Stop();  // téléchargement en cours abandonné (arrêt du plugin)

  // Dossiers : <références>/<Fournisseur>[/<station>]
  wxString ProviderDir(const pd::GaugeProvider& p) const;
  wxString StationDir(const pd::GaugeProvider& p, const pd::GaugeStation& s) const;

  // Liste des stations : lue dans le dossier (gardée 30 jours), sinon null ;
  // background : téléchargement en tâche de fond si absente ou périmée
  const std::vector<pd::GaugeStation>* Stations(const pd::GaugeProvider& p, bool background);
  // Téléchargement immédiat (fenêtre de progression) ; false et err en cas d'échec
  bool LoadStationsNow(const pd::GaugeProvider& p, bool refresh, wxWindow* parent, wxString& err);

  // Mesures de [t0, t1) par tranches d'un jour : tranches absentes ou
  // récentes périmées (refreshSec) mises en file. true si rien ne manque.
  bool Ensure(const pd::GaugeProvider& p, const pd::GaugeStation& s, long long t0, long long t1, int refreshSec);
  bool Busy() const { return m_busy || !m_queue.empty(); }
  int Version() const { return m_version; }  // change à chaque fichier reçu
  // Appelé (fil principal) après chaque téléchargement terminé
  void SetOnUpdate(std::function<void()> f) { m_onUpdate = std::move(f); }

 private:
  struct Job {
    wxString url, dest;
    const pd::GaugeProvider* provider = nullptr;
    bool stations = false;  // liste des stations
  };
  void Enqueue(const Job& j);
  void Next();
  void OnDownload(OCPN_downloadEvent& ev);
  bool Fresh(const wxString& file, int maxAgeSec) const;

  PersonalDepth_pi* m_pi;
  std::map<std::string, std::vector<pd::GaugeStation>> m_stations;  // par fournisseur
  std::map<std::string, long long> m_stationsTried;                 // dernier essai (s)
  std::deque<Job> m_queue;
  Job m_cur;
  bool m_busy = false;
  long m_handle = 0;
  int m_version = 0;
  std::map<std::string, long long> m_emptyUntil;  // tranche sans mesure : pas de nouvel essai avant
  std::function<void()> m_onUpdate;
};

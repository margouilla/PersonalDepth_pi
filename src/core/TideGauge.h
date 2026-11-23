// PersonalDepth_pi - marégraphes : hauteurs d'eau observées en ligne
//
// Un fournisseur (réseau de marégraphes) donne la liste de ses stations et
// les mesures d'une période ; le reste du plugin (import des sondes, source
// de marée « marégraphe ») ne connaît que cette interface. REFMAR (Shom,
// France) est le premier ; d'autres réseaux (NOAA CO-OPS, NTSLF, IOC…)
// s'ajoutent par une nouvelle classe enregistrée dans GaugeProviders().
//
// Mesures gardées telles que reçues, un fichier par tranche :
//   <dossier des références>/<Fournisseur>/<id>_<NOM>/<id>_<AAAA-MM-JJ>_<N>d.txt
// (N jours à partir de la date, UTC). Les mêmes fichiers servent à l'import
// et à l'onglet Marée.
//
// Copyright (C) 2026 Marcaurelio Franzetti
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <string>
#include <vector>

namespace pd {

struct GaugeStation {
  std::string provider;  // identifiant du fournisseur (« refmar »)
  std::string id;        // identifiant de la station chez le fournisseur
  std::string name;      // nom brut (« LE_CONQUET »)
  std::string network;   // réseau (RONIM, PARTENAIRES…)
  std::string state;     // état annoncé (OK…)
  double lon = 0, lat = 0;
  std::string label() const;   // nom lisible (« Le Conquet »)
  std::string folder() const;  // sous-dossier : « <id>_<NOM> » (caractères sûrs)
};

struct GaugeChunk {
  long long t0 = 0, t1 = 0;  // [t0, t1) secondes UTC, minuit
  std::string url;
  std::string file;  // <id>_<AAAA-MM-JJ>_<N>d.txt
};

class GaugeProvider {
 public:
  virtual ~GaugeProvider() = default;
  virtual std::string id() const = 0;        // « refmar » (réglages, dossiers)
  virtual std::string title() const = 0;     // « REFMAR (Shom) »
  virtual std::string dirName() const = 0;   // « REFMAR »
  virtual std::string region() const = 0;    // « France »
  virtual std::string datum() const = 0;     // référence des hauteurs (« ZH »)
  virtual std::string licence() const = 0;
  virtual std::string citation() const = 0;
  virtual std::string stationsUrl() const = 0;
  virtual int maxChunkDays() const = 0;      // tranche la plus longue d'une requête
  virtual bool parseStations(const std::string& text, std::vector<GaugeStation>& out, std::string& err) const = 0;
  virtual std::string dataUrl(const std::string& id, long long t0, long long t1) const = 0;
  // Fichier de ce fournisseur (début du texte) : lu tel quel par l'import
  virtual bool recognises(const std::string& head) const = 0;
  // Réponse ou fichier utile (au moins une mesure)
  virtual bool hasData(const std::string& text) const = 0;
  // Texte enregistré pour une réponse utile (par défaut la réponse telle
  // quelle ; un fournisseur dont la réponse ne dit pas sa référence y ajoute
  // un entête, pour que recognises() ne prenne que ses propres fichiers)
  virtual std::string store(const std::string& response) const { return response; }
  // Mesures : instants (s UTC) et hauteurs (m, référence datum())
  virtual bool parse(const std::string& text, std::vector<long long>& t, std::vector<double>& h, int& skipped,
                     std::string& err) const = 0;

  // Tranches de maxDays jours au plus (au plus maxChunkDays()) couvrant
  // [t0, t1), alignées sur minuit UTC
  std::vector<GaugeChunk> chunks(const std::string& stationId, long long t0, long long t1, int maxDays) const;
};

const std::vector<const GaugeProvider*>& GaugeProviders();
const GaugeProvider* FindGaugeProvider(const std::string& id);

namespace gauge {
// Nom d'une tranche, et sa lecture (false si le nom n'est pas une tranche)
std::string ChunkFile(const std::string& stationId, long long t0, int days);
bool ParseChunkFile(const std::string& fileName, long long& t0, int& days);
// Distance (km) entre deux points
double DistanceKm(double lon1, double lat1, double lon2, double lat2);
// Station la plus proche ; -1 si la liste est vide
int Nearest(const std::vector<GaugeStation>& st, double lon, double lat, double* km = nullptr);
}  // namespace gauge

// Série de mesures d'une station : fusion des tranches lues dans son dossier
class GaugeSeries {
 public:
  // Tranches du dossier qui touchent [t0, t1) ; false si aucune mesure
  bool load(const GaugeProvider& p, const std::string& folder, long long t0, long long t1);
  // Hauteur à l'instant t : interpolée entre deux mesures écartées de maxGap s
  // au plus ; avant la première / après la dernière, gardée maxHold s au
  // plus. tObs : instant de la mesure la plus proche.
  bool at(long long t, long long maxGap, long long maxHold, double& h, long long* tObs = nullptr) const;
  size_t size() const { return t_.size(); }
  long long tMin() const { return t_.empty() ? 0 : t_.front(); }
  long long tMax() const { return t_.empty() ? 0 : t_.back(); }
  const std::vector<long long>& times() const { return t_; }
  const std::vector<double>& heights() const { return h_; }
  void set(std::vector<long long> t, std::vector<double> h);  // triées, doublons moyennés

 private:
  std::vector<long long> t_;
  std::vector<double> h_;
};

}  // namespace pd

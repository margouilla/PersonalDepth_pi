// PersonalDepth_pi - hauteurs d'eau observées des marégraphes REFMAR (Shom)
// Copyright (C) 2026 Marcaurelio Franzetti
// SPDX-License-Identifier: GPL-2.0-or-later
#include "Refmar.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <map>

#include "Json.h"
#include "Lang.h"
#include "PointCloud.h"
#include "WaterLevel.h"

namespace pd {

namespace refmar {

const char* kStationsUrl = "https://services.data.shom.fr/maregraphie/service/tidegauges";
const char* kCitation = "REFMAR, Shom - https://dx.doi.org/10.17183/REFMAR";
const char* kLicence = "Licence Ouverte / Open Licence 2.0 (Etalab)";

bool ParseStations(const std::string& json, std::vector<GaugeStation>& out, std::string& err) {
  out.clear();
  Json j;
  if (!Json::Parse(json, j, err)) return false;
  if (!j.isArray()) { err = tr("Liste des marégraphes REFMAR illisible"); return false; }
  for (const auto& s : j.items()) {
    GaugeStation st;
    st.provider = "refmar";
    st.id = s["shom_id"].str();
    st.name = s["name"].str();
    st.network = s["reseau"].str();
    st.state = s["state"].str();
    st.lon = s["longitude"].num(NAN);
    st.lat = s["latitude"].num(NAN);
    if (st.id.empty() || st.name.empty() || !std::isfinite(st.lon) || !std::isfinite(st.lat)) continue;
    if (st.id.find_first_not_of("0123456789") != std::string::npos) continue;  // sert dans une adresse
    out.push_back(st);
  }
  if (out.empty()) { err = tr("Liste des marégraphes REFMAR vide"); return false; }
  std::sort(out.begin(), out.end(), [](const GaugeStation& a, const GaugeStation& b) { return a.name < b.name; });
  return true;
}

std::string DataUrl(const std::string& id, long long t0, long long t1) {
  const std::string d0 = FormatTime(t0).substr(0, 10), d1 = FormatTime(t1 - 86400).substr(0, 10);
  return "https://services.data.shom.fr/maregraphie/observation/txt/" + id + "?sources=1,2,3,4&dtStart=" + d0 +
         "T00:00:00Z&dtEnd=" + d1 + "T23:59:59Z";
}

bool IsRefmarText(const std::string& head) {
  return head.find("# Date;Valeur;Source") != std::string::npos ||
         (head.compare(0, 9, "# Station") == 0 && head.find("zero_hydrographique") != std::string::npos);
}

std::string HeaderValue(const std::string& text, const std::string& key) {
  size_t p = 0;
  while (p < text.size()) {
    size_t e = text.find('\n', p);
    if (e == std::string::npos) e = text.size();
    const std::string l = text.substr(p, e - p);
    if (l.empty() || l[0] != '#') break;
    const std::string pre = "# " + key;
    if (l.compare(0, pre.size(), pre) == 0) {
      size_t c = l.find(':', pre.size());
      if (c != std::string::npos) {
        std::string v = l.substr(c + 1);
        while (!v.empty() && (v.back() == '\r' || v.back() == ' ')) v.pop_back();
        while (!v.empty() && v.front() == ' ') v.erase(v.begin());
        return v;
      }
    }
    p = e + 1;
  }
  return std::string();
}

bool HasData(const std::string& text) {
  if (text.compare(0, 9, "# Station") != 0) return false;
  size_t p = 0;
  while ((p = text.find('\n', p)) != std::string::npos) {
    ++p;
    if (p < text.size() && text[p] != '#' && text[p] != '\r' && text[p] != '\n') return true;
  }
  return false;
}

bool Parse(const std::string& text, std::vector<long long>& T, std::vector<double>& H, int& skipped,
           std::string& err) {
  T.clear(); H.clear();
  skipped = 0;
  // rang de chaque source (plus petit = meilleur), 0 = écartée
  auto rank = [](int s) {
    switch (s) {
      case 3: return 1;
      case 2: return 2;
      case 1: return 3;
      case 4: return 4;
      case 5: return 5;
      default: return 0;
    }
  };
  struct Obs { long long t; double h; int r; };
  std::vector<Obs> obs;
  std::map<long long, int> best;  // jour -> meilleur rang
  size_t p = 0;
  while (p < text.size()) {
    size_t e = text.find('\n', p);
    if (e == std::string::npos) e = text.size();
    std::string l = text.substr(p, e - p);
    p = e + 1;
    if (!l.empty() && l.back() == '\r') l.pop_back();
    if (l.empty() || l[0] == '#') continue;
    const size_t a = l.find(';'), b = a == std::string::npos ? a : l.find(';', a + 1);
    long long t;
    double h, src = 0;
    if (a == std::string::npos) { ++skipped; continue; }
    const std::string when = l.substr(0, a);
    const size_t sp = when.find(' ');
    const std::string val = l.substr(a + 1, b == std::string::npos ? std::string::npos : b - a - 1);
    if (!ParseDateTime(when.substr(0, sp), sp == std::string::npos ? std::string() : when.substr(sp + 1), 2, t) ||
        !ParseFieldNumber(val, '.', h)) {
      ++skipped;
      continue;
    }
    if (b != std::string::npos) ParseFieldNumber(l.substr(b + 1), '.', src);
    const int r = b == std::string::npos ? 1 : rank((int)src);
    if (!r) continue;  // pleines et basses mers, source inconnue
    obs.push_back({t, h, r});
    const long long day = t >= 0 ? t / 86400 : -((-t + 86399) / 86400);
    auto it = best.find(day);
    if (it == best.end() || r < it->second) best[day] = r;
  }
  std::map<long long, double> keep;
  for (const auto& o : obs) {
    const long long day = o.t >= 0 ? o.t / 86400 : -((-o.t + 86399) / 86400);
    if (o.r == best[day]) keep[o.t] = o.h;
  }
  for (const auto& kv : keep) { T.push_back(kv.first); H.push_back(kv.second); }
  if (T.empty()) { err = tr("Aucune hauteur d'eau dans le fichier REFMAR"); return false; }
  return true;
}

namespace {
class RefmarProvider : public GaugeProvider {
 public:
  std::string id() const override { return "refmar"; }
  std::string title() const override { return "REFMAR (Shom)"; }
  std::string dirName() const override { return "REFMAR"; }
  std::string region() const override { return "France"; }
  std::string datum() const override { return "ZH"; }
  std::string licence() const override { return kLicence; }
  std::string citation() const override { return kCitation; }
  std::string stationsUrl() const override { return kStationsUrl; }
  int maxChunkDays() const override { return 31; }
  bool parseStations(const std::string& text, std::vector<GaugeStation>& out, std::string& err) const override {
    return ParseStations(text, out, err);
  }
  std::string dataUrl(const std::string& id, long long t0, long long t1) const override { return DataUrl(id, t0, t1); }
  bool recognises(const std::string& head) const override { return IsRefmarText(head); }
  bool hasData(const std::string& text) const override { return HasData(text); }
  bool parse(const std::string& text, std::vector<long long>& t, std::vector<double>& h, int& skipped,
             std::string& err) const override {
    return Parse(text, t, h, skipped, err);
  }
};
}  // namespace

const GaugeProvider& Provider() {
  static const RefmarProvider p;
  return p;
}

}  // namespace refmar
}  // namespace pd

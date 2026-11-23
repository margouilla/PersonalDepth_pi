// PersonalDepth_pi - marégraphes NOAA CO-OPS (États-Unis)
// Copyright (C) 2026 Marcaurelio Franzetti
// SPDX-License-Identifier: GPL-2.0-or-later
#include "NoaaCoops.h"

#include <algorithm>
#include <cmath>
#include <cstring>

#include "Json.h"
#include "Lang.h"
#include "PointCloud.h"
#include "WaterLevel.h"

namespace pd {
namespace noaa {

const char* kStationsUrl = "https://api.tidesandcurrents.noaa.gov/mdapi/prod/webapi/stations.json?type=waterlevels";
static const char* kMarker = "# NOAA CO-OPS water_level datum=MLLW units=metric time_zone=GMT";

bool ParseStations(const std::string& json, std::vector<GaugeStation>& out, std::string& err) {
  out.clear();
  Json j;
  if (!Json::Parse(json, j, err)) return false;
  const Json& list = j["stations"];
  if (!list.isArray()) { err = tr("Liste des marégraphes NOAA illisible"); return false; }
  for (const auto& s : list.items()) {
    if (!s["tidal"].boolean(true) || s["greatlakes"].boolean(false)) continue;  // pas de MLLW
    GaugeStation st;
    st.provider = "noaa";
    st.id = s["id"].str();
    st.name = s["name"].str();
    st.network = s["state"].str();
    st.lon = s["lng"].num(NAN);
    st.lat = s["lat"].num(NAN);
    if (st.id.empty() || st.name.empty() || !std::isfinite(st.lon) || !std::isfinite(st.lat)) continue;
    if (st.id.find_first_not_of("0123456789") != std::string::npos) continue;  // sert dans une adresse
    out.push_back(st);
  }
  if (out.empty()) { err = tr("Liste des marégraphes NOAA vide"); return false; }
  std::sort(out.begin(), out.end(), [](const GaugeStation& a, const GaugeStation& b) { return a.name < b.name; });
  return true;
}

static std::string Ymd(long long t) {
  const std::string d = FormatTime(t).substr(0, 10);  // AAAA-MM-JJ
  return d.substr(0, 4) + d.substr(5, 2) + d.substr(8, 2);
}

std::string DataUrl(const std::string& id, long long t0, long long t1) {
  return "https://api.tidesandcurrents.noaa.gov/api/prod/datagetter?product=water_level&application=PersonalDepth"
         "&begin_date=" + Ymd(t0) + "&end_date=" + Ymd(t1 - 86400) + "&datum=MLLW&station=" + id +
         "&time_zone=gmt&units=metric&format=csv";
}

bool Parse(const std::string& text, std::vector<long long>& T, std::vector<double>& H, int& skipped,
           std::string& err) {
  T.clear(); H.clear();
  skipped = 0;
  size_t p = 0;
  while (p < text.size()) {
    size_t e = text.find('\n', p);
    if (e == std::string::npos) e = text.size();
    std::string l = text.substr(p, e - p);
    p = e + 1;
    if (!l.empty() && l.back() == '\r') l.pop_back();
    if (l.empty() || l[0] == '#' || l.compare(0, 9, "Date Time") == 0) continue;
    const size_t a = l.find(','), b = a == std::string::npos ? a : l.find(',', a + 1);
    if (a == std::string::npos) { ++skipped; continue; }
    const std::string when = l.substr(0, a), val = l.substr(a + 1, b == std::string::npos ? std::string::npos : b - a - 1);
    if (val.empty()) continue;  // mesure absente
    const size_t sp = when.find(' ');
    long long t;
    double h;
    if (!ParseDateTime(when.substr(0, sp), sp == std::string::npos ? std::string() : when.substr(sp + 1), 1, t) ||
        !ParseFieldNumber(val, '.', h)) {
      ++skipped;
      continue;
    }
    T.push_back(t);
    H.push_back(h);
  }
  if (T.empty()) { err = tr("Aucune hauteur d'eau dans le fichier NOAA"); return false; }
  return true;
}

bool HasData(const std::string& text) {
  std::vector<long long> t;
  std::vector<double> h;
  int sk;
  std::string err;
  return text.find("Date Time, Water Level") != std::string::npos && Parse(text, t, h, sk, err);
}

namespace {
class NoaaProvider : public GaugeProvider {
 public:
  std::string id() const override { return "noaa"; }
  std::string title() const override { return "NOAA CO-OPS"; }
  std::string dirName() const override { return "NOAA"; }
  std::string region() const override { return tr("États-Unis"); }
  std::string datum() const override { return "MLLW"; }
  std::string licence() const override { return tr("domaine public (NOAA, gouvernement des États-Unis)"); }
  std::string citation() const override { return "NOAA CO-OPS, tidesandcurrents.noaa.gov"; }
  std::string stationsUrl() const override { return kStationsUrl; }
  int maxChunkDays() const override { return 31; }
  bool parseStations(const std::string& text, std::vector<GaugeStation>& out, std::string& err) const override {
    return ParseStations(text, out, err);
  }
  std::string dataUrl(const std::string& id, long long t0, long long t1) const override { return DataUrl(id, t0, t1); }
  // seulement les fichiers enregistrés par le plugin (référence et unité connues)
  bool recognises(const std::string& head) const override { return head.compare(0, std::strlen(kMarker), kMarker) == 0; }
  bool hasData(const std::string& text) const override { return HasData(text); }
  std::string store(const std::string& response) const override {
    return response.compare(0, std::strlen(kMarker), kMarker) == 0 ? response : std::string(kMarker) + "\n" + response;
  }
  bool parse(const std::string& text, std::vector<long long>& t, std::vector<double>& h, int& skipped,
             std::string& err) const override {
    return Parse(text, t, h, skipped, err);
  }
};
}  // namespace

const GaugeProvider& Provider() {
  static const NoaaProvider p;
  return p;
}

}  // namespace noaa
}  // namespace pd

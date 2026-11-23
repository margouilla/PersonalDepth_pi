// PersonalDepth_pi - marégraphes : hauteurs d'eau observées en ligne
// Copyright (C) 2026 Marcaurelio Franzetti
// SPDX-License-Identifier: GPL-2.0-or-later
#include "TideGauge.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <map>

#include "FileUtil.h"
#include "PointCloud.h"
#include "NoaaCoops.h"
#include "Refmar.h"
#include "WaterLevel.h"

namespace fs = std::filesystem;

namespace pd {

std::string GaugeStation::label() const {
  // « LE_CONQUET » -> « Le Conquet » ; « BOULOGNE-SUR-MER » -> « Boulogne-Sur-Mer »
  std::string o;
  bool start = true;
  for (char c : name) {
    if (c == '_') c = ' ';
    const unsigned char u = (unsigned char)c;
    if (u < 128 && std::isalpha(u)) {
      o += start ? (char)std::toupper(u) : (char)std::tolower(u);
      start = false;
    } else {
      o += c;
      start = (c == ' ' || c == '-' || c == '\'');
    }
  }
  return o;
}

std::string GaugeStation::folder() const {
  std::string n;
  for (char c : name) {
    const unsigned char u = (unsigned char)c;
    n += (u < 128 && (std::isalnum(u) || c == '-')) ? c : '_';
  }
  return id + "_" + n;
}

std::vector<GaugeChunk> GaugeProvider::chunks(const std::string& stationId, long long t0, long long t1,
                                              int maxDays) const {
  std::vector<GaugeChunk> v;
  const int days = std::max(1, std::min(maxDays, maxChunkDays()));
  t0 = (t0 >= 0 ? t0 / 86400 : -((-t0 + 86399) / 86400)) * 86400;
  t1 = ((t1 + 86399) / 86400) * 86400;
  for (long long a = t0; a < t1;) {
    const long long b = std::min(t1, a + (long long)days * 86400);
    GaugeChunk c;
    c.t0 = a;
    c.t1 = b;
    c.url = dataUrl(stationId, a, b);
    c.file = gauge::ChunkFile(stationId, a, (int)((b - a) / 86400));
    v.push_back(c);
    a = b;
  }
  return v;
}

const std::vector<const GaugeProvider*>& GaugeProviders() {
  static const std::vector<const GaugeProvider*> v = {&refmar::Provider(), &noaa::Provider()};
  return v;
}

const GaugeProvider* FindGaugeProvider(const std::string& id) {
  for (const auto* p : GaugeProviders())
    if (p->id() == id) return p;
  return nullptr;
}

namespace gauge {

std::string ChunkFile(const std::string& stationId, long long t0, int days) {
  return stationId + "_" + FormatTime(t0).substr(0, 10) + "_" + std::to_string(days) + "d.txt";
}

bool ParseChunkFile(const std::string& f, long long& t0, int& days) {
  // ..._AAAA-MM-JJ_<N>d.txt
  if (f.size() < 20 || f.compare(f.size() - 5, 5, "d.txt") != 0) return false;
  const size_t u = f.rfind('_', f.size() - 6);
  if (u == std::string::npos || u < 11) return false;
  const std::string n = f.substr(u + 1, f.size() - 5 - u - 1), date = f.substr(u - 10, 10);
  if (n.empty() || n.find_first_not_of("0123456789") != std::string::npos || f[u - 11] != '_') return false;
  days = std::atoi(n.c_str());
  return days > 0 && ParseDateTime(date, "", 1, t0);
}

double DistanceKm(double lon1, double lat1, double lon2, double lat2) {
  const double r = M_PI / 180;
  const double dl = (lat2 - lat1) * r, dn = (lon2 - lon1) * r;
  const double a = std::sin(dl / 2) * std::sin(dl / 2) +
                   std::cos(lat1 * r) * std::cos(lat2 * r) * std::sin(dn / 2) * std::sin(dn / 2);
  return 2 * 6371.0 * std::asin(std::min(1.0, std::sqrt(a)));
}

int Nearest(const std::vector<GaugeStation>& st, double lon, double lat, double* km) {
  int best = -1;
  double bd = 1e30;
  for (size_t i = 0; i < st.size(); ++i) {
    const double d = DistanceKm(lon, lat, st[i].lon, st[i].lat);
    if (d < bd) { bd = d; best = (int)i; }
  }
  if (km) *km = best < 0 ? 0 : bd;
  return best;
}

}  // namespace gauge

void GaugeSeries::set(std::vector<long long> t, std::vector<double> h) {
  std::map<long long, std::pair<double, int>> acc;
  for (size_t i = 0; i < t.size() && i < h.size(); ++i) {
    auto& a = acc[t[i]];
    a.first += h[i];
    ++a.second;
  }
  t_.clear();
  h_.clear();
  for (const auto& kv : acc) {
    t_.push_back(kv.first);
    h_.push_back(kv.second.first / kv.second.second);
  }
}

bool GaugeSeries::load(const GaugeProvider& p, const std::string& folder, long long t0, long long t1) {
  std::vector<long long> T;
  std::vector<double> H;
  std::error_code ec;
  for (const auto& e : fs::directory_iterator(U8Path(folder), ec)) {
    if (!e.is_regular_file(ec)) continue;
    long long c0;
    int days;
    if (!gauge::ParseChunkFile(e.path().filename().u8string(), c0, days)) continue;
    if (c0 >= t1 || c0 + (long long)days * 86400 <= t0) continue;
    FILE* f = OpenFile(e.path().u8string(), "rb");
    if (!f) continue;
    std::string s;
    char b[16384];
    size_t n;
    while ((n = std::fread(b, 1, sizeof b, f)) > 0) s.append(b, n);
    std::fclose(f);
    std::vector<long long> t;
    std::vector<double> h;
    int sk = 0;
    std::string err;
    if (!p.parse(s, t, h, sk, err)) continue;
    for (size_t i = 0; i < t.size(); ++i)
      if (t[i] >= t0 && t[i] < t1) { T.push_back(t[i]); H.push_back(h[i]); }
  }
  set(std::move(T), std::move(H));
  return !t_.empty();
}

bool GaugeSeries::at(long long t, long long maxGap, long long maxHold, double& h, long long* tObs) const {
  if (t_.empty()) return false;
  if (t <= t_.front()) {
    if (t_.front() - t > maxHold) return false;
    h = h_.front();
    if (tObs) *tObs = t_.front();
    return true;
  }
  if (t >= t_.back()) {
    if (t - t_.back() > maxHold) return false;
    h = h_.back();
    if (tObs) *tObs = t_.back();
    return true;
  }
  const size_t i = std::upper_bound(t_.begin(), t_.end(), t) - t_.begin();  // t_[i-1] <= t < t_[i]
  if (t_[i] - t_[i - 1] > maxGap) return false;
  const double f = (double)(t - t_[i - 1]) / (double)(t_[i] - t_[i - 1]);
  h = h_[i - 1] + f * (h_[i] - h_[i - 1]);
  if (tObs) *tObs = (t - t_[i - 1] <= t_[i] - t) ? t_[i - 1] : t_[i];
  return true;
}

}  // namespace pd

// PersonalDepth_pi - hauteurs d'eau observées
// Copyright (C) 2026 Marcaurelio Franzetti
// SPDX-License-Identifier: GPL-2.0-or-later
#include "WaterLevel.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <map>

#include "FileUtil.h"
#include "Lang.h"
#include "PointCloud.h"

namespace fs = std::filesystem;

namespace pd {

bool WaterLevelSeries::ParseLine(const std::string& lineIn, int dateOrder, long long& t, double& h) {
  std::string line = lineIn;
  while (!line.empty() && (line.back() == '\r' || line.back() == ' ' || line.back() == '\t'))
    line.pop_back();
  size_t b = 0;
  while (b < line.size() && (line[b] == ' ' || line[b] == '\t')) ++b;
  line = line.substr(b);
  if (line.empty() || line[0] == '#') return false;
  // séparateur : virgule, point-virgule ou tabulation, sinon le dernier blanc
  // (« 2026-11-01 05:00:00 0.35 »)
  // (point-virgule ou tabulation présents : la virgule peut être décimale)
  const char* seps = line.find_first_of(";\t") != std::string::npos ? ";\t" : ",";
  size_t cut = line.find_last_of(seps);
  if (cut == std::string::npos) cut = line.find_last_of(' ');
  if (cut == std::string::npos) return false;
  std::string when = line.substr(0, cut), val = line.substr(cut + 1);
  while (!when.empty() && (when.back() == ' ' || when.back() == ',' || when.back() == ';'))
    when.pop_back();
  // « 2026-11-01,05:00:00,0.35 » : date et heure en deux champs
  std::string date = when, time;
  const size_t c2 = when.find_first_of(seps);
  if (c2 != std::string::npos) {
    date = when.substr(0, c2);
    time = when.substr(c2 + 1);
    while (!time.empty() && (time.front() == ' ')) time.erase(time.begin());
  }
  if (!ParseFieldNumber(val, '.', h) && !ParseFieldNumber(val, ',', h)) return false;
  if (!std::isfinite(h)) return false;
  return ParseDateTime(date, time, dateOrder, t);
}

static std::vector<std::string> ListFiles(const std::string& path) {
  std::vector<std::string> out;
  std::error_code ec;
  const fs::path p = U8Path(path);
  if (fs::is_directory(p, ec)) {
    for (const auto& e : fs::directory_iterator(p, ec)) {
      if (!e.is_regular_file(ec)) continue;
      std::string ext = e.path().extension().u8string();
      std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
      if (ext == ".txt" || ext == ".csv" || ext == ".dat") out.push_back(e.path().u8string());
    }
    std::sort(out.begin(), out.end());
  } else if (!path.empty() && fs::exists(p, ec)) {
    out.push_back(path);
  }
  return out;
}

std::string WaterLevelSeries::FirstFile(const std::string& path) {
  const auto v = ListFiles(path);
  return v.empty() ? std::string() : v.front();
}

bool WaterLevelSeries::load(const std::string& path, const TextFormat& fmt, double toMeters,
                            std::string& err) {
  t_.clear(); h_.clear(); files_.clear(); key_.clear(); skipped_ = 0;
  std::error_code ec;
  files_ = ListFiles(path);
  if (files_.empty()) {
    if (fs::is_directory(U8Path(path), ec))
      err = pd::tr("Aucun fichier de hauteurs d'eau (.txt, .csv, .dat) dans ") + path;
    else
      err = pd::tr("Fichier ou dossier de hauteurs d'eau introuvable : ") + path;
    return false;
  }
  key_ = fmt.serialize() + "|" + std::to_string(toMeters) + "|";
  std::map<long long, std::pair<double, int>> acc;  // doublons : moyenne
  for (const auto& f : files_) {
    FILE* in = OpenFile(f, "rb");
    if (!in) { err = pd::tr("Impossible d'ouvrir ") + f; return false; }
    std::string line;
    int c;
    long long lineNo = 0;
    auto parse = [&](long long& t, double& h) {
      if (!fmt.custom) return ParseLine(line, fmt.dateOrder, t, h);
      if (lineNo <= fmt.skipLines) return false;
      std::string l = line;
      if (!l.empty() && l.back() == '\r') l.pop_back();
      const auto v = SplitFields(l, fmt);
      const int nf = (int)v.size();
      if (fmt.colDate < 1 || fmt.colDate > nf || fmt.colZ < 1 || fmt.colZ > nf) return false;
      const std::string tm = (fmt.colTime && fmt.colTime <= nf) ? v[fmt.colTime - 1] : std::string();
      return ParseFieldDateTime(v[fmt.colDate - 1], tm, fmt, t) &&
             ParseFieldNumber(v[fmt.colZ - 1], fmt.decimal, h) && std::isfinite(h);
    };
    auto flush = [&]() {
      ++lineNo;
      long long t; double h;
      if (parse(t, h)) { auto& a = acc[t]; a.first += h * toMeters; ++a.second; }
      else if (lineNo > (fmt.custom ? fmt.skipLines : 0) &&
               line.find_first_not_of(" \t\r") != std::string::npos)
        ++skipped_;
      line.clear();
    };
    while ((c = std::fgetc(in)) != EOF) {
      if (c == '\n') flush();
      else line += (char)c;
    }
    flush();
    std::fclose(in);
    key_ += f + "@" + std::to_string((long long)FileMTime(f)) + ";";
  }
  for (const auto& kv : acc) {
    t_.push_back(kv.first);
    h_.push_back((float)(kv.second.first / kv.second.second));
  }
  if (t_.size() < 1) { err = pd::tr("Aucune hauteur d'eau lisible (format attendu : date heure, hauteur)"); return false; }
  hMin_ = *std::min_element(h_.begin(), h_.end());
  hMax_ = *std::max_element(h_.begin(), h_.end());
  // tolérance aux extrémités : un pas médian de la série (au moins 1 h, au
  // plus 1 jour ; 1 jour pour des valeurs journalières)
  long long step = 3600;
  if (t_.size() > 1) {
    std::vector<long long> d;
    for (size_t i = 1; i < t_.size(); ++i) d.push_back(t_[i] - t_[i - 1]);
    std::nth_element(d.begin(), d.begin() + d.size() / 2, d.end());
    step = d[d.size() / 2];
  }
  tol_ = std::min<long long>(86400, std::max<long long>(3600, step));
  return true;
}

bool WaterLevelSeries::at(long long t, float& h) const {
  if (t_.empty()) return false;
  if (t <= t_.front()) {
    if (t_.front() - t > tol_) return false;
    h = h_.front();
    return true;
  }
  if (t >= t_.back()) {
    if (t - t_.back() > tol_) return false;
    h = h_.back();
    return true;
  }
  const size_t i = std::upper_bound(t_.begin(), t_.end(), t) - t_.begin();  // t_[i-1] <= t < t_[i]
  const double f = (double)(t - t_[i - 1]) / (double)(t_[i] - t_[i - 1]);
  h = (float)(h_[i - 1] + f * (h_[i] - h_[i - 1]));
  return true;
}

std::string FormatTime(long long t, bool seconds) {
  long long days = t >= 0 ? t / 86400 : -((-t + 86399) / 86400);
  long long s = t - days * 86400;
  // jours depuis 1970 -> date civile (H. Hinnant)
  days += 719468;
  const long long era = (days >= 0 ? days : days - 146096) / 146097;
  const unsigned doe = (unsigned)(days - era * 146097);
  const unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
  const long long y0 = (long long)yoe + era * 400;
  const unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
  const unsigned mp = (5 * doy + 2) / 153;
  const unsigned d = doy - (153 * mp + 2) / 5 + 1;
  const unsigned m = mp < 10 ? mp + 3 : mp - 9;
  const long long y = y0 + (m <= 2);
  char b[40];
  if (seconds)
    std::snprintf(b, sizeof b, "%04lld-%02u-%02u %02lld:%02lld:%02lld", y, m, d, s / 3600, (s / 60) % 60, s % 60);
  else
    std::snprintf(b, sizeof b, "%04lld-%02u-%02u %02lld:%02lld", y, m, d, s / 3600, (s / 60) % 60);
  return b;
}

}  // namespace pd

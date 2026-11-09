// PersonalDepth_pi - personal bathymetry plugin for OpenCPN
// Copyright (C) 2026 Marcaurelio Franzetti <marcaurelio.franzetti@gmail.com>
//
// This program is free software; you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation; either version 2 of the License, or
// (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// SPDX-License-Identifier: GPL-2.0-or-later

#include "PointCloud.h"
#include "WaterLevel.h"
#include "Lang.h"
#include "FileUtil.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <limits>
#include <sstream>
#include <unordered_map>

#include "Projection.h"

#ifdef PD_HAVE_GDAL
#include <gdal_priv.h>
#endif

namespace pd {

// ---------------------------------------------------------------- PointCloud
void PointCloud::reserve(size_t n) {
  qlon.reserve(n);
  qlat.reserve(n);
  z.reserve(n);
}

void PointCloud::add(double lon, double lat, float depth) {
  qlon.push_back((int32_t)std::llround(lon / kQ));
  qlat.push_back((int32_t)std::llround(lat / kQ));
  z.push_back(depth);
}

void PointCloud::clear() {
  qlon.clear(); qlat.clear(); z.clear();
  qlon.shrink_to_fit(); qlat.shrink_to_fit(); z.shrink_to_fit();
  tMin = tMax = 0;
}

void PointCloud::computeBounds() {
  if (empty()) return;
  int32_t a = INT32_MAX, b = INT32_MIN, c = INT32_MAX, d = INT32_MIN;
  float zm = std::numeric_limits<float>::max(), zM = -zm;
  for (size_t i = 0; i < size(); ++i) {
    a = std::min(a, qlon[i]); b = std::max(b, qlon[i]);
    c = std::min(c, qlat[i]); d = std::max(d, qlat[i]);
    zm = std::min(zm, z[i]); zM = std::max(zM, z[i]);
  }
  lonMin = a * kQ; lonMax = b * kQ; latMin = c * kQ; latMax = d * kQ;
  zMin = zm; zMax = zM;
}

static bool IsUserNoData(double v, const std::vector<double>& nd) {
  for (double x : nd)
    if (std::fabs(v - x) <= 1e-6 * std::max(1.0, std::fabs(x))) return true;
  return false;
}

void PointCloud::applySign(const LoadOptions& o, std::vector<uint32_t>* tris) {
  // valeurs absentes : non finies, remplissages énormes, NODATA de l'utilisateur
  filter([&](float v) {
    return std::isfinite(v) && std::fabs(v) < 1e20f && !IsUserNoData(v, o.nodata);
  }, tris);
  bool negate = false;
  if (o.zsign == LoadOptions::ZSign::Elevation) negate = true;
  else if (o.zsign == LoadOptions::ZSign::Auto && !empty()) {
    size_t neg = 0;
    for (float v : z) neg += (v < 0.f);
    negate = neg > (size() * 6) / 10;
  }
  const double k = (negate ? -1.0 : 1.0) * o.zToMeters;  // unité du fichier -> m
  if (k != 1.0 || o.zOffset != 0.f)
    for (float& v : z) v = (float)(v * k) + o.zOffset;
  computeBounds();
}

static const char kMagic1[8] = {'P', 'D', 'P', 'C', '0', '0', '0', '1'};
static const char kMagic[8] = {'P', 'D', 'P', 'C', '0', '0', '0', '2'};

bool PointCloud::saveCache(const std::string& path) const {
  FILE* f = OpenFile(path, "wb");
  if (!f) return false;
  uint64_t n = size();
  const double steps[2] = {srcStepX, srcStepY};
  bool ok = std::fwrite(kMagic, 1, 8, f) == 8 && std::fwrite(&n, 8, 1, f) == 1 &&
            std::fwrite(steps, 8, 2, f) == 2 &&
            std::fwrite(qlon.data(), 4, n, f) == n &&
            std::fwrite(qlat.data(), 4, n, f) == n &&
            std::fwrite(z.data(), 4, n, f) == n;
  if (ok && tMin) {  // dates des sondes (facultatif, en fin de fichier)
    const long long tt[2] = {tMin, tMax};
    ok = std::fwrite("PDT1", 1, 4, f) == 4 && std::fwrite(tt, 8, 2, f) == 2;
  }
  std::fclose(f);
  if (!ok) RemoveFile(path);
  return ok;
}

bool PointCloud::loadCache(const std::string& path) {
  FILE* f = OpenFile(path, "rb");
  if (!f) return false;
  char m[8]; uint64_t n = 0;
  bool ok = std::fread(m, 1, 8, f) == 8 &&
            (!std::memcmp(m, kMagic, 8) || !std::memcmp(m, kMagic1, 8)) &&
            std::fread(&n, 8, 1, f) == 1;
  srcStepX = srcStepY = 0;
  if (ok && !std::memcmp(m, kMagic, 8)) {
    double steps[2];
    ok = std::fread(steps, 8, 2, f) == 2;
    srcStepX = steps[0]; srcStepY = steps[1];
  }
  if (ok) {
    qlon.resize(n); qlat.resize(n); z.resize(n);
    ok = std::fread(qlon.data(), 4, n, f) == n &&
         std::fread(qlat.data(), 4, n, f) == n &&
         std::fread(z.data(), 4, n, f) == n;
  }
  tMin = tMax = 0;
  if (ok) {
    char m4[4]; long long tt[2];
    if (std::fread(m4, 1, 4, f) == 4 && !std::memcmp(m4, "PDT1", 4) && std::fread(tt, 8, 2, f) == 2) {
      tMin = tt[0]; tMax = tt[1];
    }
  }
  std::fclose(f);
  if (!ok) { clear(); return false; }
  computeBounds();
  return true;
}

// ---------------------------------------------------------- parseur rapide
// Lit un nombre décimal ; renvoie false si aucun chiffre. Précision : < 1 ulp
// pour ≤ 15 chiffres significatifs (largement suffisant pour coords et sondes).
static inline bool isSep(char c) {
  return c == ' ' || c == '\t' || c == ',' || c == ';' || c == '\r';
}

static bool parseNum(const char*& p, const char* e, double& out) {
  while (p < e && isSep(*p)) ++p;
  if (p >= e || *p == '\n') return false;
  bool neg = false;
  if (*p == '-' || *p == '+') { neg = (*p == '-'); ++p; }
  uint64_t mant = 0; int exp10 = 0, nd = 0; bool any = false;
  while (p < e && *p >= '0' && *p <= '9') {
    if (nd < 18) { mant = mant * 10 + (*p - '0'); ++nd; } else ++exp10;
    ++p; any = true;
  }
  if (p < e && *p == '.') {
    ++p;
    while (p < e && *p >= '0' && *p <= '9') {
      if (nd < 18) { mant = mant * 10 + (*p - '0'); ++nd; --exp10; }
      ++p; any = true;
    }
  }
  if (!any) return false;
  if (p < e && (*p == 'e' || *p == 'E')) {
    ++p; bool en = false; int ev = 0;
    if (p < e && (*p == '-' || *p == '+')) { en = (*p == '-'); ++p; }
    while (p < e && *p >= '0' && *p <= '9') { ev = ev * 10 + (*p - '0'); ++p; }
    exp10 += en ? -ev : ev;
  }
  static const double p10[] = {1e0, 1e1, 1e2, 1e3, 1e4, 1e5, 1e6, 1e7, 1e8,
                               1e9, 1e10, 1e11, 1e12, 1e13, 1e14, 1e15,
                               1e16, 1e17, 1e18, 1e19, 1e20, 1e21, 1e22};
  double v = (double)mant;
  if (exp10 < 0) v = (exp10 >= -22) ? v / p10[-exp10] : v * std::pow(10.0, exp10);
  else if (exp10 > 0) v = (exp10 <= 22) ? v * p10[exp10] : v * std::pow(10.0, exp10);
  out = neg ? -v : v;
  // un nombre doit être suivi d'un séparateur ou d'une fin de ligne
  return p >= e || isSep(*p) || *p == '\n';
}

double LatticeStepUnits(const std::vector<int64_t>& q) {
  if (q.size() < 16) return 0;
  std::vector<int64_t> u(q);
  std::sort(u.begin(), u.end());
  u.erase(std::unique(u.begin(), u.end()), u.end());
  if (u.size() < 4) return 0;
  std::vector<int64_t> d(u.size() - 1);
  for (size_t k = 0; k + 1 < u.size(); ++k) d[k] = u[k + 1] - u[k];
  std::vector<int64_t> sd(d);
  std::sort(sd.begin(), sd.end());
  int64_t best = 0; size_t bestN = 0;
  for (size_t a = 0, b = 0; a < sd.size(); a = b) {
    b = a;
    while (b < sd.size() && sd[b] - sd[a] <= 4) ++b;
    if (b - a > bestN) { bestN = b - a; best = sd[(a + b) / 2]; }
  }
  if (best < 5) return 0;
  double sum = 0; size_t n = 0, ok = 0;
  for (int64_t x : d) {
    if (std::llabs(x - best) <= 2) { sum += (double)x; ++n; }
    double k = std::round((double)x / best);
    if (k >= 1 && std::fabs(x - k * best) <= 2 + k) ++ok;
  }
  if (ok < d.size() * 95 / 100 || n == 0) return 0;
  return sum / n;
}

// ------------------------------------------------------ format personnalisé
static std::string EscFmt(const std::string& s) {  // ';' '=' '%' encodés
  std::string o;
  for (char c : s) {
    if (c == '%') o += "%25";
    else if (c == ';') o += "%3B";
    else if (c == '=') o += "%3D";
    else o += c;
  }
  return o;
}

static std::string UnescFmt(const std::string& s) {
  std::string o;
  for (size_t i = 0; i < s.size(); ++i) {
    if (s[i] == '%' && i + 2 < s.size()) {
      o += (char)std::strtol(s.substr(i + 1, 2).c_str(), nullptr, 16);
      i += 2;
    } else {
      o += s[i];
    }
  }
  return o;
}

std::string TextFormat::serialize() const {
  if (!custom) return "";
  char b[160];
  std::snprintf(b, sizeof b, "skip=%d;sep=%d;dec=%d;x=%d;y=%d;z=%d;date=%d;time=%d;order=%d",
                skipLines, (int)(unsigned char)sep, (int)(unsigned char)decimal, colX, colY, colZ,
                colDate, colTime, dateOrder);
  std::string s = b;
  if (!dateFmt.empty()) s += ";dfmt=" + EscFmt(dateFmt);
  if (!timeFmt.empty()) s += ";tfmt=" + EscFmt(timeFmt);
  if (!xFmt.empty()) s += ";xfmt=" + EscFmt(xFmt);
  if (!yFmt.empty()) s += ";yfmt=" + EscFmt(yFmt);
  if (tzMinutes) s += ";tz=" + std::to_string(tzMinutes);
  return s;
}

bool TextFormat::deserialize(const std::string& s) {
  *this = TextFormat();
  if (s.empty()) return true;
  TextFormat t;
  t.custom = true;
  size_t a = 0;
  while (a < s.size()) {
    size_t b = s.find(';', a);
    if (b == std::string::npos) b = s.size();
    const std::string kv = s.substr(a, b - a);
    const size_t e = kv.find('=');
    if (e != std::string::npos) {
      const std::string k = kv.substr(0, e);
      const int v = std::atoi(kv.c_str() + e + 1);
      if (k == "dfmt") t.dateFmt = UnescFmt(kv.substr(e + 1));
      else if (k == "tfmt") t.timeFmt = UnescFmt(kv.substr(e + 1));
      else if (k == "xfmt") t.xFmt = UnescFmt(kv.substr(e + 1));
      else if (k == "yfmt") t.yFmt = UnescFmt(kv.substr(e + 1));
      else if (k == "cfmt") t.xFmt = t.yFmt = UnescFmt(kv.substr(e + 1));  // ancien : X et Y
      if (k == "skip") t.skipLines = std::max(0, v);
      else if (k == "sep") t.sep = (char)v;
      else if (k == "dec") t.decimal = v == ',' ? ',' : '.';
      else if (k == "x") t.colX = v;
      else if (k == "y") t.colY = v;
      else if (k == "z") t.colZ = v;
      else if (k == "date") t.colDate = v;
      else if (k == "time") t.colTime = v;
      else if (k == "order") t.dateOrder = v;
      else if (k == "tz") t.tzMinutes = std::max(-720, std::min(840, v));
    }
    a = b + 1;
  }
  // X et Y à 0 : format d'un fichier de hauteurs d'eau (date + valeur)
  if (t.colZ < 1 || t.colX < 0 || t.colY < 0 || (t.colX < 1) != (t.colY < 1)) return false;
  if (t.colX < 1 && t.colDate < 1) return false;
  *this = t;
  return true;
}

std::vector<std::string> SplitFields(const std::string& line, const TextFormat& f) {
  std::vector<std::string> out;
  const size_t n = line.size();
  auto blank = [](char c) { return c == ' ' || c == '\t' || c == '\r'; };
  if (f.sep == ' ' || (f.sep == 0 && line.find_first_of(f.decimal == ',' ? ";\t" : ",;\t") == std::string::npos)) {
    // espaces / tabulations successifs = un séparateur
    size_t i = 0;
    while (i < n) {
      while (i < n && blank(line[i])) ++i;
      if (i >= n) break;
      size_t j = i;
      while (j < n && !blank(line[j])) ++j;
      out.push_back(line.substr(i, j - i));
      i = j;
    }
    return out;
  }
  // séparateur explicite (ou détecté) : champs vides conservés
  char sep = f.sep;
  if (sep == 0) {
    if (line.find(';') != std::string::npos) sep = ';';
    else if (line.find('\t') != std::string::npos) sep = '\t';
    else sep = ',';
  }
  size_t i = 0;
  for (;;) {
    size_t j = line.find(sep, i);
    if (j == std::string::npos) j = n;
    size_t a = i, b = j;
    while (a < b && blank(line[a])) ++a;
    while (b > a && blank(line[b - 1])) --b;
    out.push_back(line.substr(a, b - a));
    if (j >= n) break;
    i = j + 1;
  }
  return out;
}

bool ParseFieldNumber(const std::string& s, char decimal, double& v) {
  if (s.empty()) return false;
  std::string t = s;
  if (decimal == ',')
    for (char& c : t) if (c == ',') c = '.';
  // analyseur interne : indépendant de la langue du système (strtod suit la locale)
  const char* p = t.c_str();
  const char* e = p + t.size();
  if (!parseNum(p, e, v)) return false;
  while (p < e && isSep(*p)) ++p;
  return p == e && std::isfinite(v);
}

// jours depuis 1970-01-01 (calendrier grégorien proleptique)
static long long DaysFromCivil(int y, int m, int d) {
  y -= m <= 2;
  const long long era = (y >= 0 ? y : y - 399) / 400;
  const unsigned yoe = (unsigned)(y - era * 400);
  const unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
  const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  return era * 146097 + (long long)doe - 719468;
}

static bool ParseTimeOfDay(const char* p, double& sec) {
  int h = 0, mi = 0; double s = 0;
  char* e = nullptr;
  h = (int)std::strtol(p, &e, 10);
  if (e == p || *e != ':') return false;
  p = e + 1;
  mi = (int)std::strtol(p, &e, 10);
  if (e == p) return false;
  if (*e == ':') {
    p = e + 1;
    s = std::strtod(p, &e);
    if (e == p) return false;
  }
  if (h < 0 || h > 24 || mi < 0 || mi > 59 || s < 0 || s >= 61) return false;
  sec = h * 3600.0 + mi * 60.0 + s;
  return true;
}

bool ParseDateTime(const std::string& date, const std::string& time, int dateOrder, long long& t) {
  if (date.empty()) return false;
  int n[3] = {0, 0, 0}, k = 0;
  size_t i = 0;
  const std::string& s = date;
  if (s.size() >= 8 && std::all_of(s.begin(), s.begin() + 8, ::isdigit) &&
      (s.size() == 8 || !std::isdigit((unsigned char)s[8]))) {  // 20210325
    n[0] = std::atoi(s.substr(0, 4).c_str()); n[1] = std::atoi(s.substr(4, 2).c_str());
    n[2] = std::atoi(s.substr(6, 2).c_str()); k = 3; i = 8;
  } else {
    while (k < 3 && i < s.size()) {
      if (!std::isdigit((unsigned char)s[i])) return false;
      int v = 0;
      while (i < s.size() && std::isdigit((unsigned char)s[i])) v = v * 10 + (s[i++] - '0');
      n[k++] = v;
      if (k < 3) {
        if (i >= s.size() || !(s[i] == '-' || s[i] == '/' || s[i] == '.')) return false;
        ++i;
      }
    }
  }
  if (k < 3) return false;
  int y, m, d;
  int order = dateOrder;
  if (order == 0) order = n[0] > 31 ? 1 : (n[1] > 12 ? 3 : 2);
  if (order == 1) { y = n[0]; m = n[1]; d = n[2]; }
  else if (order == 2) { d = n[0]; m = n[1]; y = n[2]; }
  else { m = n[0]; d = n[1]; y = n[2]; }
  if (y < 100) y += y < 70 ? 2000 : 1900;
  if (y < 1900 || y > 2200 || m < 1 || m > 12 || d < 1 || d > 31) return false;
  double sec = 0;
  if (i < s.size() && (s[i] == 'T' || s[i] == ' ' || s[i] == '_')) {  // date et heure
    if (!ParseTimeOfDay(s.c_str() + i + 1, sec)) return false;
  } else if (!time.empty()) {
    if (!ParseTimeOfDay(time.c_str(), sec)) return false;
  }
  t = DaysFromCivil(y, m, d) * 86400LL + (long long)std::floor(sec);
  return true;
}

// ---------------------------------------------------------------- gabarits
// Un gabarit est une suite de champs (lettres répétées), de littéraux et
// d'espaces. « \x » : x littéral. Un champ suivi directement d'un autre champ
// (sans séparateur) a une largeur fixe égale au nombre de lettres ; sinon les
// zéros de tête sont facultatifs. « s.s », « D.D », « M.M » : partie décimale.
namespace {
struct PatTok {
  char letter = 0;    // 0 : littéral ; ' ' : espaces
  int count = 0;
  bool decimals = false;
  bool ampm = false;
  std::string lit;
};

std::vector<PatTok> Tokenize(const std::string& pat, const char* letters) {
  std::vector<PatTok> out;
  for (size_t i = 0; i < pat.size();) {
    const char c = pat[i];
    PatTok t;
    if (c == '\\' && i + 1 < pat.size()) {
      t.lit = std::string(1, pat[i + 1]);
      i += 2;
    } else if (c == ' ' || c == '\t') {
      t.letter = ' ';
      while (i < pat.size() && (pat[i] == ' ' || pat[i] == '\t')) ++i;
    } else if (c == 'A' && i + 1 < pat.size() && pat[i + 1] == 'P' && std::strchr(letters, 'z')) {
      t.ampm = true;
      t.letter = 'A';
      i += 2;
    } else if (std::strchr(letters, c)) {
      t.letter = c;
      while (i < pat.size() && pat[i] == c) { ++t.count; ++i; }
      if (i + 1 < pat.size() && pat[i] == '.' && pat[i + 1] == c) {  // « s.s »
        t.decimals = true;
        ++i;
        while (i < pat.size() && pat[i] == c) ++i;
      }
    } else {
      t.lit = std::string(1, c);
      ++i;
    }
    if (!t.lit.empty() && !out.empty() && out.back().letter == 0 && !out.back().ampm)
      out.back().lit += t.lit;  // littéraux consécutifs regroupés
    else
      out.push_back(t);
  }
  return out;
}

// Lit un nombre : largeur fixe `width` (0 = variable), signe facultatif,
// partie décimale si `decimals` (point ou virgule)
bool ReadNum(const std::string& s, size_t& i, int width, bool sign, bool decimals, double& v,
             int* nDigits = nullptr) {
  const size_t i0 = i;
  bool neg = false;
  if (sign && i < s.size() && (s[i] == '-' || s[i] == '+')) { neg = s[i] == '-'; ++i; }
  const size_t d0 = i;
  double x = 0;
  while (i < s.size() && std::isdigit((unsigned char)s[i]) && (width == 0 || (int)(i - d0) < width))
    x = x * 10 + (s[i++] - '0');
  if (i == d0 || (width > 0 && (int)(i - d0) != width)) { i = i0; return false; }
  if (nDigits) *nDigits = (int)(i - d0);
  if (decimals && i + 1 < s.size() && (s[i] == '.' || s[i] == ',') && std::isdigit((unsigned char)s[i + 1])) {
    ++i;
    double f = 0.1;
    while (i < s.size() && std::isdigit((unsigned char)s[i])) { x += (s[i++] - '0') * f; f /= 10; }
  }
  v = neg ? -x : x;
  return true;
}

int MonthByName(const std::string& s, size_t& i) {
  // mot lu, accents français ramenés à l'ASCII (é è ê -> e, û -> u, ô -> o)
  size_t j = i;
  std::string w;
  while (j < s.size()) {
    const unsigned char c = (unsigned char)s[j];
    if (std::isalpha(c)) { w += (char)std::tolower(c); ++j; continue; }
    if (c == 0xC3 && j + 1 < s.size()) {
      const unsigned char d = (unsigned char)s[j + 1];
      if (d == 0xA9 || d == 0xA8 || d == 0xAA || d == 0x89) w += 'e';
      else if (d == 0xBB || d == 0xB9) w += 'u';
      else if (d == 0xB4) w += 'o';
      else break;
      j += 2;
      continue;
    }
    break;
  }
  if (w.size() < 3) return 0;
  static const char* en[] = {"january", "february", "march", "april", "may", "june", "july",
                             "august", "september", "october", "november", "december"};
  static const char* fr[] = {"janvier", "fevrier", "mars", "avril", "mai", "juin", "juillet",
                             "aout", "septembre", "octobre", "novembre", "decembre"};
  int m = 0;
  for (int k = 0; k < 12 && !m; ++k)  // nom complet
    if (w == en[k] || w == fr[k]) m = k + 1;
  for (int k = 0; k < 12 && !m; ++k)  // abréviation anglaise à 3 lettres
    if (w.size() == 3 && std::string(en[k]).compare(0, 3, w) == 0) m = k + 1;
  for (int k = 0; k < 12 && !m; ++k)  // abréviation française (janv, févr, juil, sept…)
    if (w.size() >= 3 && std::string(fr[k]).compare(0, w.size(), w) == 0) m = k + 1;
  if (m) i = j;
  return m;
}

bool SkipSpaces(const std::string& s, size_t& i) {
  const size_t i0 = i;
  while (i < s.size() && (s[i] == ' ' || s[i] == '\t')) ++i;
  return i > i0;
}
}  // namespace

bool ParseDatePattern(const std::string& text, const std::string& pattern, DateParts& p) {
  const std::vector<PatTok> tk = Tokenize(pattern, "yMdhmsz");
  const std::string& s = text;
  size_t i = 0;
  SkipSpaces(s, i);
  for (size_t k = 0; k < tk.size(); ++k) {
    const PatTok& t = tk[k];
    const bool nextField = k + 1 < tk.size() && tk[k + 1].letter && tk[k + 1].letter != ' ';
    const int width = (nextField && t.count > 1) ? t.count : 0;
    double v;
    if (t.letter == ' ') { SkipSpaces(s, i); continue; }
    if (t.letter == 0) {  // littéral
      if (s.compare(i, t.lit.size(), t.lit) != 0) return false;
      i += t.lit.size();
      continue;
    }
    if (t.ampm) {
      if (i + 1 >= s.size()) return false;
      const char a = (char)std::toupper((unsigned char)s[i]);
      if ((a != 'A' && a != 'P') || std::toupper((unsigned char)s[i + 1]) != 'M') return false;
      p.ampm = a == 'A' ? 1 : 2;
      i += 2;
      continue;
    }
    switch (t.letter) {
      case 'y': {
        int nd = 0;
        if (!ReadNum(s, i, width, false, false, v, &nd)) return false;
        int y = (int)v;
        if (nd <= 2) y += y < 70 ? 2000 : 1900;
        p.y = y;
        break;
      }
      case 'M':
        if (t.count >= 3) {
          const int m = MonthByName(s, i);
          if (!m) return false;
          p.mo = m;
        } else {
          if (!ReadNum(s, i, width, false, false, v)) return false;
          p.mo = (int)v;
        }
        break;
      case 'd':
        if (!ReadNum(s, i, width, false, false, v)) return false;
        p.d = (int)v;
        break;
      case 'h':
        if (!ReadNum(s, i, width, false, false, v)) return false;
        p.h = (int)v;
        break;
      case 'm':
        if (!ReadNum(s, i, width, false, false, v)) return false;
        p.mi = (int)v;
        break;
      case 's':
        if (!ReadNum(s, i, width, false, t.decimals, v)) return false;
        p.s = v;
        break;
      case 'z': {
        int nd = 0;
        if (!ReadNum(s, i, width, false, false, v, &nd)) return false;
        (void)nd;
        p.s = std::max(0.0, p.s) + v / 1000.0;  // millièmes de seconde
        break;
      }
    }
  }
  SkipSpaces(s, i);
  return i == s.size();
}

bool ParseCoordPattern(const std::string& text, const std::string& pattern, double& deg) {
  const std::vector<PatTok> tk = Tokenize(pattern, "DMSX");
  const std::string& s = text;
  size_t i = 0;
  SkipSpaces(s, i);
  double D = 0, M = 0, S = 0;
  bool neg = false, hemiNeg = false;
  for (size_t k = 0; k < tk.size(); ++k) {
    const PatTok& t = tk[k];
    const bool nextField = k + 1 < tk.size() && tk[k + 1].letter && tk[k + 1].letter != ' ';
    const int width = (nextField && t.count > 1) ? t.count : 0;
    double v;
    if (t.letter == ' ') { SkipSpaces(s, i); continue; }
    if (t.letter == 0) {
      if (s.compare(i, t.lit.size(), t.lit) != 0) return false;
      i += t.lit.size();
      continue;
    }
    switch (t.letter) {
      case 'D':
        if (i < s.size() && s[i] == '-') neg = true;
        if (!ReadNum(s, i, width, true, t.decimals, v)) return false;
        D = std::fabs(v);
        break;
      case 'M':
        if (!ReadNum(s, i, width, false, t.decimals, v)) return false;
        M = v;
        break;
      case 'S':
        if (!ReadNum(s, i, width, false, t.decimals, v)) return false;
        S = v;
        break;
      case 'X': {
        if (i >= s.size()) return false;
        const char h = (char)std::toupper((unsigned char)s[i]);
        if (h != 'N' && h != 'S' && h != 'E' && h != 'W' && h != 'O') return false;
        hemiNeg = h == 'S' || h == 'W' || h == 'O';
        ++i;
        break;
      }
    }
  }
  SkipSpaces(s, i);
  if (i != s.size() || M >= 60 || S >= 60) return false;
  deg = D + M / 60.0 + S / 3600.0;
  if (neg != hemiNeg) deg = -deg;  // nombre négatif dans un hémisphère négatif : positif
  return true;
}

bool ParseFieldCoord(const std::string& s, const TextFormat& f, bool isY, double& v) {
  const std::string& p = isY ? f.yFmt : f.xFmt;
  if (p.empty()) return ParseFieldNumber(s, f.decimal, v);
  return ParseCoordPattern(s, p, v);
}

static bool ParseFieldDateTimeLocal(const std::string& date, const std::string& time,
                                    const TextFormat& f, long long& t);

bool ParseFieldDateTime(const std::string& date, const std::string& time, const TextFormat& f,
                        long long& t) {
  if (!ParseFieldDateTimeLocal(date, time, f, t)) return false;
  t -= (long long)f.tzMinutes * 60;  // heure du fichier -> UTC
  return true;
}

const std::vector<int>& TimeZoneOffsets() {
  static const std::vector<int> v = {
      -720, -660, -600, -570, -540, -480, -420, -360, -300, -240, -210, -180, -120, -60, 0,
      60,   120,  180,  210,  240,  270,  300,  330,  345,  360,  390,  420,  480,  525, 540,
      570,  600,  630,  660,  720,  765,  780,  840};
  return v;
}

std::string TimeZoneLabel(int m) {
  char b[24];
  std::snprintf(b, sizeof b, "UTC%c%02d:%02d", m < 0 ? '-' : '+', std::abs(m) / 60, std::abs(m) % 60);
  return b;
}

static bool ParseFieldDateTimeLocal(const std::string& date, const std::string& time,
                                    const TextFormat& f, long long& t) {
  if (f.dateFmt.empty() && f.timeFmt.empty()) return ParseDateTime(date, time, f.dateOrder, t);
  DateParts p;
  if (!date.empty()) {
    if (!f.dateFmt.empty()) {
      if (!ParseDatePattern(date, f.dateFmt, p)) return false;
    } else {
      long long t0;  // date seule reconnue automatiquement
      if (!ParseDateTime(date, "", f.dateOrder, t0)) return false;
      const long long days = t0 >= 0 ? t0 / 86400 : (t0 - 86399) / 86400;
      long long z = days + 719468;  // civil depuis jours (H. Hinnant)
      const long long era = (z >= 0 ? z : z - 146096) / 146097;
      const unsigned doe = (unsigned)(z - era * 146097);
      const unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
      const unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
      const unsigned mp = (5 * doy + 2) / 153;
      p.d = (int)(doy - (153 * mp + 2) / 5 + 1);
      p.mo = (int)(mp < 10 ? mp + 3 : mp - 9);
      p.y = (int)(yoe + era * 400 + (p.mo <= 2));
      const long long sec = t0 - days * 86400;
      if (sec) { p.h = (int)(sec / 3600); p.mi = (int)(sec / 60 % 60); p.s = (double)(sec % 60); }
    }
  }
  if (!time.empty()) {
    if (!f.timeFmt.empty()) {
      DateParts q;
      if (!ParseDatePattern(time, f.timeFmt, q)) return false;
      if (q.h >= 0) p.h = q.h;
      if (q.mi >= 0) p.mi = q.mi;
      if (q.s >= 0) p.s = q.s;
      if (q.ampm) p.ampm = q.ampm;
      if (q.y >= 0) p.y = q.y;
      if (q.mo >= 0) p.mo = q.mo;
      if (q.d >= 0) p.d = q.d;
    } else {
      double sec;
      if (!ParseTimeOfDay(time.c_str(), sec)) return false;
      p.h = (int)(sec / 3600);
      p.mi = (int)(std::fmod(sec, 3600) / 60);
      p.s = std::fmod(sec, 60);
    }
  }
  // champs absents : 1970-01-01 00:00:00 ; une date est nécessaire au levé
  if (p.y < 0 || p.mo < 0 || p.d < 0) return false;
  int h = std::max(0, p.h);
  if (p.ampm == 2 && h < 12) h += 12;
  if (p.ampm == 1 && h == 12) h = 0;
  if (p.y < 1900 || p.y > 2200 || p.mo < 1 || p.mo > 12 || p.d < 1 || p.d > 31 || h > 24 ||
      p.mi > 59 || p.s >= 61)
    return false;
  t = DaysFromCivil(p.y, p.mo, p.d) * 86400LL + h * 3600LL + std::max(0, p.mi) * 60LL +
      (long long)std::floor(std::max(0.0, p.s));
  return true;
}

bool ReadTextHead(const std::string& path, size_t maxLines, std::vector<std::string>& lines,
                  std::string& err) {
  lines.clear();
  std::ifstream in(U8Path(path), std::ios::binary);
  if (!in) { err = pd::tr("Impossible d'ouvrir ") + path; return false; }
  std::string l;
  while (lines.size() < maxLines && std::getline(in, l)) {
    if (!l.empty() && l.back() == '\r') l.pop_back();
    if (lines.empty() && l.size() >= 3 && (unsigned char)l[0] == 0xEF && (unsigned char)l[1] == 0xBB &&
        (unsigned char)l[2] == 0xBF)
      l.erase(0, 3);  // BOM UTF-8
    lines.push_back(l);
  }
  return true;
}

TextFormat GuessWaterLevelFormat(const std::vector<std::string>& lines) {
  TextFormat f;
  f.custom = true;
  f.colX = f.colY = 0;
  int semi = 0, tab = 0, comma = 0, cnt = 0;
  for (size_t i = lines.size() > 40 ? lines.size() - 40 : 0; i < lines.size(); ++i) {
    const std::string& l = lines[i];
    if (l.empty()) continue;
    ++cnt;
    semi += l.find(';') != std::string::npos;
    tab += l.find('\t') != std::string::npos;
    comma += l.find(',') != std::string::npos;
  }
  if (semi > cnt / 2) { f.sep = ';'; if (comma > cnt / 2) f.decimal = ','; }
  else if (tab > cnt / 2) { f.sep = '\t'; if (comma > cnt / 2) f.decimal = ','; }
  else if (comma > cnt / 2) f.sep = ',';
  else f.sep = ' ';
  // première ligne avec une date et un nombre (après la date)
  for (size_t i = 0; i < lines.size(); ++i) {
    const auto v = SplitFields(lines[i], f);
    if (v.empty() || lines[i][0] == '#' || lines[i][0] == '%') continue;
    int cd = 0, ct = 0, cz = 0;
    for (size_t c = 0; c < v.size(); ++c) {
      long long t;
      double d, sec;
      if (!cd && ParseDateTime(v[c], "", 0, t)) { cd = (int)c + 1; continue; }
      if (cd && !ct && !cz && v[c].find(':') != std::string::npos && ParseTimeOfDay(v[c].c_str(), sec)) {
        ct = (int)c + 1;
        continue;
      }
      if (cd && ParseFieldNumber(v[c], f.decimal, d)) cz = (int)c + 1;  // dernière valeur
    }
    if (cd && cz) {
      f.skipLines = (int)i;
      f.colDate = cd;
      f.colTime = ct;
      f.colZ = cz;
      return f;
    }
  }
  f.colDate = 1;
  f.colZ = 2;
  return f;
}

TextFormat GuessTextFormat(const std::vector<std::string>& lines) {
  TextFormat f;
  f.custom = true;
  // séparateur et décimale : d'après les dernières lignes (données)
  int semi = 0, tab = 0, comma = 0, cnt = 0;
  for (size_t i = lines.size() > 40 ? lines.size() - 40 : 0; i < lines.size(); ++i) {
    const std::string& l = lines[i];
    if (l.empty()) continue;
    ++cnt;
    semi += l.find(';') != std::string::npos;
    tab += l.find('\t') != std::string::npos;
    comma += l.find(',') != std::string::npos;
  }
  if (semi > cnt / 2) { f.sep = ';'; if (comma > cnt / 2) f.decimal = ','; }
  else if (tab > cnt / 2) { f.sep = '\t'; if (comma > cnt / 2) f.decimal = ','; }
  else if (comma > cnt / 2) f.sep = ',';
  else f.sep = ' ';
  // entête : lignes jusqu'à la première ligne de données (≥ 3 nombres), suivie
  // d'une autre ligne de même nombre de champs
  auto numeric = [&](const std::vector<std::string>& v) {
    int k = 0;
    for (const auto& s : v) { double d; k += ParseFieldNumber(s, f.decimal, d); }
    return k;
  };
  size_t first = 0;
  for (; first < lines.size(); ++first) {
    const auto v = SplitFields(lines[first], f);
    if (v.empty() || lines[first][0] == '#' || lines[first][0] == '%' || numeric(v) < 3) continue;
    if (first + 1 < lines.size() && SplitFields(lines[first + 1], f).size() != v.size()) continue;
    break;
  }
  if (first >= lines.size()) return f;
  f.skipLines = (int)first;
  // rôles des colonnes d'après une ligne de données, et l'entête si elle nomme les colonnes
  const auto v = SplitFields(lines[first], f);
  std::vector<std::string> names;
  for (size_t h = first; h-- > 0;) {
    std::string l = lines[h];
    while (!l.empty() && (l[0] == '#' || l[0] == '%' || l[0] == ' ')) l.erase(0, 1);
    if (l.empty()) continue;
    names = SplitFields(l, f);
    break;
  }
  auto lower = [](std::string s) { for (char& c : s) c = (char)std::tolower((unsigned char)c); return s; };
  std::vector<int> num;
  f.colX = f.colY = f.colZ = 0;
  for (size_t c = 0; c < v.size(); ++c) {
    double d;
    long long t;
    if (ParseFieldNumber(v[c], f.decimal, d)) { num.push_back((int)c + 1); continue; }
    if (!f.colDate && ParseDateTime(v[c], "", 0, t)) { f.colDate = (int)c + 1; continue; }
    double sec;
    if (!f.colTime && ParseTimeOfDay(v[c].c_str(), sec)) f.colTime = (int)c + 1;
  }
  // noms de colonnes de l'entête (si alignés sur les données)
  int latName = -1, lonName = -1;  // ordre lat / lon annoncé par l'entête
  for (size_t c = 0; c < names.size(); ++c) {
    const std::string n = lower(names[c]);
    if (latName < 0 && n.rfind("lat", 0) == 0) latName = (int)c;
    if (lonName < 0 && n.rfind("lon", 0) == 0) lonName = (int)c;
  }
  if (names.size() == v.size()) {
    for (size_t c = 0; c < names.size() && c < v.size(); ++c) {
      const std::string n = lower(names[c]);
      const int col = (int)c + 1;
      if (std::find(num.begin(), num.end(), col) == num.end()) continue;
      if (!f.colY && (n.rfind("lat", 0) == 0 || n.rfind("north", 0) == 0 || n == "y" || n.rfind("nord", 0) == 0)) f.colY = col;
      else if (!f.colX && (n.rfind("lon", 0) == 0 || n.rfind("east", 0) == 0 || n == "x" || n.rfind("est", 0) == 0)) f.colX = col;
      else if (!f.colZ && (n.rfind("depth", 0) == 0 || n.rfind("prof", 0) == 0 || n == "z" || n.rfind("height", 0) == 0 ||
                           n.rfind("elev", 0) == 0 || n.rfind("haut", 0) == 0 || n.rfind("alt", 0) == 0)) f.colZ = col;
    }
  }
  // sinon : trois premières colonnes numériques ; X/Y inversés si la
  // première est une latitude et la seconde une longitude hors ±90
  std::vector<int> rest;
  for (int c : num) if (c != f.colX && c != f.colY && c != f.colZ) rest.push_back(c);
  size_t r = 0;
  if (!f.colX && !f.colY && rest.size() >= 2) {
    double a, b;
    ParseFieldNumber(v[rest[0] - 1], f.decimal, a);
    ParseFieldNumber(v[rest[1] - 1], f.decimal, b);
    const bool latFirst = (latName >= 0 && lonName >= 0 && latName < lonName) ||
                          (std::fabs(a) <= 90 && std::fabs(b) > 90 && std::fabs(b) <= 180);
    if (latFirst) { f.colY = rest[0]; f.colX = rest[1]; }
    else { f.colX = rest[0]; f.colY = rest[1]; }
    r = 2;
  }
  for (; r < rest.size() && (!f.colX || !f.colY || !f.colZ); ++r) {
    if (!f.colX) f.colX = rest[r];
    else if (!f.colY) f.colY = rest[r];
    else if (!f.colZ) f.colZ = rest[r];
  }
  if (!f.colX) f.colX = 1;
  if (!f.colY) f.colY = 2;
  if (!f.colZ) f.colZ = 3;
  if (f.colTime && !f.colDate) f.colTime = 0;  // heure sans date : inutilisable
  return f;
}

namespace {
struct ParseState {
  const LoadOptions& o;
  PointCloud& pc;
  size_t outOfRange = 0, failed = 0;
  double sampleX = 0, sampleY = 0;
  bool haveSample = false;
  std::vector<int64_t> sx, sy;  // échantillon (mm) pour détecter un pas de grille
  std::vector<float> corr;      // hauteur d'eau de chaque sonde gardée (si o.water)
  long long waterNoTime = 0, waterMiss = 0;
  ParseState(const LoadOptions& oo, PointCloud& p) : o(oo), pc(p) {}

  void add(double a, double b, double z) {
    if (o.order == LoadOptions::Order::LatLonZ) std::swap(a, b);
    if (o.crs) {
      if (!haveSample) { sampleX = a; sampleY = b; haveSample = true; }
      if (sx.size() < 300000) {
        sx.push_back(std::llround(a * 1000.0));
        sy.push_back(std::llround(b * 1000.0));
      }
      double lon, lat;
      if (!o.crs->toWGS84(a, b, lon, lat)) { ++failed; return; }
      pc.add(lon, lat, (float)z);
      return;
    }
    if (a < -180 || a > 360 || b < -90 || b > 90) {  // entête, bruit… ou mauvais système
      if (std::fabs(a) > 1000 || std::fabs(b) > 1000) {
        if (!haveSample) { sampleX = a; sampleY = b; haveSample = true; }
        ++outOfRange;
      }
      return;
    }
    if (a > 180) a -= 360;
    pc.add(a, b, (float)z);
  }
  // Pas de grille de la source projetée (mètres)
  void finish() {
    if (!o.crs || o.crs->isGeographic()) return;
    const double ux = LatticeStepUnits(sx), uy = LatticeStepUnits(sy);
    if (ux <= 0 || uy <= 0) return;
    // Un semis irrégulier aux coordonnées arrondies (ex. Litto3D « PTS » au cm)
    // ressemble à une grille de pas = résolution d'arrondi : on exige qu'une
    // part raisonnable des nœuds de l'emprise soit occupée.
    const auto [x0, x1] = std::minmax_element(sx.begin(), sx.end());
    const auto [y0, y1] = std::minmax_element(sy.begin(), sy.end());
    const double nodes = ((double)(*x1 - *x0) / ux + 1) * ((double)(*y1 - *y0) / uy + 1);
    if ((double)sx.size() < 0.02 * nodes) return;
    pc.srcStepX = ux / 1000.0 * o.crs->unitToMeter();
    pc.srcStepY = uy / 1000.0 * o.crs->unitToMeter();
  }
  std::string emptyMessage() const {
    if (outOfRange && haveSample) {
      char b[160];
      std::snprintf(b, sizeof b, pd::tr("Coordonnées hors plage lon/lat (ex. %.2f %.2f) : "), sampleX, sampleY);
      return std::string(b) + GuessCrsHint(sampleX, sampleY);
    }
    if (failed) return pd::tr("Conversion impossible : vérifier le système de coordonnées");
    return pd::tr("Aucune sonde lisible (format attendu : X Y Z)");
  }
};
}  // namespace

static void parseLine(const char* p, const char* e, ParseState& st) {
  double a, b, c;
  if (!parseNum(p, e, a) || !parseNum(p, e, b) || !parseNum(p, e, c)) return;
  st.add(a, b, c);
}

// Ligne d'un format personnalisé : colonnes X, Y, Z (et date/heure) choisies
static void parseLineCustom(const char* p, const char* e, ParseState& st, long long& tMin,
                            long long& tMax) {
  const TextFormat& f = st.o.text;
  if (p < e && (*p == '#' || *p == '%')) return;  // commentaire
  std::string line(p, e);
  const std::vector<std::string> v = SplitFields(line, f);
  const int nf = (int)v.size();
  if (f.colX < 1 || f.colY < 1 || f.colZ < 1 || f.colX > nf || f.colY > nf || f.colZ > nf) return;
  double x, y, z;
  if (!ParseFieldCoord(v[f.colX - 1], f, false, x) || !ParseFieldCoord(v[f.colY - 1], f, true, y) ||
      !ParseFieldNumber(v[f.colZ - 1], f.decimal, z))
    return;
  bool hasT = false;
  long long t = 0;
  if (f.colDate && f.colDate <= nf) {
    const std::string& tm = (f.colTime && f.colTime <= nf) ? v[f.colTime - 1] : std::string();
    if (ParseFieldDateTime(v[f.colDate - 1], tm, f, t)) {
      hasT = true;
      if (!tMin || t < tMin) tMin = t;
      if (t > tMax) tMax = t;
    }
  }
  if (!st.o.water) { st.add(x, y, z); return; }
  // hauteur d'eau observée à l'heure de la sonde
  float h;
  if (!hasT) { ++st.waterNoTime; return; }
  if (!st.o.water->at(t, h)) { ++st.waterMiss; return; }
  const size_t n0 = st.pc.size();
  st.add(x, y, z);
  if (st.pc.size() > n0) st.corr.push_back(h);
}

bool LoadXYZ(const std::string& path, const LoadOptions& optIn, PointCloud& pc,
             std::string& err, const ProgressFn& progress,
             const std::atomic<bool>* cancel, const XYZBlockSink* sink, size_t blockN) {
  LoadOptions opt = optIn;
  const bool custom = opt.text.custom;
  if (custom) opt.order = LoadOptions::Order::LonLatZ;  // colonnes X / Y explicites
  long long lineNo = 0, tMin = 0, tMax = 0;
  FILE* f = OpenFile(path, "rb");
  if (!f) { err = pd::tr("Impossible d'ouvrir ") + path; return false; }
  std::fseek(f, 0, SEEK_END);
  long long total = std::ftell(f);
  std::fseek(f, 0, SEEK_SET);
  pc.clear();
  if (sink && blockN < 1024) blockN = 1024;
  if (sink) pc.reserve(blockN + (blockN >> 3));
  else pc.reserve((size_t)std::max<long long>(1024, total / 28));  // ~28 o/ligne

  const size_t CH = (sink ? 4u : 16u) << 20;
  std::vector<char> buf(CH + 4096);
  size_t carry = 0; long long done = 0;
  ParseState st(opt, pc);
  auto valid = [&](float v) {
    return std::isfinite(v) && std::fabs(v) < 1e20f && !IsUserNoData(v, opt.nodata);
  };
  // signe de Z (mode automatique) : décidé une fois (premier bloc) puis imposé
  auto decideSign = [&]() {
    if (opt.zsign != LoadOptions::ZSign::Auto) return;
    size_t neg = 0, nv = 0;
    for (float v : pc.z) if (valid(v)) { ++nv; neg += (v < 0.f); }
    if (!nv) return;
    opt.zsign = neg > (nv * 6) / 10 ? LoadOptions::ZSign::Elevation : LoadOptions::ZSign::DepthPositive;
  };
  float cmin = 1e30f, cmax = -1e30f;
  bool corrected = false;
  // hauteurs d'eau observées : profondeur = k·z + décalage ; on veut profondeur − s·h
  auto applyWater = [&]() {
    if (!opt.water || st.corr.size() != pc.size()) return;
    decideSign();  // la correction ne doit pas faire basculer la détection automatique
    const bool negate = opt.zsign == LoadOptions::ZSign::Elevation;
    const float s = (opt.waterSubtract ? 1.f : -1.f) / (float)opt.zToMeters, sg = negate ? -1.f : 1.f;
    for (size_t i = 0; i < pc.z.size(); ++i) {
      if (!valid(pc.z[i])) continue;
      pc.z[i] -= s * st.corr[i] * sg;
      cmin = std::min(cmin, -s * st.corr[i]);
      cmax = std::max(cmax, -s * st.corr[i]);
    }
    corrected = true;
  };
  // lecture par blocs : chaque bloc complet est converti puis remis à `sink`
  bool finished = false, stopByUser = false;
  size_t kept = 0;
  auto flush = [&]() -> bool {
    if (!finished) { st.finish(); finished = true; }  // pas de grille : échantillon du début
    applyWater();
    decideSign();
    pc.applySign(opt);
    pc.tMin = tMin;
    pc.tMax = tMax;
    pc.waterApplied = corrected;
    pc.waterNoTime = st.waterNoTime;
    pc.waterMiss = st.waterMiss;
    pc.waterCorrMin = cmin;
    pc.waterCorrMax = cmax;
    kept += pc.size();
    const bool more = pc.empty() ? true : (*sink)(pc);
    pc.qlon.clear(); pc.qlat.clear(); pc.z.clear();  // capacité gardée
    st.corr.clear();
    return more;
  };
  for (;;) {
    size_t n = std::fread(buf.data() + carry, 1, CH, f);
    size_t len = carry + n;
    if (len == 0) break;
    const char* s = buf.data();
    const char* end = s + len;
    const char* lastNL = (n == 0) ? end : nullptr;
    if (!lastNL)
      for (const char* q = end - 1; q >= s; --q) if (*q == '\n') { lastNL = q + 1; break; }
    if (!lastNL) lastNL = end;  // ligne > 16 Mo : on la traite telle quelle
    const char* p = s;
    while (p < lastNL) {
      const char* nl = (const char*)std::memchr(p, '\n', lastNL - p);
      const char* le = nl ? nl : lastNL;
      ++lineNo;
      if (!custom) parseLine(p, le, st);
      else if (lineNo > opt.text.skipLines) parseLineCustom(p, le, st, tMin, tMax);
      p = nl ? nl + 1 : lastNL;
    }
    carry = end - lastNL;
    std::memmove(buf.data(), lastNL, carry);
    done += n;
    if (progress && total > 0) progress((float)done / (float)total);
    if (cancel && *cancel) { std::fclose(f); err = "Annulé"; return false; }
    if (sink && pc.size() >= blockN && !flush()) { stopByUser = true; break; }
    if (n == 0) break;
  }
  std::fclose(f);
  if (sink) {
    if (!stopByUser && !pc.empty()) flush();
    if (cancel && *cancel) { err = "Annulé"; return false; }
  }
  if (sink ? kept == 0 : pc.empty()) {
    if (opt.water && (st.waterNoTime || st.waterMiss)) {
      char b[300];
      std::snprintf(b, sizeof b,
                    pd::tr("Aucune sonde corrigée par les hauteurs d'eau : %lld sans date lisible, "
                           "%lld hors de la période des hauteurs (%s … %s)"),
                    st.waterNoTime, st.waterMiss, FormatTime(opt.water->tMin()).c_str(),
                    FormatTime(opt.water->tMax()).c_str());
      err = b;
    } else {
      err = st.emptyMessage();
    }
    return false;
  }
  if (sink) return true;
  st.finish();
  pc.waterApplied = false;
  pc.waterNoTime = st.waterNoTime;
  pc.waterMiss = st.waterMiss;
  applyWater();
  if (corrected) {
    pc.waterApplied = true;
    pc.waterCorrMin = cmin;
    pc.waterCorrMax = cmax;
  }
  pc.applySign(opt);
  pc.tMin = tMin;
  pc.tMax = tMax;
  return true;
}

// --------------------------------------------------------------- ESRI ASCII
// Valeur absente : égalité à la précision d'écriture près (« -3.40282e+38 »
// contre « -3.4028234663852886e+38 » dans l'entête), ou valeur non finie.
static bool NoData(double v, double nodata) {
  if (!std::isfinite(v) || std::fabs(v) >= 1e20) return true;
  return std::fabs(v - nodata) <= 1e-6 * std::max(1.0, std::fabs(nodata));
}

bool LoadESRIAscii(const std::string& path, const LoadOptions& opt,
                   PointCloud& pc, double& cellLon, double& cellLat,
                   std::string& err, const std::atomic<bool>* cancel) {
  std::ifstream in(U8Path(path));
  if (!in) { err = pd::tr("Impossible d'ouvrir ") + path; return false; }
  long ncols = 0, nrows = 0; double xll = 0, yll = 0, cs = 0, dx = 0, dy = 0;
  double nodata = -9999; bool center = false;
  std::string key;
  for (int k = 0; k < 7; ++k) {
    std::streampos pos = in.tellg();
    if (!(in >> key)) break;
    std::string lk = key;
    std::transform(lk.begin(), lk.end(), lk.begin(), ::tolower);
    if (lk == "ncols") in >> ncols;
    else if (lk == "nrows") in >> nrows;
    else if (lk == "xllcorner") in >> xll;
    else if (lk == "xllcenter") { in >> xll; center = true; }
    else if (lk == "yllcorner") in >> yll;
    else if (lk == "yllcenter") { in >> yll; center = true; }
    else if (lk == "cellsize") in >> cs;
    else if (lk == "dx") in >> dx;
    else if (lk == "dy") in >> dy;
    else if (lk == "nodata_value") in >> nodata;
    else { in.seekg(pos); break; }
  }
  if (cs > 0) dx = dy = cs;
  if (ncols <= 0 || nrows <= 0 || dx <= 0 || dy <= 0) { err = pd::tr("Entête ASC invalide"); return false; }
  if (center) { xll -= dx / 2; yll -= dy / 2; }
  pc.clear();
  pc.reserve((size_t)ncols * nrows);
  // corps lu par blocs avec l'analyseur rapide (ifstream >> double est très
  // lent, surtout sous MSVC) ; un jeton illisible (nan…) vaut NODATA
  const long long total = (long long)ncols * nrows;
  long long idx = 0;
  const size_t kChunk = (size_t)8 << 20;
  std::vector<char> buf;
  std::string carry;
  bool last = false;
  double lat = 0;
  // grille projetée : conversion exacte d'un nœud sur kStep le long de la
  // ligne, interpolation linéaire entre les deux (écart < 1 mm sur 32 cellules)
  const int kStep = 32;
  std::vector<double> nlon, nlat;
  std::vector<uint8_t> nok;
  auto rowNodes = [&](double y) {
    const long nn = (ncols - 1) / kStep + 2;
    nlon.assign(nn, 0); nlat.assign(nn, 0); nok.assign(nn, 0);
    for (long k = 0; k < nn; ++k) {
      const long c = std::min<long>(k * kStep, ncols - 1);
      nok[k] = opt.crs->toWGS84(xll + (c + 0.5) * dx, y, nlon[k], nlat[k]) ? 1 : 0;
    }
  };
  while (idx < total && !last) {
    buf.assign(carry.begin(), carry.end());
    carry.clear();
    const size_t old = buf.size();
    buf.resize(old + kChunk);
    in.read(buf.data() + old, (std::streamsize)kChunk);
    const size_t got = (size_t)in.gcount();
    buf.resize(old + got);
    last = got < kChunk;
    size_t end = buf.size();
    if (!last) {  // jeton coupé en fin de bloc : reporté au bloc suivant
      while (end > 0 && !(isSep(buf[end - 1]) || buf[end - 1] == '\n')) --end;
      carry.assign(buf.begin() + (std::ptrdiff_t)end, buf.end());
    }
    const char* p = buf.data();
    const char* e = buf.data() + end;
    while (idx < total) {
      while (p < e && (isSep(*p) || *p == '\n')) ++p;
      if (p >= e) break;
      const char* s0 = p;
      double v;
      if (!parseNum(p, e, v)) {
        p = s0;
        while (p < e && !(isSep(*p) || *p == '\n')) ++p;
        v = NAN;
      }
      const long r = (long)(idx / ncols), c = (long)(idx % ncols);
      ++idx;
      if (c == 0) {
        if (cancel && *cancel && (r & 63) == 0) { err = "Annulé"; return false; }
        lat = yll + (nrows - 1 - r + 0.5) * dy;
        if (opt.crs) rowNodes(lat);
      }
      if (NoData(v, nodata)) continue;
      const double x = xll + (c + 0.5) * dx;
      if (opt.crs) {
        const long k = c / kStep;
        const long c0 = k * kStep, c1 = std::min<long>((k + 1) * kStep, ncols - 1);
        double lo, la;
        if (c == c0 && nok[k]) {
          pc.add(nlon[k], nlat[k], (float)v);
        } else if (nok[k] && nok[k + 1] && c1 > c0) {
          const double t = double(c - c0) / double(c1 - c0);
          pc.add(nlon[k] + t * (nlon[k + 1] - nlon[k]), nlat[k] + t * (nlat[k + 1] - nlat[k]), (float)v);
        } else if (opt.crs->toWGS84(x, lat, lo, la)) {  // bord du domaine : conversion exacte
          pc.add(lo, la, (float)v);
        }
      } else {
        pc.add(x, lat, (float)v);
      }
    }
  }
  if (idx < total) { err = pd::tr("Fichier ASC tronqué"); return false; }
  if (pc.empty()) {
    err = (!opt.crs && (std::fabs(xll) > 1000 || std::fabs(yll) > 1000))
              ? pd::tr("Grille hors plage lon/lat : ") + GuessCrsHint(xll, yll)
              : pd::tr("Grille vide");
    return false;
  }
  cellLon = dx; cellLat = dy;
  if (opt.crs && !opt.crs->isGeographic()) {
    pc.srcStepX = dx * opt.crs->unitToMeter();
    pc.srcStepY = dy * opt.crs->unitToMeter();
  }
  pc.applySign(opt);
  return true;
}

// ------------------------------------------------------------- LandXML TIN
static bool nextTag(const std::string& s, size_t& pos, const char* tag,
                    std::string& attrs, std::string& body) {
  std::string open = std::string("<") + tag;
  for (;;) {
    size_t a = s.find(open, pos);
    if (a == std::string::npos) return false;
    char c = s[a + open.size()];
    if (c != ' ' && c != '>' && c != '\t') { pos = a + 1; continue; }
    size_t gt = s.find('>', a);
    if (gt == std::string::npos) return false;
    attrs = s.substr(a + open.size(), gt - a - open.size());
    size_t close = s.find(std::string("</") + tag + ">", gt);
    if (close == std::string::npos) return false;
    body = s.substr(gt + 1, close - gt - 1);
    pos = close;
    return true;
  }
}

bool LoadLandXMLTin(const std::string& path, const LoadOptions& opt,
                    PointCloud& pc, std::vector<uint32_t>& tris,
                    std::string& err) {
  std::ifstream in(U8Path(path), std::ios::binary);
  if (!in) { err = pd::tr("Impossible d'ouvrir ") + path; return false; }
  std::stringstream ss; ss << in.rdbuf();
  const std::string s = ss.str();
  std::unordered_map<long, uint32_t> idmap;
  pc.clear(); tris.clear();
  std::string attrs, body;
  size_t pos = 0;
  while (nextTag(s, pos, "P", attrs, body)) {
    size_t q = attrs.find("id=\"");
    long id = (q != std::string::npos) ? std::strtol(attrs.c_str() + q + 4, nullptr, 10)
                                       : (long)pc.size() + 1;
    // LandXML : "Nord Est Élévation" -> lat lon z
    std::istringstream b(body); double la, lo, zz;
    if (!(b >> la >> lo >> zz)) continue;
    if (opt.crs) {  // N E projetés -> WGS84
      double x = lo, y = la;
      if (!opt.crs->toWGS84(x, y, lo, la)) continue;
    }
    idmap[id] = (uint32_t)pc.size();
    pc.add(lo, la, (float)zz);
  }
  pos = 0;
  while (nextTag(s, pos, "F", attrs, body)) {
    std::istringstream b(body); long i, j, k;
    if (!(b >> i >> j >> k)) continue;
    auto I = idmap.find(i), J = idmap.find(j), K = idmap.find(k);
    if (I == idmap.end() || J == idmap.end() || K == idmap.end()) continue;
    tris.push_back(I->second); tris.push_back(J->second); tris.push_back(K->second);
  }
  if (pc.empty()) { err = pd::tr("Aucun point <P> trouvé"); return false; }
  // LandXML : élévations -> en Auto, convertit en profondeurs si majoritairement négatives
  pc.applySign(opt, &tris);
  return true;
}

#ifdef PD_HAVE_GDAL
bool LoadGdalRaster(const std::string& path, const LoadOptions& opt,
                    PointCloud& pc, double& cellLon, double& cellLat,
                    std::string& err) {
  GDALAllRegister();
  GDALDataset* ds = (GDALDataset*)GDALOpen(path.c_str(), GA_ReadOnly);
  if (!ds) { err = pd::tr("GDAL ne peut pas ouvrir ") + path; return false; }
  double gt[6];
  if (ds->GetGeoTransform(gt) != CE_None || gt[2] != 0 || gt[4] != 0) {
    GDALClose(ds); err = pd::tr("Géoréférencement absent ou tourné"); return false;
  }
  GDALRasterBand* band = ds->GetRasterBand(1);
  int okNd = 0; double nd = band->GetNoDataValue(&okNd);
  int nx = ds->GetRasterXSize(), ny = ds->GetRasterYSize();
  std::vector<float> row(nx);
  pc.clear(); pc.reserve((size_t)nx * ny / 2);
  for (int j = 0; j < ny; ++j) {
    if (band->RasterIO(GF_Read, 0, j, nx, 1, row.data(), nx, 1, GDT_Float32, 0, 0) != CE_None) break;
    for (int i = 0; i < nx; ++i) {
      float v = row[i];
      if ((okNd && v == (float)nd) || std::isnan(v) || v > 1e30f) continue;
      pc.add(gt[0] + (i + 0.5) * gt[1], gt[3] + (j + 0.5) * gt[5], v);
    }
  }
  cellLon = std::fabs(gt[1]); cellLat = std::fabs(gt[5]);
  GDALClose(ds);
  if (pc.empty()) { err = pd::tr("Raster vide"); return false; }
  pc.applySign(opt);
  return true;
}
#endif

}  // namespace pd

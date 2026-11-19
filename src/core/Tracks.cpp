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

#include "Tracks.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <map>

#include "FileUtil.h"
#include "Lang.h"
#include "SideScan.h"
#include "Triangulate.h"
#include "WaterLevel.h"

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

namespace pd {

static std::string LowerExt(const std::string& p) {
  const size_t d = p.find_last_of('.');
  const size_t s = p.find_last_of("/\\");
  if (d == std::string::npos || (s != std::string::npos && d < s)) return std::string();
  std::string e = p.substr(d);
  for (char& c : e) c = (char)std::tolower((unsigned char)c);
  return e;
}

bool IsTrackFile(const std::string& path) {
  const std::string e = LowerExt(path);
  return e == ".gpx" || e == ".sl2" || e == ".sl3";
}

// ------------------------------------------------------------------- GPX

static bool ReadWhole(const std::string& path, std::string& s, std::string& err) {
  FILE* f = OpenFile(path, "rb");
  if (!f) { err = pd::tr("Impossible d'ouvrir ") + path; return false; }
  std::fseek(f, 0, SEEK_END);
  const long long n = std::ftell(f);
  std::fseek(f, 0, SEEK_SET);
  if (n < 0) { std::fclose(f); err = pd::tr("Impossible d'ouvrir ") + path; return false; }
  s.resize((size_t)n);
  const bool ok = n == 0 || std::fread(&s[0], 1, (size_t)n, f) == (size_t)n;
  std::fclose(f);
  if (!ok) err = pd::tr("Lecture impossible : ") + path;
  return ok;
}

// nom local (sans préfixe d'espace de noms), en minuscules
static std::string LocalName(const char* p, const char* e) {
  const char* b = p;
  for (const char* q = p; q < e; ++q)
    if (*q == ':') b = q + 1;
  std::string n(b, e);
  for (char& c : n) c = (char)std::tolower((unsigned char)c);
  return n;
}

// valeur de l'attribut `name` dans l'entête d'élément [p, e)
static bool Attr(const char* p, const char* e, const char* name, double& v) {
  const size_t L = std::strlen(name);
  for (const char* q = p; q + L + 2 < e; ++q) {
    if (std::strncmp(q, name, L) != 0) continue;
    if (q > p && !std::isspace((unsigned char)q[-1])) continue;
    const char* r = q + L;
    while (r < e && std::isspace((unsigned char)*r)) ++r;
    if (r >= e || *r != '=') continue;
    ++r;
    while (r < e && std::isspace((unsigned char)*r)) ++r;
    if (r >= e || (*r != '"' && *r != '\'')) continue;
    const char quote = *r++;
    const char* z = (const char*)std::memchr(r, quote, e - r);
    if (!z) return false;
    std::string txt(r, z);
    char* end = nullptr;
    v = std::strtod(txt.c_str(), &end);
    return end != txt.c_str();
  }
  return false;
}

bool ReadGpxSamples(const std::string& path, std::vector<TrackSample>& out, TrackInfo& info,
                    std::string& err) {
  info = TrackInfo();
  info.kind = "GPX";
  std::string s;
  if (!ReadWhole(path, s, err)) return false;
  const char* p = s.data();
  const char* end = p + s.size();
  while (p < end) {
    const char* lt = (const char*)std::memchr(p, '<', end - p);
    if (!lt) break;
    const char* gt = (const char*)std::memchr(lt, '>', end - lt);
    if (!gt) break;
    p = gt + 1;
    if (lt[1] == '/' || lt[1] == '?' || lt[1] == '!') continue;
    const char* ne = lt + 1;
    while (ne < gt && !std::isspace((unsigned char)*ne) && *ne != '/') ++ne;
    const std::string name = LocalName(lt + 1, ne);
    if (name != "trkpt" && name != "rtept" && name != "wpt") continue;
    ++info.records;
    TrackSample t{0, 0, 0.f, 0, false};
    if (!Attr(ne, gt, "lat", t.lat) || !Attr(ne, gt, "lon", t.lon) || std::fabs(t.lat) > 90 ||
        std::fabs(t.lon) > 360) {
      ++info.noDepth;
      continue;
    }
    if (t.lon > 180) t.lon -= 360;
    if (gt[-1] == '/') { ++info.noDepth; continue; }  // <trkpt …/> : rien dedans
    // corps de l'élément jusqu'à sa balise fermante
    const std::string close = "</" + std::string(lt + 1, ne) + ">";
    const char* be = std::search(p, end, close.begin(), close.end());
    bool haveDepth = false;
    for (const char* q = p; q < be;) {
      const char* a = (const char*)std::memchr(q, '<', be - q);
      if (!a) break;
      const char* b = (const char*)std::memchr(a, '>', be - a);
      if (!b) break;
      q = b + 1;
      if (a[1] == '/' || b[-1] == '/') continue;
      const char* ae = a + 1;
      while (ae < b && !std::isspace((unsigned char)*ae)) ++ae;
      const std::string n = LocalName(a + 1, ae);
      const char* te = (const char*)std::memchr(q, '<', be - q);
      if (!te) te = be;
      std::string txt(q, te);
      while (!txt.empty() && std::isspace((unsigned char)txt.back())) txt.pop_back();
      size_t k = 0;
      while (k < txt.size() && std::isspace((unsigned char)txt[k])) ++k;
      txt.erase(0, k);
      if (n == "time" && !t.hasT) {
        long long tt;
        if (ParseDateTime(txt, std::string(), 0, tt)) { t.t = tt; t.hasT = true; }
      } else if (n == "depth" && !haveDepth) {
        char* e2 = nullptr;
        const double d = std::strtod(txt.c_str(), &e2);
        if (e2 != txt.c_str() && std::isfinite(d)) { t.depth = (float)d; haveDepth = true; }
      }
    }
    p = be < end ? be + close.size() : end;
    if (!haveDepth || !(t.depth > 0.f)) { ++info.noDepth; continue; }
    out.push_back(t);
  }
  if (!info.records) { err = pd::tr("Fichier GPX sans point (trkpt, rtept, wpt)"); return false; }
  return true;
}

// ---------------------------------------------------- journaux sonar SL2 / SL3
// Entête de fichier : format (2 = SL2, 3 = SL3), version, taille de bloc (u16).
// Trames : entête fixe puis image sonar ; champs petit-boutistes.
//                         SL2   SL3
//   position de la trame    0     0   (u32, contrôle)
//   taille de la trame     28     8   (u16)
//   canal                  32    12   (u16 : 0 primaire, 1 secondaire, 2 DownScan…)
//   date (1re trame)       60    40   (u32, secondes UTC ; ensuite : ms)
//   profondeur             64    48   (f32, pieds ; 0 : fond non trouvé)
//   Est, Nord              108   92   (i32, Mercator sphérique Lowrance, m)
//   heure relative         140  124   (i32, ms depuis l'ouverture)
namespace {
struct SlLayout {
  int size, chan, date, depth, east, time, header;
};
constexpr SlLayout kSl2{28, 32, 60, 64, 108, 140, 144};
constexpr SlLayout kSl3{8, 12, 40, 48, 92, 124, 128};
constexpr double kLowranceR = 6356752.3142;  // rayon polaire WGS84 (Mercator Lowrance)

template <class T>
T Get(const uint8_t* p, int off) {
  T v;
  std::memcpy(&v, p + off, sizeof v);
  return v;
}

struct RawPing {
  int32_t e, n;
  float depthFt;
  int32_t ms;
};
}  // namespace

bool ReadSonarLogSamples(const std::string& path, std::vector<TrackSample>& out, TrackInfo& info,
                         std::string& err, const ProgressFn& progress,
                         const std::atomic<bool>* cancel) {
  info = TrackInfo();
  FILE* f = OpenFile(path, "rb");
  if (!f) { err = pd::tr("Impossible d'ouvrir ") + path; return false; }
  std::error_code ec;
  const long long total = (long long)std::filesystem::file_size(U8Path(path), ec);
  uint8_t fh[8];
  if (std::fread(fh, 1, 8, f) != 8 || (Get<uint16_t>(fh, 0) != 2 && Get<uint16_t>(fh, 0) != 3)) {
    std::fclose(f);
    err = pd::tr("Journal sonar non reconnu (formats SL2 et SL3)");
    return false;
  }
  const bool sl3 = Get<uint16_t>(fh, 0) == 3;
  const SlLayout L = sl3 ? kSl3 : kSl2;
  info.kind = sl3 ? "SL3" : "SL2";
  // lecture séquentielle par grands blocs (cartes SD : pas de petits accès)
  const size_t BUF = 8u << 20;
  std::vector<uint8_t> buf(BUF);
  long long base = 8, have = 0;  // buf[0..have) = fichier[base..base+have)
  auto window = [&](long long pos, size_t need) -> const uint8_t* {
    if (pos < base || pos + (long long)need > base + have) {
#if defined(_WIN32)
      _fseeki64(f, pos, SEEK_SET);
#else
      fseeko(f, (off_t)pos, SEEK_SET);
#endif
      base = pos;
      have = (long long)std::fread(buf.data(), 1, BUF, f);
      if (have < (long long)need) return nullptr;
    }
    return buf.data() + (pos - base);
  };
  std::map<int, std::vector<RawPing>> byChan;  // canaux candidats
  long long t0 = 0;
  long long pos = 8, frames = 0;
  bool stop = false;
  while (pos + L.header <= total) {
    const uint8_t* h = window(pos, (size_t)L.header);
    if (!h) break;
    if (Get<uint32_t>(h, 0) != (uint32_t)pos) break;  // fin tronquée ou format inattendu
    const uint16_t size = Get<uint16_t>(h, L.size);
    if (size < L.header) break;
    const int ch = Get<uint16_t>(h, L.chan);
    if (frames == 0) t0 = Get<uint32_t>(h, L.date);
    ++frames;
    if (ch == 0 || ch == 1 || ch == 2) {
      RawPing r{Get<int32_t>(h, L.east), Get<int32_t>(h, L.east + 4), Get<float>(h, L.depth),
                Get<int32_t>(h, L.time)};
      byChan[ch].push_back(r);
    }
    pos += size;
    if ((frames & 4095) == 0) {
      if (progress && total > 0) progress((float)pos / (float)total);
      if (cancel && *cancel) { stop = true; break; }
    }
  }
  std::fclose(f);
  info.records = frames;
  if (stop) { err = "Annulé"; return false; }
  if (!frames) { err = pd::tr("Journal sonar non reconnu (formats SL2 et SL3)"); return false; }
  const std::vector<RawPing>* pings = nullptr;
  for (int ch : {0, 2, 1}) {
    auto it = byChan.find(ch);
    if (it != byChan.end() && !it->second.empty()) { pings = &it->second; info.channel = ch; break; }
  }
  if (!pings) { err = pd::tr("Journal sonar sans canal de profondeur (primaire, DownScan, secondaire)"); return false; }
  info.pings = (long long)pings->size();
  // pings d'une même position GPS (récepteur à 1–10 Hz, sondeur à 10–20 Hz) :
  // médiane des profondeurs valides, heure du milieu
  std::vector<float> d;
  const auto& P = *pings;
  for (size_t i = 0; i < P.size();) {
    size_t j = i;
    d.clear();
    while (j < P.size() && P[j].e == P[i].e && P[j].n == P[i].n) {
      const float v = P[j].depthFt;
      if (std::isfinite(v) && v > 0.f && v < 40000.f) d.push_back(v);
      else ++info.noDepth;
      ++j;
    }
    if (!d.empty() && !(P[i].e == 0 && P[i].n == 0)) {
      std::nth_element(d.begin(), d.begin() + d.size() / 2, d.end());
      TrackSample t;
      t.lon = P[i].e / kLowranceR * 180.0 / M_PI;
      t.lat = (2.0 * std::atan(std::exp(P[i].n / kLowranceR)) - M_PI / 2) * 180.0 / M_PI;
      t.depth = (float)(d[d.size() / 2] * 0.3048);
      t.hasT = t0 > 0;
      t.t = t.hasT ? t0 + (long long)std::llround(P[(i + j - 1) / 2].ms / 1000.0) : 0;
      out.push_back(t);
    }
    i = j;
  }
  return true;
}

// ------------------------------------------------------------------ nuage

namespace {
constexpr double kDegM = 111194.92664;
}

bool EstimateTrackOffset(const std::vector<double>& x, const std::vector<double>& y, const std::vector<float>& z,
                         const std::vector<double>& hdg, double& offFwd, double& quality,
                         std::vector<double>* costOut) {
  quality = 0;
  const size_t n = x.size();
  // axe du levé (moyenne axiale des caps) et classes de sens
  double C = 0, S = 0;
  for (size_t i = 0; i < n; ++i)
    if (std::isfinite(hdg[i])) { C += std::cos(2 * hdg[i]); S += std::sin(2 * hdg[i]); }
  const double axis = 0.5 * std::atan2(S, C);
  const double fx = std::sin(axis), fy = std::cos(axis);
  std::vector<size_t> A, B;
  for (size_t i = 0; i < n; ++i) {
    if (!std::isfinite(hdg[i])) continue;
    const double c = std::cos(hdg[i] - axis);
    if (c > 0.8) A.push_back(i);
    else if (c < -0.8) B.push_back(i);
  }
  if (A.size() < 100 || B.size() < 100) return false;
  // surface des lignes B : grille (pas 1 m, élargi), comblée entre les
  // lignes par triangulation (triangles de moins de 60 m de côté)
  double x0 = 1e18, x1 = -1e18, y0 = 1e18, y1 = -1e18;
  for (size_t i : B) { x0 = std::min(x0, x[i]); x1 = std::max(x1, x[i]); y0 = std::min(y0, y[i]); y1 = std::max(y1, y[i]); }
  const double fillR = 60.0;  // lignes B : une sur deux, soit deux écarts entre lignes
  x0 -= fillR; y0 -= fillR; x1 += fillR; y1 += fillR;
  const double c = std::max(1.0, std::sqrt((x1 - x0) * (y1 - y0) / 4e6));
  const int W = (int)std::ceil((x1 - x0) / c) + 1, H = (int)std::ceil((y1 - y0) / c) + 1;
  std::vector<double> sz((size_t)W * H, 0), sn((size_t)W * H, 0);
  for (size_t i : B) {
    const int gx = (int)((x[i] - x0) / c), gy = (int)((y[i] - y0) / c);
    sz[(size_t)gy * W + gx] += z[i];
    sn[(size_t)gy * W + gx] += 1;
  }
  std::vector<float> G((size_t)W * H, NAN);
  std::vector<size_t> filled;
  for (size_t k = 0; k < G.size(); ++k)
    if (sn[k] > 0) { G[k] = (float)(sz[k] / sn[k]); filled.push_back(k); }
  {  // comblement entre les lignes : triangulation (TIN) des cellules
    // renseignées, interpolation linéaire dans les triangles de côté < fillR
    std::vector<double> xy;
    xy.reserve(filled.size() * 2);
    for (size_t k : filled) { xy.push_back((double)(k % W) + 0.5); xy.push_back((double)(k / W) + 0.5); }
    const std::vector<size_t> tri = filled.size() >= 3 ? Triangulate(xy) : std::vector<size_t>();
    const double maxE = fillR / c;
    std::vector<float> G2 = G;
    for (size_t t = 0; t + 2 < tri.size(); t += 3) {
      const size_t a0 = tri[t], a1 = tri[t + 1], a2 = tri[t + 2];
      const double ax = xy[2 * a0], ay = xy[2 * a0 + 1], bx = xy[2 * a1], by = xy[2 * a1 + 1], cx = xy[2 * a2],
                   cy = xy[2 * a2 + 1];
      if (std::hypot(ax - bx, ay - by) > maxE || std::hypot(bx - cx, by - cy) > maxE ||
          std::hypot(cx - ax, cy - ay) > maxE)
        continue;
      const double den = (by - cy) * (ax - cx) + (cx - bx) * (ay - cy);
      if (std::fabs(den) < 1e-12) continue;
      const float za = G[filled[a0]], zb = G[filled[a1]], zc = G[filled[a2]];
      const int gx0 = std::max(0, (int)std::floor(std::min({ax, bx, cx}))), gx1 = std::min(W - 1, (int)std::ceil(std::max({ax, bx, cx})));
      const int gy0 = std::max(0, (int)std::floor(std::min({ay, by, cy}))), gy1 = std::min(H - 1, (int)std::ceil(std::max({ay, by, cy})));
      for (int gy = gy0; gy <= gy1; ++gy)
        for (int gx = gx0; gx <= gx1; ++gx) {
          const double px = gx + 0.5, py = gy + 0.5;
          const double l1 = ((by - cy) * (px - cx) + (cx - bx) * (py - cy)) / den;
          const double l2 = ((cy - ay) * (px - cx) + (ax - cx) * (py - cy)) / den;
          const double l3 = 1 - l1 - l2;
          if (l1 < -1e-9 || l2 < -1e-9 || l3 < -1e-9) continue;
          float& g = G2[(size_t)gy * W + gx];
          if (std::isnan(g)) g = (float)(l1 * za + l2 * zb + l3 * zc);
        }
    }
    G.swap(G2);
  }
  auto at = [&](double px, double py) -> double {  // bilinéaire
    const double u = (px - x0) / c - 0.5, v = (py - y0) / c - 0.5;
    const int i = (int)std::floor(u), j = (int)std::floor(v);
    if (i < 0 || j < 0 || i + 1 >= W || j + 1 >= H) return NAN;
    const double a = u - i, b = v - j;
    const float g00 = G[(size_t)j * W + i], g10 = G[(size_t)j * W + i + 1], g01 = G[(size_t)(j + 1) * W + i],
                g11 = G[(size_t)(j + 1) * W + i + 1];
    if (std::isnan(g00) || std::isnan(g10) || std::isnan(g01) || std::isnan(g11)) return NAN;
    return (1 - a) * (1 - b) * g00 + a * (1 - b) * g10 + (1 - a) * b * g01 + a * b * g11;
  };
  // coût : dispersion des écarts A − B (écart moyen retiré : marée, niveau)
  // pour un recalage d le long de l'axe ; seules les sondes A couvertes
  // pour tous les recalages comptent (même échantillon partout)
  const double Dmax = 20, st = 0.5;
  const int nd = (int)(2 * Dmax / st) + 1;
  std::vector<size_t> use;
  for (size_t i : A) {
    if (std::isnan(at(x[i] - Dmax * fx, y[i] - Dmax * fy)) || std::isnan(at(x[i] + Dmax * fx, y[i] + Dmax * fy)) ||
        std::isnan(at(x[i], y[i])))
      continue;
    use.push_back(i);
  }
  if (use.size() < 200) return false;
  std::vector<double> cost(nd, NAN);
  std::vector<double> r;
  for (int k = 0; k < nd; ++k) {
    const double d = -Dmax + k * st;
    r.clear();
    for (size_t i : use) {
      const double b = at(x[i] + d * fx, y[i] + d * fy);
      if (!std::isnan(b)) r.push_back(z[i] - b);
    }
    if (r.size() < use.size() / 2) continue;
    std::vector<double> t = r;
    std::nth_element(t.begin(), t.begin() + t.size() / 2, t.end());
    const double med = t[t.size() / 2];
    double s2 = 0, m = 0;
    for (double e : r) {  // écarts bornés à ±2 m autour de la médiane (sondes aberrantes)
      const double q = std::max(-2.0, std::min(2.0, e - med));
      m += q; s2 += q * q;
    }
    m /= r.size();
    cost[k] = s2 / r.size() - m * m;
  }
  if (costOut) *costOut = cost;
  int kb = -1;
  for (int k = 0; k < nd; ++k)
    if (!std::isnan(cost[k]) && (kb < 0 || cost[k] < cost[kb])) kb = k;
  if (kb <= 0 || kb >= nd - 1) return false;  // pas de minimum à l'intérieur
  // décision : le coût remonte nettement à ±6 m du minimum (relief
  // suffisant), et le minimum est franc devant l'absence de décalage
  const int k6 = (int)std::lround(6 / st), k0 = (int)std::lround(Dmax / st);
  const double cm = kb - k6 >= 0 && !std::isnan(cost[kb - k6]) ? cost[kb - k6] : NAN;
  const double cp = kb + k6 < nd && !std::isnan(cost[kb + k6]) ? cost[kb + k6] : NAN;
  const double rise = std::min(std::isnan(cm) ? 1e9 : cm, std::isnan(cp) ? 1e9 : cp);
  quality = cost[kb] > 0 ? rise / cost[kb] : 0;
  if (!(rise >= 1.2 * cost[kb] && rise - cost[kb] > 0.0025)) return false;  // relief insuffisant
  if (std::abs(kb - k0) > 2 && !(cost[kb] <= 0.8 * cost[k0])) return false;  // minimum peu marqué
  double d = -Dmax + kb * st;
  {
    const double a = cost[kb - 1], b0 = cost[kb], b = cost[kb + 1];
    const double den = a - 2 * b0 + b;
    if (!std::isnan(a) && !std::isnan(b) && den > 0) d += st * std::max(-0.5, std::min(0.5, 0.5 * (a - b) / den));
  }
  offFwd = 0.5 * d;  // le recalage vaut deux fois le décalage
  return std::fabs(offFwd) <= 10;
}

struct AntennaEstimate {
  bool ok = false;
  double fwd = 0, stbd = 0;
};

// Position de la sonde : antenne GPS + décalage (avant, tribord) selon le cap
// de la route ; décalage donné, ou estimé (profondeurs des allers-retours sur
// les pentes, sinon sonar latéral du journal)
static void ApplyAntennaOffset(const std::string& path, std::vector<TrackSample>& s, const LoadOptions& opt,
                               std::string& desc, AntennaEstimate& pc_est, const std::atomic<bool>* cancel) {
  const size_t n = s.size();
  if (n < 2) return;
  double lat0 = 0, lon0 = 0;
  for (const auto& t : s) { lat0 += t.lat; lon0 += t.lon; }
  lat0 /= n;
  lon0 /= n;
  const double kx = kDegM * std::cos(lat0 * M_PI / 180.0);
  std::vector<double> x(n), y(n), hdg(n, NAN), dist(n, 0);
  std::vector<float> z(n);
  for (size_t i = 0; i < n; ++i) {
    x[i] = (s[i].lon - lon0) * kx;
    y[i] = (s[i].lat - lat0) * kDegM;
    z[i] = s[i].depth;
    if (i) dist[i] = dist[i - 1] + std::min(20.0, std::hypot(x[i] - x[i - 1], y[i] - y[i - 1]));
  }
  // cap : corde de la route sur ±5 m ; à l'arrêt, dernier cap connu
  {
    size_t lo = 0, hi = 0;
    double last = NAN;
    for (size_t i = 0; i < n; ++i) {
      while (lo < i && dist[lo] < dist[i] - 5) ++lo;
      if (hi < i) hi = i;
      while (hi + 1 < n && dist[hi + 1] <= dist[i] + 5) ++hi;
      if (std::hypot(x[hi] - x[lo], y[hi] - y[lo]) > 2) last = std::atan2(x[hi] - x[lo], y[hi] - y[lo]);
      hdg[i] = last;
    }
  }
  double F = opt.antFwd, S = opt.antStbd;
  const char* how = pd::tr("saisi");
  bool estimated = false;
  if (opt.antAuto) {
    double f = 0, st = 0, q = 0;
    // d'abord les profondeurs elles-mêmes : l'écart mesuré comprend aussi le
    // retard de la profondeur du sondeur (lissage), qui décale les sondes le
    // long de la route comme une sonde placée en arrière ; à défaut (pas de
    // relief), le sonar latéral du journal (même sonde, image sans retard)
    if (EstimateTrackOffset(x, y, z, hdg, f, q)) {
      F = f;
      how = pd::tr("estimé sur les profondeurs, vers l'avant seulement");
      estimated = true;
    } else if (IsSonarLogFile(path) && EstimateSideScanOffset(path, f, st, q, cancel)) {
      F = f;
      S = st;
      how = pd::tr("estimé sur le sonar latéral");
      estimated = true;
    }
  }
  if (estimated) {
    pc_est.ok = true;
    pc_est.fwd = F;
    pc_est.stbd = S;
  }
  char b[300];
  if (F == 0 && S == 0) {
    if (opt.antAuto)
      desc = pd::tr(" ; décalage sonde / antenne non estimé (pas d'allers-retours sur une pente ni de sonar latéral recalable)");
    return;
  }
  std::snprintf(b, sizeof b, pd::tr(" ; sonde à %+.1f m vers l'avant, %+.1f m vers tribord de l'antenne (%s)"), F, S, how);
  desc = b;
  if (opt.antAuto && !estimated)
    desc += pd::tr(" — estimation impossible, valeurs saisies");
  for (size_t i = 0; i < n; ++i) {
    if (!std::isfinite(hdg[i])) continue;
    const double fx = std::sin(hdg[i]), fy = std::cos(hdg[i]), sx = std::cos(hdg[i]), sy = -std::sin(hdg[i]);
    const double dx = F * fx + S * sx, dy = F * fy + S * sy;
    s[i].lon += dx / kx;
    s[i].lat += dy / kDegM;
  }
}

bool LoadTrack(const std::string& path, const LoadOptions& opt, PointCloud& pc, std::string& err,
               const ProgressFn& progress, const std::atomic<bool>* cancel, TrackInfo* infoOut) {
  std::vector<TrackSample> s;
  TrackInfo info;
  const bool gpx = LowerExt(path) == ".gpx";
  const bool ok = gpx ? ReadGpxSamples(path, s, info, err)
                      : ReadSonarLogSamples(path, s, info, err, progress, cancel);
  if (infoOut) *infoOut = info;
  if (!ok) return false;
  pc.posInfo.clear();
  AntennaEstimate est;
  if (opt.antAuto || opt.antFwd != 0 || opt.antStbd != 0) ApplyAntennaOffset(path, s, opt, pc.posInfo, est, cancel);
  pc.antEstOk = est.ok;
  pc.antEstFwd = est.fwd;
  pc.antEstStbd = est.stbd;
  pc.clear();
  pc.reserve(s.size());
  long long tMin = 0, tMax = 0, noTime = 0, miss = 0;
  float cmin = 1e30f, cmax = -1e30f;
  const float sgn = opt.waterSubtract ? 1.f : -1.f;
  for (const TrackSample& t : s) {
    if (cancel && *cancel) { err = "Annulé"; return false; }
    float z = t.depth;
    if (opt.water) {
      long long tt = t.t;
      if (!t.hasT) {
        if (!opt.waterTime) { ++noTime; continue; }
        tt = opt.waterTime;
      }
      float h;
      if (!opt.water->at(tt, h)) { ++miss; continue; }
      z -= sgn * h;
      cmin = std::min(cmin, -sgn * h);
      cmax = std::max(cmax, -sgn * h);
    }
    if (t.hasT) {
      if (!tMin && !tMax) tMin = tMax = t.t;
      tMin = std::min(tMin, t.t);
      tMax = std::max(tMax, t.t);
    }
    pc.add(t.lon, t.lat, z + opt.zOffset);
  }
  pc.computeBounds();
  pc.tMin = tMin;
  pc.tMax = tMax;
  pc.waterApplied = opt.water && !pc.empty();
  pc.waterNoTime = noTime;
  pc.waterMiss = miss;
  pc.waterCorrMin = pc.waterApplied ? cmin : 0.f;
  pc.waterCorrMax = pc.waterApplied ? cmax : 0.f;
  if (pc.empty()) {
    char b[300];
    if (opt.water && (noTime || miss))
      std::snprintf(b, sizeof b,
                    pd::tr("Aucune sonde corrigée par les hauteurs d'eau : %lld sans date lisible, "
                           "%lld hors de la période des hauteurs (%s … %s)"),
                    noTime, miss, FormatTime(opt.water->tMin()).c_str(), FormatTime(opt.water->tMax()).c_str());
    else
      std::snprintf(b, sizeof b, pd::tr("%s : aucune profondeur dans le fichier (%lld points lus)"),
                    info.kind.c_str(), info.records);
    err = b;
    return false;
  }
  return true;
}

}  // namespace pd

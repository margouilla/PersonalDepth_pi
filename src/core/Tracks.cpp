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
#include "WaterLevel.h"

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

bool LoadTrack(const std::string& path, const LoadOptions& opt, PointCloud& pc, std::string& err,
               const ProgressFn& progress, const std::atomic<bool>* cancel, TrackInfo* infoOut) {
  std::vector<TrackSample> s;
  TrackInfo info;
  const bool gpx = LowerExt(path) == ".gpx";
  const bool ok = gpx ? ReadGpxSamples(path, s, info, err)
                      : ReadSonarLogSamples(path, s, info, err, progress, cancel);
  if (infoOut) *infoOut = info;
  if (!ok) return false;
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

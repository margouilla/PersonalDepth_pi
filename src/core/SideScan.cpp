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

#include "SideScan.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <map>

#include "Datum.h"
#include "FileUtil.h"
#include "Lang.h"
#include "PointCloud.h"

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

namespace pd {

namespace {
// Champs des trames (petit-boutistes) ; données sonar = fin de la trame
// (taille de trame − taille des données : l'entête SL3 varie selon la version)
//                    SL2   SL3
//   taille trame      28     8  u16
//   canal             32    12  u16 (5 : sonar latéral bâbord | tribord)
//   taille données    34    44  u16
//   limites haut/bas  40    20  f32 ×2, pieds (canal 5 : −R, +R)
//   profondeur        64    48  f32, pieds
//   Est, Nord        108    92  i32, Mercator sphérique Lowrance (m)
//   route fond       120   104  f32, radians
//   cap              128   112  f32, radians (0 sans compas)
//   heure            140   124  i32, ms depuis l'ouverture
struct Layout {
  int size, chan, pk, lim, depth, east, cog, head, time, header;
};
constexpr Layout kL2{28, 32, 34, 40, 64, 108, 120, 128, 140, 144};
constexpr Layout kL3{8, 12, 44, 20, 48, 92, 104, 112, 124, 168};
constexpr double kLowranceR = 6356752.3142;
constexpr double kFt = 0.3048;
constexpr double kDegM = 111194.92664;  // mètres par degré (sphère de rayon moyen)
constexpr int kSideScan = 5;
constexpr double kMaxPixels = 2.5e7;  // fauchée parcourue, en pixels (≈ 150 Mo)
constexpr float kWScale = 1024.f;     // poids quantifiés sur 16 bits

template <class T>
T Get(const uint8_t* p, int off) {
  T v;
  std::memcpy(&v, p + off, sizeof v);
  return v;
}

struct Ping {
  long long dataPos;  // position des données dans le fichier
  int32_t e, n, ms;
  float depthM;       // profondeur du journal (0 : inconnue)
  float rangeM;       // portée par bord
  float cog, head;
  uint16_t samples;   // échantillons (deux bords)
  float pick = 0;     // hauteur au-dessus du fond lue sur l'image (premier écho), 0 : non trouvée
  // calculés
  double x = 0, y = 0, hdg = 0, h = 0, s = 0;
  bool ok = false;
};

// Lecture séquentielle par grands blocs
class Reader {
 public:
  explicit Reader(FILE* f) : f_(f), buf_(8u << 20) {}
  const uint8_t* at(long long pos, size_t need) {
    if (pos < base_ || pos + (long long)need > base_ + have_) {
#if defined(_WIN32)
      _fseeki64(f_, pos, SEEK_SET);
#else
      fseeko(f_, (off_t)pos, SEEK_SET);
#endif
      base_ = pos;
      have_ = (long long)std::fread(buf_.data(), 1, buf_.size(), f_);
      if (have_ < (long long)need) return nullptr;
    }
    return buf_.data() + (pos - base_);
  }

 private:
  FILE* f_;
  std::vector<uint8_t> buf_;
  long long base_ = -1, have_ = 0;
};
// Premier écho du fond sur l'image du ping (bedpick) : la colonne d'eau est
// sombre, le fond clair ; on cherche le plus fort saut de la moyenne des deux
// bords autour de la profondeur du journal (0,5 à 1,5 fois), sinon dans la
// première moitié de la portée. Renvoie la hauteur (m), 0 si aucun saut net.
float PickBottom(const uint8_t* dd, int n, double range, float depthM) {
  if (n < 32) return 0.f;
  const double dr = range / n;
  std::vector<float> pr(n);
  for (int j = 0; j < n; ++j) pr[j] = 0.5f * (dd[n - 1 - j] + dd[n + j]);  // depuis la verticale
  std::vector<double> cs(n + 1, 0.0);
  for (int j = 0; j < n; ++j) cs[j + 1] = cs[j] + pr[j];
  const int k = std::max(3, n / 150);
  int j0 = 3 + k, j1 = n / 2;
  if (depthM > 0) {
    j0 = std::max(j0, (int)(0.5 * depthM / dr));
    j1 = std::min(n - k - 1, (int)(1.5 * depthM / dr) + 1);
  }
  double best = 0;
  int bj = -1;
  for (int j = j0; j <= j1 && j + k <= n; ++j) {
    const double after = (cs[j + k] - cs[j]) / k, before = (cs[j] - cs[j - k]) / k;
    const double sc = after - before;
    if (sc > best && after > 1.4 * before + 4) { best = sc; bj = j; }
  }
  if (bj < 0 || best < 8) return 0.f;
  return (float)(bj * dr);
}

// Décalage antenne GPS / sonde estimé sur les lignes parcourues en sens
// opposés (principe du « patch test ») : sans décalage appliqué, une tache du
// fond apparaît en p − u sur les lignes de sens A et en p + u sur celles de
// sens B (u = X·avant + Y·tribord du sens A). Les deux mosaïques grossières,
// débarrassées de leurs variations lentes, sont recalées par corrélation : le
// meilleur recalage D vaut 2u.
template <class Place>
bool EstimateOffset(Place& place, const std::vector<Ping>& P, double maxR, double& offF, double& offS,
                    double& axis, double& score,
                    const std::function<void(float, const std::string&)>& progress, const std::string& step,
                    const std::atomic<bool>* cancel) {
  score = 0;
  // axe du levé : moyenne axiale des caps (2θ), pondérée par la distance
  double C = 0, S = 0, dA = 0, dB = 0;
  double x0 = 1e18, x1 = -1e18, y0 = 1e18, y1 = -1e18;
  size_t prev = SIZE_MAX;
  for (size_t i = 0; i < P.size(); ++i) {
    if (!P[i].ok) continue;
    x0 = std::min(x0, P[i].x); x1 = std::max(x1, P[i].x);
    y0 = std::min(y0, P[i].y); y1 = std::max(y1, P[i].y);
    if (prev != SIZE_MAX) {
      const double d = std::min(4.0, std::hypot(P[i].x - P[prev].x, P[i].y - P[prev].y));
      C += d * std::cos(2 * P[i].hdg);
      S += d * std::sin(2 * P[i].hdg);
    }
    prev = i;
  }
  axis = 0.5 * std::atan2(S, C);
  prev = SIZE_MAX;
  for (size_t i = 0; i < P.size(); ++i) {
    if (!P[i].ok) continue;
    if (prev != SIZE_MAX) {
      const double d = std::min(4.0, std::hypot(P[i].x - P[prev].x, P[i].y - P[prev].y));
      const double ca = std::cos(P[i].hdg - axis);
      if (ca > 0.8) dA += d;
      if (ca < -0.8) dB += d;
    }
    prev = i;
  }
  if (dA < 50 || dB < 50) return false;  // pas de lignes en sens opposés
  // grille grossière : 0,5 m, élargie pour rester sous 6 M cellules
  const double area = (x1 - x0 + 2 * maxR) * (y1 - y0 + 2 * maxR);
  const double pix = std::max(0.5, std::sqrt(area / 6e6));
  SideScanMosaic ga, gb;
  ga.pixel = gb.pixel = pix;
  if (!place(0.0, 0.0, nullptr, &ga, &gb, axis, 0.5f, 0.6f)) return false;
  if (cancel && *cancel) return false;
  if (progress) progress(0.6f, step);
  // grilles denses sur l'emprise commune
  const long long i0 = (long long)std::floor((x0 - maxR) / pix), j0 = (long long)std::floor((y0 - maxR) / pix);
  const int W = (int)std::ceil((x1 - x0 + 2 * maxR) / pix) + 1, H = (int)std::ceil((y1 - y0 + 2 * maxR) / pix) + 1;
  auto dense = [&](const SideScanMosaic& g, std::vector<float>& v) {
    v.assign((size_t)W * H, NAN);
    for (const auto& kv : g.blocks) {
      const long long bi = (kv.first >> 32) - (1LL << 30), bj = (kv.first & 0xffffffffLL) - (1LL << 30);
      for (int r = 0; r < SideScanMosaic::kB; ++r)
        for (int c = 0; c < SideScanMosaic::kB; ++c) {
          const size_t k = (size_t)r * SideScanMosaic::kB + c;
          if (!kv.second.w[k]) continue;
          const long long x = bi * SideScanMosaic::kB + c - i0, y = bj * SideScanMosaic::kB + r - j0;
          if (x < 0 || y < 0 || x >= W || y >= H) continue;
          v[(size_t)y * W + x] = kv.second.sum[k] / (float)kv.second.w[k];
        }
    }
    // filtre passe-bande : moyenne locale sur ±1 m (chatoiement retiré)
    // moins moyenne sur ±12 m (gain, éclairage) ; restent les taches du fond
    // de quelques mètres, vues pareil des deux sens
    const int r1 = std::max(1, (int)std::lround(1.0 / pix)), r2 = std::max(4, (int)std::lround(12.0 / pix));
    std::vector<double> cs((size_t)(W + 1) * (H + 1), 0), cn((size_t)(W + 1) * (H + 1), 0);
    for (int y = 0; y < H; ++y)
      for (int x = 0; x < W; ++x) {
        const float a = v[(size_t)y * W + x];
        const size_t q = (size_t)(y + 1) * (W + 1) + x + 1;
        cs[q] = cs[q - 1] + cs[q - (W + 1)] - cs[q - (W + 1) - 1] + (std::isnan(a) ? 0 : a);
        cn[q] = cn[q - 1] + cn[q - (W + 1)] - cn[q - (W + 1) - 1] + (std::isnan(a) ? 0 : 1);
      }
    auto boxMean = [&](int x, int y, int r, double& n) {
      const int xa = std::max(0, x - r), xb = std::min(W, x + r + 1), ya = std::max(0, y - r), yb = std::min(H, y + r + 1);
      auto at = [&](const std::vector<double>& t, int xx, int yy) { return t[(size_t)yy * (W + 1) + xx]; };
      n = at(cn, xb, yb) - at(cn, xa, yb) - at(cn, xb, ya) + at(cn, xa, ya);
      const double sm = at(cs, xb, yb) - at(cs, xa, yb) - at(cs, xb, ya) + at(cs, xa, ya);
      return n > 0 ? sm / n : 0.0;
    };
    std::vector<float> out((size_t)W * H, NAN);
    for (int y = 0; y < H; ++y)
      for (int x = 0; x < W; ++x) {
        if (std::isnan(v[(size_t)y * W + x])) continue;
        double n1, n2;
        const double m1 = boxMean(x, y, r1, n1), m2 = boxMean(x, y, r2, n2);
        if (n2 >= 0.3 * (2 * r2 + 1) * (2 * r2 + 1)) out[(size_t)y * W + x] = (float)(m1 - m2);
      }
    v.swap(out);
  };
  std::vector<float> A, B;
  dense(ga, A);
  dense(gb, B);
  ga.blocks.clear();
  gb.blocks.clear();
  std::vector<uint32_t> idx;  // cellules renseignées de A
  for (size_t k = 0; k < A.size(); ++k)
    if (!std::isnan(A[k])) idx.push_back((uint32_t)k);
  auto ncc = [&](int dx, int dy, long long* cnt = nullptr) {
    double sab = 0, saa = 0, sbb = 0;
    long long nn = 0;
    for (uint32_t k : idx) {
      const int x = (int)(k % W) + dx, y = (int)(k / W) + dy;
      if (x < 0 || y < 0 || x >= W || y >= H) continue;
      const float b = B[(size_t)y * W + x];
      if (std::isnan(b)) continue;
      const float a = A[k];
      sab += (double)a * b; saa += (double)a * a; sbb += (double)b * b;
      ++nn;
    }
    if (cnt) *cnt = nn;
    return nn >= 2000 && saa > 0 && sbb > 0 ? sab / std::sqrt(saa * sbb) : -1.0;
  };
  const int Smax = (int)std::ceil(20.0 / pix);  // ±20 m (décalage jusqu'à ±10 m)
  double best = -1;
  int bx = 0, by = 0;
  const int st = std::max(1, (int)std::lround(1.0 / pix));
  for (int dy = -Smax; dy <= Smax; dy += st)
    for (int dx = -Smax; dx <= Smax; dx += st) {
      const double c = ncc(dx, dy);
      if (c > best) { best = c; bx = dx; by = dy; }
      if (cancel && *cancel) return false;
    }
  const int cx = bx, cy = by;
  for (int dy = cy - st; dy <= cy + st; ++dy)
    for (int dx = cx - st; dx <= cx + st; ++dx) {
      const double c = ncc(dx, dy);
      if (c > best) { best = c; bx = dx; by = dy; }
    }
  // sommet sous-cellule (parabole)
  auto sub = [&](double cm, double c0, double cp) {
    const double d = cm - 2 * c0 + cp;
    return d < 0 ? std::max(-0.5, std::min(0.5, 0.5 * (cm - cp) / d)) : 0.0;
  };
  const double fxs = bx + sub(ncc(bx - 1, by), best, ncc(bx + 1, by));
  const double fys = by + sub(ncc(bx, by - 1), best, ncc(bx, by + 1));
  const double c0 = ncc(0, 0);
  score = best;
  // pic net et à l'intérieur de la recherche
  if (best < 0.08 || best < c0 + 0.02 || std::abs(bx) >= Smax || std::abs(by) >= Smax) return false;
  const double Dx = fxs * pix, Dy = fys * pix;
  const double fa = std::sin(axis), fb = std::cos(axis), sa = std::cos(axis), sb = -std::sin(axis);
  offF = 0.5 * (Dx * fa + Dy * fb);
  offS = 0.5 * (Dx * sa + Dy * sb);
  return std::fabs(offF) <= 10 && std::fabs(offS) <= 10;
}
}  // namespace

bool IsSonarLogFile(const std::string& path) {
  std::string e = std::filesystem::u8path(path).extension().u8string();
  for (auto& c : e) c = (char)std::tolower((unsigned char)c);
  return e == ".sl2" || e == ".sl3";
}

// ------------------------------------------------------------------ mosaïque
void SideScanMosaic::add(double x, double y, float v, float w) {
  const long long i = (long long)std::floor(x / pixel), j = (long long)std::floor(y / pixel);
  const long long bi = i >= 0 ? i / kB : -((-i - 1) / kB) - 1, bj = j >= 0 ? j / kB : -((-j - 1) / kB) - 1;
  Block& b = blocks[Key(bi, bj)];
  if (b.w.empty()) { b.sum.assign(kB * kB, 0.f); b.w.assign(kB * kB, 0); }
  const size_t k = (size_t)(j - bj * kB) * kB + (size_t)(i - bi * kB);
  const int wq = std::max(1, (int)std::lround(w * kWScale));
  if (b.w[k] > 65535 - wq) return;  // poids saturé (bateau arrêté) : moyenne acquise
  b.sum[k] += v * wq;
  b.w[k] = (uint16_t)(b.w[k] + wq);
}

void SideScanMosaic::lonLat(long long i, long long j, double& lon, double& lat) const {
  lat = lat0 + (j + 0.5) * pixel / kDegM;
  lon = lon0 + (i + 0.5) * pixel / (kDegM * std::cos(lat0 * M_PI / 180.0));
}

void SideScanMosaic::forEach(const std::function<void(double, double, float)>& fn) const {
  // blocs dans l'ordre (lignes de blocs) : chaque lot de points de l'import
  // touche peu de tuiles de la base
  std::vector<int64_t> keys;
  keys.reserve(blocks.size());
  for (const auto& kv : blocks) keys.push_back(kv.first);
  std::sort(keys.begin(), keys.end(), [](int64_t a, int64_t b) {
    return (a & 0xffffffffLL) != (b & 0xffffffffLL) ? (a & 0xffffffffLL) < (b & 0xffffffffLL) : a < b;
  });
  for (int64_t key : keys) {
    const long long bi = (key >> 32) - (1LL << 30), bj = (key & 0xffffffffLL) - (1LL << 30);
    const Block& b = blocks.at(key);
    for (int r = 0; r < kB; ++r)
      for (int c = 0; c < kB; ++c) {
        const size_t k = (size_t)r * kB + c;
        if (!b.w[k]) continue;
        double lon, lat;
        lonLat(bi * kB + c, bj * kB + r, lon, lat);
        fn(lon, lat, b.sum[k] / (float)b.w[k]);
      }
  }
}

// ------------------------------------------------------------------ lecture
bool BuildSideScanMosaic(const std::string& path, const SideScanOptions& o, SideScanMosaic& m,
                         SideScanInfo& info, std::string& err,
                         const std::function<void(float, const std::string&)>& progress,
                         const std::atomic<bool>* cancel) {
  info = SideScanInfo();
  m.blocks.clear();
  FILE* f = OpenFile(path, "rb");
  if (!f) { err = pd::tr("Impossible d'ouvrir ") + path; return false; }
  struct Closer { FILE* f; ~Closer() { std::fclose(f); } } closer{f};
  std::error_code ec;
  const long long total = (long long)std::filesystem::file_size(U8Path(path), ec);
  uint8_t fh[8];
  if (std::fread(fh, 1, 8, f) != 8 || (Get<uint16_t>(fh, 0) != 2 && Get<uint16_t>(fh, 0) != 3)) {
    err = pd::tr("Journal sonar non reconnu (formats SL2 et SL3)");
    return false;
  }
  const bool sl3 = Get<uint16_t>(fh, 0) == 3;
  const Layout L = sl3 ? kL3 : kL2;
  info.kind = sl3 ? "SL3" : "SL2";
  const std::string stepRead = pd::tr("lecture du sonar latéral");
  const std::string stepMos = pd::tr("assemblage de la mosaïque");

  // 1) pings du sonar latéral : navigation (données lues au passage 2)
  std::vector<Ping> P;
  Reader rd(f);
  std::map<int, long long> ranges;  // portée (dm) -> pings
  long long pos = 8;
  while (pos + 32 <= total) {
    const uint8_t* h = rd.at(pos, (size_t)std::min<long long>(L.header, total - pos));
    if (!h || Get<uint32_t>(h, 0) != (uint32_t)pos) break;
    const uint16_t size = Get<uint16_t>(h, L.size);
    if (size < L.header || pos + size > total) break;
    ++info.frames;
    if (Get<uint16_t>(h, L.chan) == kSideScan) {
      const uint16_t pk = Get<uint16_t>(h, L.pk);
      const float up = Get<float>(h, L.lim), lo = Get<float>(h, L.lim + 4);
      const double R = 0.5 * (lo - up) * kFt;
      if (pk >= 8 && pk <= size && (pk & 1) == 0 && R > 0.5 && R < 2000) {
        Ping p;
        p.dataPos = pos + size - pk;
        p.e = Get<int32_t>(h, L.east);
        p.n = Get<int32_t>(h, L.east + 4);
        p.ms = Get<int32_t>(h, L.time);
        const float d = Get<float>(h, L.depth);
        p.depthM = (std::isfinite(d) && d > 0.f && d < 3000.f) ? (float)(d * kFt) : 0.f;
        p.rangeM = (float)R;
        p.cog = Get<float>(h, L.cog);
        p.head = Get<float>(h, L.head);
        p.samples = pk;
        ++ranges[(int)std::lround(R * 10)];
        if (o.bedpick) {
          const uint8_t* dd = rd.at(p.dataPos, pk);
          if (dd) p.pick = PickBottom(dd, pk / 2, R, p.depthM);
        }
        P.push_back(p);
      }
    }
    pos += size;
    if ((info.frames & 4095) == 0) {
      if (progress && total > 0) progress(0.5f * (float)pos / (float)total, stepRead);
      if (cancel && *cancel) { err = "Annulé"; return false; }
    }
  }
  info.pings = (long long)P.size();
  if (P.empty()) {
    err = pd::tr("Journal sans sonar latéral (canal SideScan / StructureScan absent)");
    return false;
  }
  {
    long long best = 0;
    for (auto& kv : ranges)
      if (kv.second > best) { best = kv.second; info.rangeM = kv.first / 10.0; }
  }

  // 2) hauteur au-dessus du fond : médiane glissante du premier écho lu sur
  //    l'image (bedpick), sinon de la profondeur du journal
  {
    std::vector<float> w, wp;
    const size_t K = 12;
    for (size_t i = 0; i < P.size(); ++i) {
      w.clear();
      wp.clear();
      for (size_t k = i >= K ? i - K : 0; k < std::min(P.size(), i + K + 1); ++k) {
        if (P[k].depthM > 0) w.push_back(P[k].depthM);
        if (P[k].pick > 0) wp.push_back(P[k].pick);
      }
      if (wp.size() >= 5) {
        std::nth_element(wp.begin(), wp.begin() + wp.size() / 2, wp.end());
        P[i].h = wp[wp.size() / 2];
        ++info.picked;
      } else if (!w.empty()) {
        std::nth_element(w.begin(), w.begin() + w.size() / 2, w.end());
        P[i].h = w[w.size() / 2];
      }
    }
  }

  // 3) positions : point GPS interpolé dans le temps entre deux positions
  //    (récepteur à 1–10 Hz, pings plus fréquents), repère local en mètres
  auto lonOf = [](int32_t e) { return e / kLowranceR * 180.0 / M_PI; };
  auto latOf = [](int32_t n) { return (2.0 * std::atan(std::exp(n / kLowranceR)) - M_PI / 2) * 180.0 / M_PI; };
  std::vector<size_t> fix;  // premier ping de chaque nouvelle position
  for (size_t i = 0; i < P.size(); ++i) {
    if (P[i].e == 0 && P[i].n == 0) continue;
    if (fix.empty() || P[i].e != P[fix.back()].e || P[i].n != P[fix.back()].n) fix.push_back(i);
  }
  if (fix.empty()) { err = pd::tr("Journal sans position GPS"); return false; }
  {
    double la0 = 1e9, la1 = -1e9, lo0 = 1e9, lo1 = -1e9;
    for (size_t k : fix) {
      const double la = latOf(P[k].n), lo = lonOf(P[k].e);
      la0 = std::min(la0, la); la1 = std::max(la1, la);
      lo0 = std::min(lo0, lo); lo1 = std::max(lo1, lo);
    }
    m.lat0 = 0.5 * (la0 + la1);
    m.lon0 = 0.5 * (lo0 + lo1);
  }
  const double kx = kDegM * std::cos(m.lat0 * M_PI / 180.0);
  auto xyOf = [&](size_t k, double& x, double& y) {
    x = (lonOf(P[k].e) - m.lon0) * kx;
    y = (latOf(P[k].n) - m.lat0) * kDegM;
  };
  for (size_t a = 0; a < fix.size(); ++a) {
    const size_t i0 = fix[a], i1 = a + 1 < fix.size() ? fix[a + 1] : P.size();
    double xa, ya, xb, yb;
    xyOf(i0, xa, ya);
    if (a + 1 < fix.size()) xyOf(fix[a + 1], xb, yb); else { xb = xa; yb = ya; }
    const double ta = P[i0].ms, tb = a + 1 < fix.size() ? P[fix[a + 1]].ms : ta;
    for (size_t i = i0; i < i1; ++i) {
      if (P[i].e == 0 && P[i].n == 0) continue;
      const double u = tb > ta ? std::min(1.0, std::max(0.0, (P[i].ms - ta) / (tb - ta))) : 0.0;
      P[i].x = xa + (xb - xa) * u;
      P[i].y = ya + (yb - ya) * u;
      P[i].ok = true;
    }
  }
  // lissage de la route : moyenne glissante sur ±1,5 s (bruit du GPS, qui
  //    ferait tourner les pings en éventail)
  if (o.smoothTrack) {
    std::vector<size_t> idx;
    for (size_t i = 0; i < P.size(); ++i)
      if (P[i].ok) idx.push_back(i);
    std::vector<double> cx(idx.size() + 1, 0), cy(idx.size() + 1, 0);
    for (size_t q = 0; q < idx.size(); ++q) {
      cx[q + 1] = cx[q] + P[idx[q]].x;
      cy[q + 1] = cy[q] + P[idx[q]].y;
    }
    std::vector<double> nx(idx.size()), ny(idx.size());
    size_t lo = 0, hi = 0;
    for (size_t q = 0; q < idx.size(); ++q) {
      const int32_t t = P[idx[q]].ms;
      while (lo < q && (P[idx[lo]].ms < t - 1500 || P[idx[lo]].ms > t)) ++lo;
      if (hi < q) hi = q;
      while (hi + 1 < idx.size() && P[idx[hi + 1]].ms <= t + 1500 && P[idx[hi + 1]].ms >= t) ++hi;
      const size_t a = std::min(q - lo, hi - q);  // fenêtre symétrique (bords, virages serrés)
      const double c = (double)(2 * a + 1);
      nx[q] = (cx[q + a + 1] - cx[q - a]) / c;
      ny[q] = (cy[q + a + 1] - cy[q - a]) / c;
    }
    for (size_t q = 0; q < idx.size(); ++q) {
      P[idx[q]].x = nx[q];
      P[idx[q]].y = ny[q];
    }
  }
  // distance parcourue
  {
    double s = 0, px = 0, py = 0;
    bool first = true;
    for (auto& p : P) {
      if (!p.ok) continue;
      if (!first) s += std::hypot(p.x - px, p.y - py);
      first = false;
      p.s = s;
      px = p.x; py = p.y;
    }
    info.lengthKm = s / 1000.0;
  }
  // 4) cap : direction de la route sur ±5 m ; à l'arrêt, cap compas ou route fond
  {
    std::vector<size_t> idx;
    for (size_t i = 0; i < P.size(); ++i)
      if (P[i].ok) idx.push_back(i);
    const double W = 5.0;
    size_t lo = 0, hi = 0;
    double last = NAN;
    for (size_t q = 0; q < idx.size(); ++q) {
      Ping& p = P[idx[q]];
      while (lo < q && P[idx[lo]].s < p.s - W) ++lo;
      if (hi < q) hi = q;
      while (hi + 1 < idx.size() && P[idx[hi + 1]].s <= p.s + W) ++hi;
      const Ping &a = P[idx[lo]], &b = P[idx[hi]];
      if (std::hypot(b.x - a.x, b.y - a.y) > 2.0) {
        p.hdg = std::atan2(b.x - a.x, b.y - a.y);
      } else if (std::isfinite(p.head) && p.head != 0.f && std::fabs(p.head) < 7.f) {
        p.hdg = p.head;
      } else if (std::isfinite(last)) {
        p.hdg = last;
      } else if (std::isfinite(p.cog) && std::fabs(p.cog) < 7.f) {
        p.hdg = p.cog;
      } else {
        p.ok = false;
        continue;
      }
      last = p.hdg;
    }
  }

  // 5) mosaïque, avec égalisation glissante : chaque tranche de 1 m de
  //    distance au sol (par bord) est ramenée au même niveau moyen, moyenne
  //    prise sur ±K pings. Corrige la décroissance avec la distance, le
  //    diagramme du transducteur et les changements de gain ou de portée.
  //    Au-delà de la portée acoustique, la moyenne tombe sur un plateau de
  //    bruit : à partir de la première tranche (après le maximum) qui passe
  //    sous plancher + 15 % de l'écart au maximum, le bord n'est pas placé.
  // pas : portée / 400 (de 0,1 à 1 m) ou celui demandé, élargi si la
  // fauchée parcourue dépasse kMaxPixels pixels (mémoire : 6 octets par pixel)
  {
    double area = 0;
    size_t prev = SIZE_MAX;
    for (size_t i = 0; i < P.size(); ++i) {
      if (!P[i].ok || P[i].h <= 0) continue;
      if (prev != SIZE_MAX) {
        const double R = P[i].rangeM, hh = P[i].h;
        area += std::min(4.0, std::hypot(P[i].x - P[prev].x, P[i].y - P[prev].y)) * 2 *
                std::sqrt(std::max(0.0, R * R - hh * hh));
      }
      prev = i;
    }
    m.pixel = o.pixel > 0 ? o.pixel : std::min(1.0, std::max(0.1, info.rangeM / 400.0));
    const double minPix = std::sqrt(area / kMaxPixels);
    if (m.pixel < minPix) {
      m.pixel = std::ceil(minPix * 100.0) / 100.0;
      info.pixelWidened = true;
    }
  }
  info.pixel = m.pixel;
  const double pix = m.pixel;
  constexpr int K = 150;           // demi-fenêtre (pings)
  constexpr float kTarget = 100.f; // niveau moyen après égalisation
  double maxR = 0;
  for (const auto& p : P) maxR = std::max(maxR, (double)p.rangeM);
  const int nb = (int)std::ceil(maxR) + 1;  // tranches de 1 m par bord
  const size_t N = P.size();
  std::vector<uint8_t> bins;            // moyenne de chaque tranche par ping (0 : vide)
  if (o.equalize) bins.assign(N * 2 * nb, 0);
  // placement des pings ; offF / offS : position de la sonde par rapport à
  // l'antenne GPS (m, vers l'avant et vers tribord) ; mm : mosaïque ; ga / gb :
  // mosaïques grossières des lignes dans le sens de l'axe et à contre-sens
  auto place = [&](double offF, double offS, SideScanMosaic* mm, SideScanMosaic* ga, SideScanMosaic* gb,
                   double axis, float p0, float p1) -> bool {
    std::vector<double> wSum(2 * nb, 0), wN(2 * nb, 0);
    std::vector<std::vector<uint8_t>> ring(K + 2);  // données des pings c−1..c+K
    std::vector<double> bs(2 * nb), bn(2 * nb);
    auto usable = [&](size_t i) { return P[i].ok && P[i].h > 0; };
    auto window = [&](size_t i, double sg) {
      if (!o.equalize || !usable(i)) return;
      const uint8_t* b = &bins[i * 2 * nb];
      for (int k = 0; k < 2 * nb; ++k)
        if (b[k]) { wSum[k] += sg * b[k]; wN[k] += sg; }
    };
    size_t prevOk = SIZE_MAX;
    bool readFail = false;
    long long used = 0;
    for (size_t i = 0; i < N + K && !readFail; ++i) {
      if (i < N && usable(i)) {  // lecture du ping i et de ses moyennes par tranche
        const Ping& p = P[i];
        const uint8_t* dd = rd.at(p.dataPos, p.samples);
        if (!dd) { readFail = true; break; }
        ring[i % (K + 2)].assign(dd, dd + p.samples);
        if (o.equalize) {
          std::fill(bs.begin(), bs.end(), 0.0);
          std::fill(bn.begin(), bn.end(), 0.0);
          const int n = p.samples / 2;
          const double dr = p.rangeM / n, hh = p.h;
          for (int side = 0; side < 2; ++side)
            for (int j = 0; j < n; ++j) {
              const double r = (j + 0.5) * dr;
              if (r <= hh * 1.02) continue;
              const int b = std::min(nb - 1, (int)std::sqrt(r * r - hh * hh));
              bs[side * nb + b] += side == 0 ? dd[n - 1 - j] : dd[n + j];
              bn[side * nb + b] += 1;
            }
          uint8_t* out = &bins[i * 2 * nb];
          for (int k = 0; k < 2 * nb; ++k)
            if (bn[k] > 0) out[k] = (uint8_t)std::max(1.0, std::min(255.0, std::round(bs[k] / bn[k])));
        }
        window(i, 1.0);
      }
      if (i < (size_t)K) continue;
      const size_t c = i - K;  // ping placé : fenêtre [c − K, c + K]
      if (c >= (size_t)K + 1) window(c - K - 1, -1.0);
      if (c >= N || !usable(c)) continue;
      const Ping& p = P[c];
      const uint8_t* dd = ring[c % (K + 2)].data();
      // parasites (autres sondeurs, canaux du même appareil) : traits brefs
      // sur un seul ping ; médiane de chaque échantillon avec les pings voisins
      const uint8_t* d0 = (c > 0 && usable(c - 1) && P[c - 1].samples == p.samples) ? ring[(c - 1) % (K + 2)].data() : nullptr;
      const uint8_t* d2 = (c + 1 < N && usable(c + 1) && P[c + 1].samples == p.samples) ? ring[(c + 1) % (K + 2)].data() : nullptr;
      auto sample = [&](int k) -> float {
        if (!d0 || !d2) return dd[k];
        const uint8_t a = d0[k], b = dd[k], e = d2[k];
        return (float)std::max(std::min(a, b), std::min(std::max(a, b), e));
      };
      ++used;
      // facteurs d'égalisation et tranches hors de portée
      float fac[2][1024], wq[2][1024];  // facteur d'égalisation, poids (force du signal)
      bool on[2][1024];
      const int nbc = std::min(nb, 1024);
      for (int side = 0; side < 2; ++side) {
        // moyennes de la fenêtre ; plancher de bruit = médiane du dernier
        // cinquième des tranches renseignées (plateau plat hors de portée)
        double w[1024];
        int last = -1, bmax = 0;
        double mx = 0;
        for (int b = 0; b < nbc; ++b) {
          w[b] = wN[side * nb + b] > 0 ? wSum[side * nb + b] / wN[side * nb + b] : 0;
          if (w[b] > 0) last = b;
          if (w[b] > mx) { mx = w[b]; bmax = b; }
        }
        double floorN = 0;
        bool cut = false;
        if (o.equalize && last > 10) {
          std::vector<double> tail;
          for (int b = last - (last + 1) / 5; b <= last; ++b)
            if (w[b] > 0) tail.push_back(w[b]);
          if (!tail.empty()) {
            std::nth_element(tail.begin(), tail.begin() + tail.size() / 2, tail.end());
            floorN = tail[tail.size() / 2];
            cut = mx > 2.5 * floorN;  // signal nettement au-dessus du bruit
          }
        }
        const double limit = floorN + 0.15 * (mx - floorN);
        bool beyond = false;  // au-delà de la portée utile : le reste du bord est coupé
        for (int b = 0; b < nbc; ++b) {
          fac[side][b] = 1.f;
          wq[side][b] = 1.f;
          on[side][b] = true;
          if (!o.equalize) continue;
          // recouvrements : la partie au signal fort (près de la route)
          // l'emporte sur les bords faibles et bruités des lignes voisines
          if (mx > floorN) wq[side][b] = (float)std::min(1.0, std::max(0.03, (w[b] - floorN) / (mx - floorN)));
          if (cut && b > bmax && w[b] < limit) beyond = true;
          on[side][b] = !beyond && w[b] >= 2.0;
          if (on[side][b]) fac[side][b] = (float)std::min(4.0, std::max(0.25, kTarget / w[b]));
        }
      }
      // rayures : gain d'un ping plus fort ou plus faible que ses voisins
      // (réglage automatique du sondeur, roulis) ; rapport médian entre les
      // tranches du ping et la moyenne de la fenêtre, ramené à 1
      if (o.equalize && o.destripe) {
        const uint8_t* bc = &bins[c * 2 * nb];
        std::vector<float> rr;
        for (int side = 0; side < 2; ++side) {
          rr.clear();
          for (int b = 0; b < nbc; ++b) {
            const double w = wN[side * nb + b] > 0 ? wSum[side * nb + b] / wN[side * nb + b] : 0;
            if (on[side][b] && bc[side * nb + b] && w > 0) rr.push_back((float)(bc[side * nb + b] / w));
          }
          if (rr.size() < 5) continue;
          std::nth_element(rr.begin(), rr.begin() + rr.size() / 2, rr.end());
          const float g = std::min(2.f, std::max(0.5f, rr[rr.size() / 2]));
          for (int b = 0; b < nbc; ++b) fac[side][b] /= g;
        }
      }
      // empreinte le long de la route : écart au ping suivant (et virage)
      size_t nx = c + 1;
      while (nx < N && !usable(nx)) ++nx;
      double dAlong = 0, dHdg = 0;
      const Ping* q = nx < N ? &P[nx] : (prevOk != SIZE_MAX ? &P[prevOk] : nullptr);
      if (q) {
        dAlong = std::hypot(q->x - p.x, q->y - p.y);
        dHdg = std::fabs(std::remainder(q->hdg - p.hdg, 2 * M_PI));
      }
      dAlong = std::min(dAlong, 4.0);  // saut de position (GPS perdu) : pas de traînée
      dHdg = std::min(dHdg, 0.2);
      const double fx = std::sin(p.hdg), fy = std::cos(p.hdg);  // avant
      const double sx = std::cos(p.hdg), sy = -std::sin(p.hdg);  // tribord
      // position de la sonde : antenne GPS + décalage (avant, tribord)
      const double tx = p.x + offF * fx + offS * sx, ty = p.y + offF * fy + offS * sy;
      // estimation du décalage : lignes dans le sens de l'axe du levé (A) ou à
      // contre-sens (B), chacune dans sa mosaïque grossière
      SideScanMosaic* cls = nullptr;
      if (ga) {
        const double ca = std::cos(p.hdg - axis);
        cls = ca > 0.8 ? ga : (ca < -0.8 ? gb : nullptr);
      }
      const int n = p.samples / 2;
      const double dr = p.rangeM / n, hh = p.h;
      // rééchantillonnage au sol : pas de pix / 2 ; chaque pas prend la
      // moyenne des échantillons obliques qu'il couvre (loin de la verticale,
      // plusieurs par pas) ou l'interpolation entre les deux plus proches (près
      // de la verticale, où un échantillon s'étale sur plusieurs pas)
      const double gs = 0.5 * pix;
      std::vector<double> cs(n + 1);
      for (int side = 0; side < 2; ++side) {
        const double sg = side == 0 ? -1.0 : 1.0;
        cs[0] = 0;
        for (int j = 0; j < n; ++j) cs[j + 1] = cs[j] + sample(side == 0 ? n - 1 - j : n + j);
        // valeur moyenne entre les positions continues u0 < u1 (en échantillons,
        // l'échantillon j couvrant [j, j + 1[)
        auto meanAt = [&](double u0, double u1) -> double {
          u0 = std::max(0.0, u0);
          u1 = std::min((double)n, u1);
          if (u1 - u0 < 1.0) {  // moins d'un échantillon : interpolation au centre
            const double u = 0.5 * (u0 + u1) - 0.5;
            const int j = std::max(0, std::min(n - 2, (int)std::floor(u)));
            const double t = std::min(1.0, std::max(0.0, u - j));
            return (cs[j + 1] - cs[j]) * (1 - t) + (cs[j + 2] - cs[j + 1]) * t;
          }
          auto at = [&](double u) {  // somme de 0 à u
            const int j = std::min(n - 1, (int)u);
            return cs[j] + (cs[j + 1] - cs[j]) * (u - j);
          };
          return (at(u1) - at(u0)) / (u1 - u0);
        };
        const double rMin = hh * 1.02, rMax = p.rangeM;
        if (rMax <= rMin) continue;
        const double gMax = std::sqrt(rMax * rMax - hh * hh);
        for (double g = std::sqrt(rMin * rMin - hh * hh) + 0.5 * gs; g < gMax; g += gs) {
          const int b = std::min(nbc - 1, (int)g);
          if (!on[side][b]) continue;
          const double ga = g - 0.5 * gs, gb = g + 0.5 * gs;
          const double r0 = std::sqrt(ga * ga + hh * hh), r1 = std::sqrt(gb * gb + hh * hh);
          const float v = (float)meanAt(r0 / dr, r1 / dr) * fac[side][b];
          const double cx = tx + sg * g * sx, cy = ty + sg * g * sy;
          // poids : force du signal ; près de la verticale (incidence presque
          // normale, image peu contrastée) les lignes voisines l'emportent
          float wk = wq[side][b] * wq[side][b];
          if (g < 0.7 * hh) wk *= 0.05f;
          const double along = dAlong + g * dHdg;
          const int na = std::min(16, std::max(1, (int)std::ceil(along / pix)));
          for (int a = 0; a < na; ++a) {
            const double ua = (na > 1 ? (double)a / na : 0.0) * along;
            if (mm) mm->add(cx + ua * fx, cy + ua * fy, v, wk);
            if (cls && wk > 0.2f) cls->add(cx + ua * fx, cy + ua * fy, v, 1.f);
          }
        }
      }
      prevOk = c;
      if ((c & 1023) == 0) {
        if (progress) progress(p0 + (p1 - p0) * (float)c / (float)N, stepMos);
        if (cancel && *cancel) { err = "Annulé"; return false; }
      }
    }
    if (readFail) return false;
    if (mm) info.used = used;
    return true;
  };

  // décalage antenne / sonde : donné, ou estimé sur les lignes parcourues en
  // sens opposés (une même tache vue des deux sens est décalée de deux fois
  // le décalage) ; sinon nul
  double offF = o.offsetFwd, offS = o.offsetStbd;
  if (o.autoOffset) {
    double ox = 0, oy = 0, axis = 0, score = 0;
    if (EstimateOffset(place, P, maxR, ox, oy, axis, score, progress, stepMos, cancel)) {
      offF = ox;
      offS = oy;
      info.offsetAuto = true;
    }
    info.offsetScore = score;
    if (cancel && *cancel) { err = "Annulé"; return false; }
  }
  info.offsetFwd = offF;
  info.offsetStbd = offS;
  if (o.estimateOnly) return true;  // décalage seul (import des profondeurs)
  if (!place(offF, offS, &m, nullptr, nullptr, 0, 0.6f, 1.f)) {
    err = pd::tr("Lecture du journal interrompue");
    return false;
  }
  if (cancel && *cancel) { err = "Annulé"; return false; }
  if (!info.used || m.blocks.empty()) {
    err = pd::tr("Sonar latéral sans position, cap ou hauteur au-dessus du fond utilisables");
    return false;
  }
  // étirement et emprise
  std::vector<long long> hist(4096, 0);
  info.lonMin = info.latMin = 1e9;
  info.lonMax = info.latMax = -1e9;
  m.forEach([&](double lon, double lat, float v) {
    ++info.pixels;
    ++hist[(size_t)std::min(4095.f, std::max(0.f, v * 8.f))];
    info.lonMin = std::min(info.lonMin, lon); info.lonMax = std::max(info.lonMax, lon);
    info.latMin = std::min(info.latMin, lat); info.latMax = std::max(info.latMax, lat);
  });
  long long acc = 0;
  const long long a2 = (long long)(0.02 * info.pixels), a98 = (long long)(0.98 * info.pixels);
  bool loSet = false;
  for (size_t k = 0; k < hist.size(); ++k) {
    acc += hist[k];
    if (!loSet && acc > a2) { info.lo = k / 8.0; loSet = true; }
    if (acc > a98) { info.hi = (k + 1) / 8.0; break; }
  }
  if (info.hi <= info.lo) info.hi = info.lo + 1;
  return true;
}

bool EstimateSideScanOffset(const std::string& path, double& offFwd, double& offStbd, double& score,
                            const std::atomic<bool>* cancel) {
  SideScanOptions o;
  o.autoOffset = true;
  o.estimateOnly = true;
  o.pixel = 0.5;
  SideScanMosaic m;
  SideScanInfo info;
  std::string err;
  if (!BuildSideScanMosaic(path, o, m, info, err, nullptr, cancel)) return false;
  score = info.offsetScore;
  if (!info.offsetAuto) return false;
  offFwd = info.offsetFwd;
  offStbd = info.offsetStbd;
  return true;
}

bool ImportSideScan(BathyDb& db, const std::string& path, const SideScanOptions& o, DbSource meta,
                    DbPriority prio, std::string& err, const DbProgress& progress,
                    const std::atomic<bool>* cancel, int64_t* newId, SideScanInfo* infoOut) {
  if (db.kind() != DbKind::Imagery) { err = "ImportSideScan : base de bathymétrie"; return false; }
  SideScanMosaic m;
  SideScanInfo info;
  auto sub = [&](float f, const std::string& s) { if (progress) progress(0.6f * f, s); };
  if (!BuildSideScanMosaic(path, o, m, info, err, sub, cancel)) return false;
  meta.res = info.pixel;
  meta.gridStepX = meta.gridStepY = info.pixel;
  meta.vunit = pd::tr("niveau");
  meta.vlo = info.lo;
  meta.vhi = info.hi;
  meta.crs = "WGS84";
  meta.datum = (int)Datum::Free;
  {
    char b[320];
    std::snprintf(b, sizeof b,
                  pd::tr("sonar latéral %s : %lld pings, portée ±%.0f m, route %.2f km, pixel %.2f m%s%s"),
                  info.kind.c_str(), info.used, info.rangeM, info.lengthKm, info.pixel,
                  info.pixelWidened ? pd::tr(" (élargi : levé étendu)") : "",
                  o.equalize ? pd::tr(", égalisé") : "");
    meta.info = b;
    if (info.offsetFwd != 0 || info.offsetStbd != 0) {
      std::snprintf(b, sizeof b, pd::tr(" ; sonde à %+.1f m vers l'avant, %+.1f m vers tribord de l'antenne (%s)"),
                    info.offsetFwd, info.offsetStbd, info.offsetAuto ? pd::tr("estimé") : pd::tr("saisi"));
      meta.info += b;
    } else if (o.autoOffset) {
      meta.info += pd::tr(" ; décalage sonde / antenne non estimé (pas de lignes en sens opposés au fond reconnaissable)");
    }
  }
  const size_t kBlock = (size_t)1 << 21;
  BathyDb::CloudFeed feed = [&](const BathyDb::CloudSink& sink, std::string&) {
    PointCloud block;
    block.reserve(kBlock);
    long long done = 0;
    bool ok = true;
    m.forEach([&](double lon, double lat, float v) {
      if (!ok) return;
      block.add(lon, lat, v);
      ++done;
      if (block.size() >= kBlock) {
        block.computeBounds();
        if (!sink(block, (float)done / (float)info.pixels)) ok = false;
        block.clear();
      }
    });
    if (ok && !block.empty()) {
      block.computeBounds();
      ok = sink(block, 1.f);
    }
    return ok;
  };
  auto sub2 = [&](float f, const std::string& s) { if (progress) progress(0.6f + 0.4f * f, s); };
  if (!db.importCloudStream(feed, 0.5 * (info.latMin + info.latMax), meta, true, prio, err, sub2, cancel, newId))
    return false;
  if (infoOut) *infoOut = info;
  return true;
}

}  // namespace pd

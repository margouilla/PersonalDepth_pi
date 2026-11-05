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

#include "TideFES.h"
#include "Lang.h"

#include <algorithm>
#include <cmath>
#include <cstring>

#include "Inflate.h"
#include "TideField.h"
#include "VerticalRef.h"

#include <thread>

namespace pd {
namespace {
constexpr double kDeg = M_PI / 180.0;

double norm360(double a) {
  a = std::fmod(a, 360.0);
  return a < 0 ? a + 360.0 : a;
}

enum FKind : uint8_t { F1, FM2, FK1, FO1, FJ1, FK2, FMm, FMf, FL2, FM3, FM4, FM6, FM8, FMKS2, FOO1, FKJ2 };

// τ s h p N' p1 (phase/-90°) | f | ξ ν ν' ν'' R
const TideWave kWaves[] = {
    {"2N2", {2, -2, 0, 2, 0, 0, 0}, FM2, {2, -2, 0, 0, 0}},
    {"Eps2", {2, -3, 2, 1, 0, 0, 0}, FM2, {2, -2, 0, 0, 0}},
    {"J1", {1, 2, 0, -1, 0, 0, 1}, FJ1, {0, -1, 0, 0, 0}},
    {"K1", {1, 1, 0, 0, 0, 0, 1}, FK1, {0, 0, -1, 0, 0}},
    {"K2", {2, 2, 0, 0, 0, 0, 0}, FK2, {0, 0, 0, -2, 0}},
    {"L2", {2, 1, 0, -1, 0, 0, 2}, FL2, {2, -2, 0, 0, -1}},
    {"Lambda2", {2, 1, -2, 1, 0, 0, 2}, FM2, {2, -2, 0, 0, 0}},
    {"M2", {2, 0, 0, 0, 0, 0, 0}, FM2, {2, -2, 0, 0, 0}},
    {"M3", {3, 0, 0, 0, 0, 0, 0}, FM3, {3, -3, 0, 0, 0}},
    {"M4", {4, 0, 0, 0, 0, 0, 0}, FM4, {4, -4, 0, 0, 0}},
    {"M6", {6, 0, 0, 0, 0, 0, 0}, FM6, {6, -6, 0, 0, 0}},
    {"M8", {8, 0, 0, 0, 0, 0, 0}, FM8, {8, -8, 0, 0, 0}},
    {"MKS2", {2, 0, 2, 0, 0, 0, 0}, FMKS2, {2, -2, 0, -2, 0}},
    {"MN4", {4, -1, 0, 1, 0, 0, 0}, FM4, {4, -4, 0, 0, 0}},
    {"MS4", {4, 2, -2, 0, 0, 0, 0}, FM2, {2, -2, 0, 0, 0}},
    {"MSf", {0, 2, -2, 0, 0, 0, 0}, FM2, {2, -2, 0, 0, 0}},
    {"Mf", {0, 2, 0, 0, 0, 0, 0}, FMf, {-2, 0, 0, 0, 0}},
    {"Mm", {0, 1, 0, -1, 0, 0, 0}, FMm, {0, 0, 0, 0, 0}},
    {"Msqm", {0, 4, -2, 0, 0, 0, 0}, FMf, {-2, 0, 0, 0, 0}},
    {"Mtm", {0, 3, 0, -1, 0, 0, 0}, FMf, {-2, 0, 0, 0, 0}},
    {"Mu2", {2, -2, 2, 0, 0, 0, 0}, FM2, {2, -2, 0, 0, 0}},
    {"N2", {2, -1, 0, 1, 0, 0, 0}, FM2, {2, -2, 0, 0, 0}},
    {"N4", {4, -2, 0, 2, 0, 0, 0}, FM4, {4, -4, 0, 0, 0}},
    {"Nu2", {2, -1, 2, -1, 0, 0, 0}, FM2, {2, -2, 0, 0, 0}},
    {"O1", {1, -1, 0, 0, 0, 0, -1}, FO1, {2, -1, 0, 0, 0}},
    {"P1", {1, 1, -2, 0, 0, 0, -1}, F1, {0, 0, 0, 0, 0}},
    {"Q1", {1, -2, 0, 1, 0, 0, -1}, FO1, {2, -1, 0, 0, 0}},
    {"R2", {2, 2, -1, 0, 0, -1, 2}, F1, {0, 0, 0, 0, 0}},
    {"S1", {1, 1, -1, 0, 0, 0, 0}, F1, {0, 0, 0, 0, 0}},
    {"S2", {2, 2, -2, 0, 0, 0, 0}, F1, {0, 0, 0, 0, 0}},
    {"S4", {4, 4, -4, 0, 0, 0, 0}, F1, {0, 0, 0, 0, 0}},
    {"Sa", {0, 0, 1, 0, 0, 0, 0}, F1, {0, 0, 0, 0, 0}},
    {"Ssa", {0, 0, 2, 0, 0, 0, 0}, F1, {0, 0, 0, 0, 0}},
    {"T2", {2, 2, -3, 0, 0, 1, 0}, F1, {0, 0, 0, 0, 0}},
    // ondes mineures déduites par admittance (absentes de l'atlas)
    {"2Q1", {1, -3, 0, 2, 0, 0, -1}, FO1, {2, -1, 0, 0, 0}},
    {"Sigma1", {1, -3, 2, 0, 0, 0, -1}, FO1, {2, -1, 0, 0, 0}},
    {"Rho1", {1, -2, 2, -1, 0, 0, -1}, FO1, {2, -1, 0, 0, 0}},
    {"M11", {1, 0, 0, -1, 0, 0, 1}, FO1, {2, -1, 0, 0, 0}},
    {"M12", {1, 0, 0, 1, 0, 0, 1}, FJ1, {0, -1, 0, 0, 0}},
    {"Chi1", {1, 0, 2, -1, 0, 0, 1}, FJ1, {0, -1, 0, 0, 0}},
    {"Pi1", {1, 1, -3, 0, 0, 1, -1}, F1, {0, 0, 0, 0, 0}},
    {"Phi1", {1, 1, 2, 0, 0, 0, 1}, F1, {0, 0, 0, 0, 0}},
    {"Theta1", {1, 2, -2, 1, 0, 0, 1}, FJ1, {0, -1, 0, 0, 0}},
    {"OO1", {1, 3, 0, 0, 0, 0, 1}, FOO1, {-2, -1, 0, 0, 0}},
    {"Eta2", {2, 3, 0, -1, 0, 0, 0}, FKJ2, {0, -2, 0, 0, 0}},
};

// Admittances (coefficients réels, R. Ray / FES) : onde mineure = a·base1 + b·base2
struct Inferred { const char* name; const char* b1; double a; const char* b2; double b; };
const Inferred kInferred[] = {
    {"2Q1", "Q1", 0.263, "O1", -0.0252},     {"Sigma1", "Q1", 0.297, "O1", -0.0264},
    {"Rho1", "Q1", 0.164, "O1", 0.0048},     {"M11", "O1", 0.0140, "K1", 0.0101},
    {"M12", "O1", 0.0389, "K1", 0.0282},     {"Chi1", "O1", 0.0064, "K1", 0.0060},
    {"Pi1", "O1", 0.0030, "K1", 0.0171},     {"Phi1", "O1", -0.0015, "K1", 0.0152},
    {"Theta1", "O1", -0.0065, "K1", 0.0155}, {"OO1", "O1", -0.0431, "K1", 0.0613},
    {"Eta2", "M2", -0.0034925, "K2", 0.0831707},
};

bool ieq(const std::string& a, const char* b) {
  if (a.size() != std::strlen(b)) return false;
  for (size_t k = 0; k < a.size(); ++k)
    if (std::tolower((unsigned char)a[k]) != std::tolower((unsigned char)b[k])) return false;
  return true;
}
}  // namespace

const TideWave* FindTideWave(const std::string& name) {
  for (const auto& w : kWaves)
    if (ieq(name, w.name)) return &w;
  return nullptr;
}

// ------------------------------------------------------------ astronomie
void TideAstro::update(double unixSeconds) {
  const double days = unixSeconds / 86400.0;
  const double jd = days + 2440587.5;
  const double tt = (jd - 2415020.0) / 36525.0;  // siècles depuis 1900-01-00 12 h
  const double frac = days - std::floor(days);
  t = norm360(180.0 + 360.0 * frac);
  h = norm360(279.696678 + 36000.768930485 * tt + 3.03e-4 * tt * tt);
  s = norm360(270.434164 + 481267.88314137 * tt - 0.001133 * tt * tt + 1.9e-6 * tt * tt * tt);
  p1 = norm360(281.220833 + 1.719175 * tt + 4.528e-4 * tt * tt + 3.3e-6 * tt * tt * tt);
  p = norm360(334.329556 + 4069.0340329577 * tt - 0.010325 * tt * tt - 1.2e-5 * tt * tt * tt);
  n = norm360(259.183275 - 1934.1420085 * tt + 0.002078 * tt * tt + 2.2e-6 * tt * tt * tt);

  const double N = n * kDeg;
  const double i = std::acos(0.913694997 - 0.035692561 * std::cos(N));
  const double nuR = std::asin(0.0897056 * std::sin(N) / std::sin(i));
  // ξ = N - 2 atan(0,64412 tan(N/2)) - ν, sans discontinuité en N = 180°
  const double half = std::atan2(0.64412 * std::sin(N / 2), std::cos(N / 2));
  double xiR = N - 2 * half - nuR;
  xiR = std::remainder(xiR, 2 * M_PI);
  const double s2i = std::sin(2 * i), si2 = std::sin(i) * std::sin(i);
  const double nupR = std::atan2(s2i * std::sin(nuR), s2i * std::cos(nuR) + 0.3347);
  const double nusR = 0.5 * std::atan2(si2 * std::sin(2 * nuR), si2 * std::cos(2 * nuR) + 0.0727);
  const double P = p * kDeg - xiR;
  const double th = std::tan(i / 2), th2 = th * th;
  x1ra = std::sqrt(1 - 12 * th2 * std::cos(2 * P) + 36 * th2 * th2);
  const double RR = std::atan2(std::sin(2 * P), 1 / (6 * th2) - std::cos(2 * P));
  I = i / kDeg; nu = nuR / kDeg; xi = xiR / kDeg; nuprim = nupR / kDeg; nusec = nusR / kDeg;
  R = RR / kDeg;
}

void TideNodal::compute(const std::vector<const TideWave*>& waves, double unixSeconds) {
  TideAstro a;
  a.update(unixSeconds);
  const double I = a.I * kDeg, nu = a.nu * kDeg;
  const double ci2 = std::cos(I / 2), si = std::sin(I), s2i = std::sin(2 * I);
  const double fM2 = std::pow(ci2, 4) / 0.9154;
  const double fK2 = std::sqrt(19.0444 * std::pow(si, 4) + 2.7702 * si * si * std::cos(2 * nu) + 0.0981);
  const double tau = a.t - a.s + a.h;
  f.resize(waves.size()); cosArg.resize(waves.size()); sinArg.resize(waves.size());
  for (size_t k = 0; k < waves.size(); ++k) {
    const TideWave& w = *waves[k];
    double ff = 1;
    switch (w.fKind) {
      case FM2: ff = fM2; break;
      case FK1: ff = std::sqrt(0.8965 * s2i * s2i + 0.6001 * s2i * std::cos(nu) + 0.1006); break;
      case FO1: ff = si * ci2 * ci2 / 0.3800; break;
      case FJ1: ff = s2i / 0.7214; break;
      case FK2: ff = fK2; break;
      case FMm: ff = (2.0 / 3.0 - si * si) / 0.5021; break;
      case FMf: ff = si * si / 0.1578; break;
      case FL2: ff = fM2 * a.x1ra; break;
      case FM3: ff = std::pow(fM2, 1.5); break;
      case FM4: ff = fM2 * fM2; break;
      case FM6: ff = fM2 * fM2 * fM2; break;
      case FM8: ff = fM2 * fM2 * fM2 * fM2; break;
      case FMKS2: ff = fM2 * fK2; break;
      case FOO1: ff = si * std::pow(std::sin(I / 2), 2) / 0.01640; break;
      case FKJ2: ff = si * si / 0.1565; break;
      default: break;
    }
    const double V = w.d[0] * tau + w.d[1] * a.s + w.d[2] * a.h + w.d[3] * a.p +
                     w.d[4] * a.n + w.d[5] * a.p1 - 90.0 * w.d[6];
    const double u = w.u[0] * a.xi + w.u[1] * a.nu + w.u[2] * a.nuprim + w.u[3] * a.nusec +
                     w.u[4] * a.R;
    const double arg = norm360(V + u) * kDeg;
    f[k] = ff;
    cosArg[k] = std::cos(arg);
    sinArg[k] = std::sin(arg);
  }
}

// ------------------------------------------------------------ zone
bool TideRegion::covers(double lonMin, double lonMax, double latMin, double latMax) const {
  if (empty()) return false;
  double x0 = (lonMin - lon0) / dlon;
  if (x0 < 0) x0 += 360.0 / dlon;
  const double x1 = x0 + (lonMax - lonMin) / dlon;
  const double y0 = (latMin - lat0) / dlat, y1 = (latMax - lat0) / dlat;
  return x0 >= 1 && x1 <= nx - 2 && y0 >= 1 && y1 <= ny - 2;
}

void TideRegion::heights(const TideNodal& nod, std::vector<float>& hh) const {
  const size_t n = (size_t)nx * ny;
  hh.assign(n, 0.f);
  std::vector<double> acc(n, 0.0);
  for (size_t w = 0; w < waves.size(); ++w) {
    const double a = nod.f[w] * nod.cosArg[w], b = nod.f[w] * nod.sinArg[w];
    const float* R = &re[w * n];
    const float* M = &im[w * n];
    for (size_t k = 0; k < n; ++k) acc[k] += a * R[k] + b * M[k];
  }
  for (size_t k = 0; k < n; ++k) hh[k] = valid[k] ? (float)acc[k] : NAN;
}

double TideRegion::sample(const std::vector<float>& field, double lon, double lat) const {
  if (empty()) return NAN;
  double x = (lon - lon0) / dlon;
  if (x < 0) x += 360.0 / dlon;  // zone à cheval sur 0°
  const double y = (lat - lat0) / dlat;
  const int i = (int)std::floor(x), j = (int)std::floor(y);
  if (i < 0 || j < 0 || i + 1 >= nx || j + 1 >= ny) return NAN;
  const double fx = x - i, fy = y - j;
  const double w[4] = {(1 - fx) * (1 - fy), fx * (1 - fy), (1 - fx) * fy, fx * fy};
  const size_t c[4] = {(size_t)j * nx + i, (size_t)j * nx + i + 1, (size_t)(j + 1) * nx + i,
                       (size_t)(j + 1) * nx + i + 1};
  double s = 0, ws = 0;
  for (int k = 0; k < 4; ++k) {
    const float v = field[c[k]];
    if (std::isnan(v) || w[k] <= 0) continue;
    s += w[k] * v;
    ws += w[k];
  }
  return ws > 0 ? s / ws : NAN;
}

double TideRegion::heightAt(double lon, double lat, double unixSeconds) const {
  if (empty()) return NAN;
  TideNodal nod;
  nod.compute(waves, unixSeconds);
  // nœuds voisins seulement
  double x = (lon - lon0) / dlon;
  if (x < 0) x += 360.0 / dlon;
  const double y = (lat - lat0) / dlat;
  const int i = (int)std::floor(x), j = (int)std::floor(y);
  if (i < 0 || j < 0 || i + 1 >= nx || j + 1 >= ny) return NAN;
  const size_t n = (size_t)nx * ny;
  std::vector<float> loc(n, NAN);
  for (int dj = 0; dj <= 1; ++dj)
    for (int di = 0; di <= 1; ++di) {
      const size_t c = (size_t)(j + dj) * nx + i + di;
      if (!valid[c]) continue;
      double acc = 0;
      for (size_t w = 0; w < waves.size(); ++w)
        acc += nod.f[w] * (nod.cosArg[w] * re[w * n + c] + nod.sinArg[w] * im[w * n + c]);
      loc[c] = (float)acc;
    }
  return sample(loc, lon, lat);
}

// ------------------------------------------------------------ fichier
TideAtlas::~TideAtlas() {
  if (f_) std::fclose(f_);
}

static bool Seek(FILE* f, long long pos) {
#ifdef _WIN32
  return _fseeki64(f, pos, SEEK_SET) == 0;
#else
  return fseeko(f, (off_t)pos, SEEK_SET) == 0;
#endif
}

template <class T> static T Get(const uint8_t* p) {
  T v;
  std::memcpy(&v, p, sizeof v);  // fichier petit-boutiste, comme x86
  return v;
}

bool TideAtlas::open(const std::string& path, std::string& err) {
  if (f_) { std::fclose(f_); f_ = nullptr; }
  names_.clear();
  FILE* f = std::fopen(path.c_str(), "rb");
  if (!f) { err = pd::tr("Impossible d'ouvrir ") + path; return false; }
  uint8_t h[80];
  if (std::fread(h, 1, 80, f) != 80 || std::memcmp(h, "PDTIDE01", 8) != 0) {
    std::fclose(f);
    err = pd::tr("Fichier de marée .pdtide invalide : ") + path;
    return false;
  }
  W_ = (int)Get<uint32_t>(h + 8);
  T_ = (int)Get<uint32_t>(h + 12);
  nx_ = Get<int32_t>(h + 16); ny_ = Get<int32_t>(h + 20);
  ntx_ = Get<int32_t>(h + 24); nty_ = Get<int32_t>(h + 28);
  lon0_ = Get<double>(h + 32); lat0_ = Get<double>(h + 40);
  dlon_ = Get<double>(h + 48); dlat_ = Get<double>(h + 56);
  unit_ = Get<double>(h + 64);
  periodic_ = (Get<uint32_t>(h + 72) & 1) != 0;
  if (W_ <= 0 || W_ > 256 || T_ <= 0 || T_ > 1024 || nx_ <= 0 || ny_ <= 0) {
    std::fclose(f);
    err = pd::tr("Entête .pdtide incohérent : ") + path;
    return false;
  }
  for (int w = 0; w < W_; ++w) {
    char nm[17] = {0};
    if (std::fread(nm, 1, 16, f) != 16) { std::fclose(f); err = pd::tr("Fichier .pdtide tronqué"); return false; }
    names_.push_back(nm);
  }
  indexPos_ = 80 + 16LL * W_;
  f_ = f;
  path_ = path;
  return true;
}

bool TideAtlas::readTile(int w, int tj, int ti, std::vector<int16_t>& re, std::vector<int16_t>& im,
                         std::vector<uint8_t>& mask, bool& any, std::string& err) {
  any = false;
  uint8_t e[16];
  if (!Seek(f_, indexPos_ + 16LL * (((long long)w * nty_ + tj) * ntx_ + ti)) ||
      std::fread(e, 1, 16, f_) != 16) {
    err = pd::tr("Lecture de l'index .pdtide impossible");
    return false;
  }
  const uint64_t off = Get<uint64_t>(e);
  const uint32_t size = Get<uint32_t>(e + 8);
  const uint8_t kind = e[12];
  if (kind == 0) return true;
  std::vector<uint8_t> comp(size), raw;
  if (!Seek(f_, (long long)off) || std::fread(comp.data(), 1, size, f_) != size) {
    err = pd::tr("Lecture d'une tuile .pdtide impossible");
    return false;
  }
  const size_t TT = (size_t)T_ * T_;
  const size_t mbytes = kind == 2 ? (TT + 7) / 8 : 0;
  if (!ZlibInflate(comp.data(), comp.size(), raw, mbytes + 4 * TT) || raw.size() != mbytes + 4 * TT) {
    err = pd::tr("Tuile .pdtide corrompue");
    return false;
  }
  mask.assign(TT, 1);
  if (kind == 2)
    for (size_t k = 0; k < TT; ++k) mask[k] = (raw[k >> 3] >> (k & 7)) & 1;
  re.resize(TT); im.resize(TT);
  for (int c = 0; c < 2; ++c) {
    const uint8_t* lo = raw.data() + mbytes + c * 2 * TT;
    const uint8_t* hi = lo + TT;
    std::vector<int16_t>& dst = c ? im : re;
    for (int r = 0; r < T_; ++r) {
      int acc = 0;  // différences le long de la ligne
      for (int q = 0; q < T_; ++q) {
        const size_t k = (size_t)r * T_ + q;
        acc += (int16_t)(uint16_t)(lo[k] | (hi[k] << 8));
        dst[k] = (int16_t)acc;
      }
    }
  }
  any = true;
  return true;
}

bool TideAtlas::loadRegion(double lonMin, double lonMax, double latMin, double latMax,
                           TideRegion& r, std::string& err) {
  r = TideRegion();
  if (!f_) { err = pd::tr("Atlas de marée non ouvert"); return false; }
  // nœuds couverts (+1 de marge de chaque côté)
  double lo0 = lonMin;
  while (periodic_ && lo0 < lon0_) lo0 += 360.0;
  long i0 = (long)std::floor((lo0 - lon0_) / dlon_) - 1;
  long i1 = (long)std::ceil((lo0 + (lonMax - lonMin) - lon0_) / dlon_) + 1;
  long j0 = std::max(0L, (long)std::floor((latMin - lat0_) / dlat_) - 1);
  long j1 = std::min((long)ny_ - 1, (long)std::ceil((latMax - lat0_) / dlat_) + 1);
  if (!periodic_) { i0 = std::max(0L, i0); i1 = std::min((long)nx_ - 1, i1); }
  if (i1 < i0 || j1 < j0) { err = pd::tr("Zone hors de l'atlas de marée"); return false; }
  if ((i1 - i0 + 1) * (j1 - j0 + 1) > 4000000) { err = pd::tr("Zone trop grande pour la marée maillée"); return false; }
  r.nx = (int)(i1 - i0 + 1);
  r.ny = (int)(j1 - j0 + 1);
  r.lon0 = lon0_ + i0 * dlon_;
  r.lat0 = lat0_ + j0 * dlat_;
  r.dlon = dlon_; r.dlat = dlat_;
  std::vector<int> wid;
  for (int w = 0; w < W_; ++w)
    if (const TideWave* tw = FindTideWave(names_[w])) { r.waves.push_back(tw); wid.push_back(w); }
  if (r.waves.empty()) { err = pd::tr("Aucune onde connue dans ") + path_; return false; }
  const size_t n = (size_t)r.nx * r.ny;
  r.re.assign(n * r.waves.size(), 0.f);
  r.im.assign(n * r.waves.size(), 0.f);
  r.valid.assign(n, 0);
  // l'onde M2 (ou la première) définit les nœuds valides
  size_t ref = 0;
  for (size_t k = 0; k < r.waves.size(); ++k)
    if (std::strcmp(r.waves[k]->name, "M2") == 0) ref = k;
  std::vector<int16_t> tre, tim;
  std::vector<uint8_t> tmask;
  const long tj0 = j0 / T_, tj1 = j1 / T_;
  for (size_t k = 0; k < r.waves.size(); ++k) {
    float* R = &r.re[k * n];
    float* M = &r.im[k * n];
    for (long tj = tj0; tj <= tj1; ++tj) {
      // tuiles en longitude, avec repliement
      for (long gi = i0; gi <= i1;) {
        long wi = periodic_ ? ((gi % nx_) + nx_) % nx_ : gi;
        const long ti = wi / T_;
        const long run = std::min<long>(T_ - wi % T_, nx_ - wi);  // jusqu'au bord de tuile ou de grille
        const long tileEnd = std::min<long>(i1, gi + run - 1);
        bool any;
        if (!readTile(wid[k], (int)tj, (int)ti, tre, tim, tmask, any, err)) return false;
        if (any) {
          for (long gj = std::max(j0, tj * T_); gj <= std::min(j1, tj * T_ + T_ - 1); ++gj)
            for (long g = gi; g <= tileEnd; ++g) {
              const long wg = periodic_ ? ((g % nx_) + nx_) % nx_ : g;
              const size_t s = (size_t)(gj - tj * T_) * T_ + (wg - ti * T_);
              if (wg >= nx_ || !tmask[s]) continue;
              const size_t d = (size_t)(gj - j0) * r.nx + (g - i0);
              R[d] = (float)(tre[s] * unit_);
              M[d] = (float)(tim[s] * unit_);
              if (k == ref) r.valid[d] = 1;
            }
        }
        gi = tileEnd + 1;
      }
    }
  }
  inferMinorWaves(r);
  return true;
}

// Ondes mineures déduites des ondes principales (comme FES / pyfes) ; une
// onde déjà présente dans l'atlas ou dont une base manque n'est pas ajoutée
void TideAtlas::inferMinorWaves(TideRegion& r) const {
  if (!inferMinor_) return;
  const size_t n = (size_t)r.nx * r.ny;
  auto index = [&](const char* name) -> int {
    for (size_t k = 0; k < r.waves.size(); ++k)
      if (std::strcmp(r.waves[k]->name, name) == 0) return (int)k;
    return -1;
  };
  for (const Inferred& in : kInferred) {
    const int i1 = index(in.b1), i2 = index(in.b2);
    if (i1 < 0 || i2 < 0 || index(in.name) >= 0) continue;
    r.waves.push_back(FindTideWave(in.name));
    const size_t base = r.re.size();
    r.re.resize(base + n);
    r.im.resize(base + n);
    for (size_t c = 0; c < n; ++c) {
      r.re[base + c] = (float)(in.a * r.re[(size_t)i1 * n + c] + in.b * r.re[(size_t)i2 * n + c]);
      r.im[base + c] = (float)(in.a * r.im[(size_t)i1 * n + c] + in.b * r.im[(size_t)i2 * n + c]);
    }
  }
}

// ------------------------------------------------------------ plus basse mer
void LowestAstronomicalTide(const TideRegion& r, const std::vector<size_t>& nodes,
                            std::vector<float>& out, const std::atomic<bool>* cancel,
                            std::atomic<float>* progress) {
  out.assign(nodes.size(), NAN);
  if (nodes.empty() || r.empty()) return;
  const size_t W = r.waves.size(), n = (size_t)r.nx * r.ny;
  const double t0 = 1546300800.0;          // 2019-01-01 : 19 ans couvrent le cycle nodal
  const int nStep = 19 * 8766;             // pas horaire
  std::vector<float> A((size_t)nStep * W), B((size_t)nStep * W);
  TideNodal nod;
  for (int k = 0; k < nStep; ++k) {
    nod.compute(r.waves, t0 + 3600.0 * k);
    for (size_t w = 0; w < W; ++w) {
      A[(size_t)k * W + w] = (float)(nod.f[w] * nod.cosArg[w]);
      B[(size_t)k * W + w] = (float)(nod.f[w] * nod.sinArg[w]);
    }
    if (cancel && *cancel) return;
    if (progress && k % 8192 == 0) *progress = 0.2f * k / nStep;
  }
  unsigned nt = std::max(1u, std::min(8u, std::thread::hardware_concurrency()));
  std::atomic<size_t> next{0}, done{0};
  auto work = [&]() {
    std::vector<double> re(W), im(W);
    TideNodal nd;
    for (;;) {
      const size_t q = next++;
      if (q >= nodes.size() || (cancel && *cancel)) return;
      const size_t c = nodes[q];
      for (size_t w = 0; w < W; ++w) { re[w] = r.re[w * n + c]; im[w] = r.im[w * n + c]; }
      // trois plus basses mers horaires distinctes (> 6 h d'écart)
      double best[3] = {1e9, 1e9, 1e9};
      int bi[3] = {-100, -100, -100};
      for (int k = 0; k < nStep; ++k) {
        const float* a = &A[(size_t)k * W];
        const float* b = &B[(size_t)k * W];
        double h = 0;
        for (size_t w = 0; w < W; ++w) h += a[w] * re[w] + b[w] * im[w];
        if (h >= best[2]) continue;
        int slot = -1;
        for (int m = 0; m < 3; ++m)
          if (std::abs(k - bi[m]) <= 6) { slot = m; break; }
        if (slot >= 0) {
          if (h < best[slot]) { best[slot] = h; bi[slot] = k; }
        } else {
          best[2] = h; bi[2] = k;
        }
        for (int m = 2; m > 0; --m)  // tri croissant
          if (best[m] < best[m - 1]) { std::swap(best[m], best[m - 1]); std::swap(bi[m], bi[m - 1]); }
      }
      double low = best[0];
      for (int m = 0; m < 3; ++m) {
        if (bi[m] < 0) continue;
        for (int dm = -60; dm <= 60; dm += 5) {
          nd.compute(r.waves, t0 + 3600.0 * bi[m] + 60.0 * dm);
          double h = 0;
          for (size_t w = 0; w < W; ++w) h += nd.f[w] * (nd.cosArg[w] * re[w] + nd.sinArg[w] * im[w]);
          low = std::min(low, h);
        }
      }
      out[q] = (float)low;
      const size_t d = ++done;
      if (progress) *progress = 0.2f + 0.8f * d / nodes.size();
    }
  };
  std::vector<std::thread> th;
  for (unsigned k = 1; k < nt; ++k) th.emplace_back(work);
  work();
  for (auto& t : th) t.join();
}

// ------------------------------------------------------------ marée maillée
bool FesTide::prepare(TideAtlas& atlas, double lonMin, double lonMax, double latMin,
                      double latMax, const TideZ0Params& zp, std::string& err,
                      const std::atomic<bool>* cancel, std::atomic<float>* progress) {
  region_ = TideRegion();
  z0_.clear();
  info_.clear();
  bbox[0] = lonMin; bbox[1] = lonMax; bbox[2] = latMin; bbox[3] = latMax;
  const double m = 0.05;
  TideRegion r;
  if (!atlas.loadRegion(lonMin - m, lonMax + m, latMin - m, latMax + m, r, err)) return false;
  const size_t n = (size_t)r.nx * r.ny;
  std::vector<float> z0(n, NAN);
  // grille de surfaces alignée sur les nœuds (ligne 0 = nord, longitudes -180..180)
  Grid2D g;
  g.nx = r.nx; g.ny = r.ny; g.dlon = r.dlon; g.dlat = r.dlat;
  g.lonW = r.lon0 >= 180.0 ? r.lon0 - 360.0 : r.lon0;
  g.latN = r.lat0 + (r.ny - 1) * r.dlat;
  auto toRegion = [&](const Grid2D& s, size_t k) {
    const size_t i = k % r.nx, j = k / r.nx;
    return s.v[(size_t)(r.ny - 1 - j) * r.nx + i];
  };
  size_t nUser = 0, nEll = 0, nLat = 0, nValid = 0;
  for (size_t k = 0; k < n; ++k) nValid += r.valid[k];
  if (!zp.userFiles.empty()) {
    Grid2D u = g;
    u.v.assign(n, NAN);
    if (!SampleSurfaces(zp.userFiles, u, err)) return false;
    for (size_t k = 0; k < n; ++k)
      if (r.valid[k] && std::isnan(z0[k]) && !std::isnan(toRegion(u, k))) { z0[k] = toRegion(u, k); ++nUser; }
  }
  if (!zp.nmFiles.empty() && !zp.zhFiles.empty()) {
    Grid2D a = g, b = g;
    a.v.assign(n, NAN); b.v.assign(n, NAN);
    if (!SampleSurfaces(zp.nmFiles, a, err) || !SampleSurfaces(zp.zhFiles, b, err)) return false;
    for (size_t k = 0; k < n; ++k) {
      const float d = toRegion(a, k) - toRegion(b, k);
      if (r.valid[k] && std::isnan(z0[k]) && !std::isnan(d)) { z0[k] = d; ++nEll; }
    }
  }
  if (cancel && *cancel) return false;
  if (zp.latFallback) {
    // nœuds restants : plus basse mer sur un sous-maillage si la zone est grande
    std::vector<size_t> need;
    for (size_t k = 0; k < n; ++k)
      if (r.valid[k] && std::isnan(z0[k])) need.push_back(k);
    if (!need.empty()) {
      const int st = std::max(1, (int)std::ceil(std::sqrt(need.size() / 2500.0)));
      auto onLattice = [&](int i, int j) {
        return (i % st == 0 || i == r.nx - 1) && (j % st == 0 || j == r.ny - 1);
      };
      std::vector<size_t> coarse;
      for (size_t k = 0; k < n; ++k)
        if (r.valid[k] && onLattice((int)(k % r.nx), (int)(k / r.nx))) coarse.push_back(k);
      std::vector<float> low;
      LowestAstronomicalTide(r, coarse, low, cancel, progress);
      if (cancel && *cancel) return false;
      std::vector<float> lat(n, NAN);
      for (size_t q = 0; q < coarse.size(); ++q) lat[coarse[q]] = low[q];
      std::vector<size_t> rest;
      for (size_t k : need) {
        const int i = (int)(k % r.nx), j = (int)(k / r.nx);
        float v = lat[k];
        if (std::isnan(v)) {  // interpolation entre nœuds du sous-maillage
          const int ia = i / st * st, ja = j / st * st;
          const int ib = std::min(r.nx - 1, ia + st), jb = std::min(r.ny - 1, ja + st);
          const double fx = ib > ia ? (double)(i - ia) / (ib - ia) : 0, fy = jb > ja ? (double)(j - ja) / (jb - ja) : 0;
          const int ci[4] = {ia, ib, ia, ib}, cj[4] = {ja, ja, jb, jb};
          const double w[4] = {(1 - fx) * (1 - fy), fx * (1 - fy), (1 - fx) * fy, fx * fy};
          double s = 0, ws = 0;
          for (int q = 0; q < 4; ++q) {
            const float x = lat[(size_t)cj[q] * r.nx + ci[q]];
            if (!std::isnan(x) && w[q] > 0) { s += w[q] * x; ws += w[q]; }
          }
          if (ws > 0) v = (float)(s / ws);
        }
        if (std::isnan(v)) rest.push_back(k);
        else { z0[k] = -v; ++nLat; }
      }
      if (!rest.empty()) {
        LowestAstronomicalTide(r, rest, low, cancel, nullptr);
        for (size_t q = 0; q < rest.size(); ++q)
          if (!std::isnan(low[q])) { z0[rest[q]] = -low[q]; ++nLat; }
      }
    }
  }
  if (!nValid) { err = pd::tr("Zone hors du modèle de marée"); return false; }
  char b[200];
  auto pc = [&](size_t x) { return (int)std::lround(100.0 * x / nValid); };
  std::snprintf(b, sizeof b, pd::tr("Z0 : %s%s%s"),
                nUser ? (std::string(pd::tr("grille ")) + std::to_string(pc(nUser)) + " % ").c_str() : "",
                nEll ? (std::string("BathyElli ") + std::to_string(pc(nEll)) + " % ").c_str() : "",
                nLat ? (std::string(pd::tr("PBMA FES ")) + std::to_string(pc(nLat)) + " %").c_str() : "");
  info_ = b;
  if (nUser + nEll + nLat == 0) { err = pd::tr("Aucune référence ZH disponible pour la marée maillée"); return false; }
  region_ = std::move(r);
  z0_ = std::move(z0);
  if (progress) *progress = 1.f;
  return true;
}

void FesTide::field(double unixSeconds, TideField& out) const {
  out = TideField();
  if (region_.empty()) return;
  TideNodal nod;
  nod.compute(region_.waves, unixSeconds);
  std::vector<float> hh;
  region_.heights(nod, hh);
  out.nx = region_.nx; out.ny = region_.ny;
  out.lon0 = region_.lon0 >= 180.0 ? region_.lon0 - 360.0 : region_.lon0;
  out.lat0 = region_.lat0;
  out.dlon = region_.dlon; out.dlat = region_.dlat;
  out.v.resize(hh.size());
  double acc = 0;
  size_t cnt = 0;
  for (size_t k = 0; k < hh.size(); ++k) {
    out.v[k] = hh[k] + z0_[k];  // NaN si hors modèle ou sans Z0
    if (!std::isnan(out.v[k])) { acc += out.v[k]; ++cnt; }
  }
  out.h0 = cnt ? (float)(acc / cnt) : 0.f;
  out.finish();
}

double FesTide::heightNM(double lon, double lat, double unixSeconds) const {
  return region_.heightAt(lon, lat, unixSeconds);
}

}  // namespace pd

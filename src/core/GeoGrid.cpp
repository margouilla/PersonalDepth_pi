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

#include "GeoGrid.h"

#include <algorithm>
#include <cmath>
#include <cstring>

#include "FileUtil.h"
#include "Inflate.h"
#include "Lang.h"

namespace pd {

namespace {
bool Seek(FILE* f, long long pos) {
#ifdef _WIN32
  return _fseeki64(f, pos, SEEK_SET) == 0;
#else
  return fseeko(f, (off_t)pos, SEEK_SET) == 0;
#endif
}
template <class T> T Get(const uint8_t* p) {
  T v;
  std::memcpy(&v, p, sizeof v);  // fichier petit-boutiste, comme x86
  return v;
}
std::string Text(const uint8_t* p, size_t n) {
  size_t k = 0;
  while (k < n && p[k]) ++k;
  return std::string((const char*)p, k);
}
constexpr long long kHead = 272;
}  // namespace

GeoGridFile::~GeoGridFile() { close(); }

void GeoGridFile::close() {
  std::lock_guard<std::mutex> lk(mutex_);
  if (f_) std::fclose(f_);
  f_ = nullptr;
  cache_.clear();
  lru_.clear();
}

bool GeoGridFile::open(const std::string& path, std::string& err) {
  close();
  FILE* f = OpenFile(path, "rb");
  if (!f) { err = pd::tr("Impossible d'ouvrir ") + path; return false; }
  uint8_t h[kHead];
  if (std::fread(h, 1, kHead, f) != (size_t)kHead || std::memcmp(h, "PDGRID01", 8) != 0) {
    std::fclose(f);
    err = pd::tr("Fichier de grille .pdgrid invalide : ") + path;
    return false;
  }
  std::lock_guard<std::mutex> lk(mutex_);
  T_ = (int)Get<uint32_t>(h + 8);
  nx_ = Get<int32_t>(h + 12); ny_ = Get<int32_t>(h + 16);
  ntx_ = Get<int32_t>(h + 20); nty_ = Get<int32_t>(h + 24);
  periodic_ = (Get<uint32_t>(h + 28) & 1) != 0;
  lon0_ = Get<double>(h + 32); lat0_ = Get<double>(h + 40);
  dlon_ = Get<double>(h + 48); dlat_ = Get<double>(h + 56);
  unit_ = Get<double>(h + 64);
  quantity_ = Text(h + 72, 32);
  model_ = Text(h + 104, 128);
  period_ = Text(h + 232, 32);
  if (T_ <= 0 || T_ > 1024 || (T_ & 7) || nx_ <= 0 || ny_ <= 0 || dlon_ <= 0 || dlat_ <= 0 ||
      ntx_ != (nx_ + T_ - 1) / T_ || nty_ != (ny_ + T_ - 1) / T_) {
    std::fclose(f);
    err = pd::tr("Entête .pdgrid incohérent : ") + path;
    return false;
  }
  f_ = f;
  path_ = path;
  return true;
}

const GeoGridFile::Tile* GeoGridFile::tile(int tj, int ti, std::string& err) {
  const long key = (long)tj * ntx_ + ti;
  auto it = cache_.find(key);
  if (it != cache_.end()) {
    lru_.remove(key);
    lru_.push_front(key);
    return &it->second;
  }
  Tile t;
  uint8_t e[16];
  if (!Seek(f_, kHead + 16LL * key) || std::fread(e, 1, 16, f_) != 16) {
    err = pd::tr("Lecture de l'index .pdgrid impossible");
    return nullptr;
  }
  const uint64_t off = Get<uint64_t>(e);
  const uint32_t size = Get<uint32_t>(e + 8);
  const uint8_t kind = e[12];
  const size_t TT = (size_t)T_ * T_;
  if (kind != 0) {
    if (kind > 4 || size > 64u * TT) { err = pd::tr("Tuile .pdgrid corrompue"); return nullptr; }
    std::vector<uint8_t> comp(size), raw;
    if (!Seek(f_, (long long)off) || std::fread(comp.data(), 1, size, f_) != size) {
      err = pd::tr("Lecture d'une tuile .pdgrid impossible");
      return nullptr;
    }
    const size_t mbytes = (kind == 2 || kind == 4) ? TT / 8 : 0;
    const size_t want = mbytes + (kind <= 2 ? 4 + 2 * TT : 4 * TT);
    if (!ZlibInflate(comp.data(), comp.size(), raw, want) || raw.size() != want) {
      err = pd::tr("Tuile .pdgrid corrompue");
      return nullptr;
    }
    t.m.assign(TT, 1);
    if (mbytes)
      for (size_t k = 0; k < TT; ++k) t.m[k] = (raw[k >> 3] >> (k & 7)) & 1;
    t.v.resize(TT);
    const uint8_t* p = raw.data() + mbytes;
    if (kind <= 2) {
      int32_t rowStart = Get<int32_t>(p);
      const uint8_t* lo = p + 4;
      const uint8_t* hi = lo + TT;
      auto d = [&](size_t k) { return (int32_t)(int16_t)(uint16_t)(lo[k] | (hi[k] << 8)); };
      for (int r = 0; r < T_; ++r) {
        const size_t k0 = (size_t)r * T_;
        rowStart += d(k0);  // d(0) = 0
        int32_t acc = rowStart;
        t.v[k0] = acc;
        for (int q = 1; q < T_; ++q) {
          acc += d(k0 + q);
          t.v[k0 + q] = acc;
        }
      }
    } else {
      for (size_t k = 0; k < TT; ++k) t.v[k] = Get<int32_t>(p + 4 * k);
    }
    t.empty = false;
  }
  if (cache_.size() >= kCache) {
    cache_.erase(lru_.back());
    lru_.pop_back();
  }
  lru_.push_front(key);
  return &(cache_[key] = std::move(t));
}

double GeoGridFile::node(long i, long j, std::string& err) {
  if (j < 0 || j >= ny_) return NAN;
  if (periodic_) i = ((i % nx_) + nx_) % nx_;
  else if (i < 0 || i >= nx_) return NAN;
  const Tile* t = tile((int)(j / T_), (int)(i / T_), err);
  if (!t || t->empty) return NAN;
  const size_t k = (size_t)(j % T_) * T_ + (i % T_);
  return t->m[k] ? t->v[k] * unit_ : NAN;
}

double GeoGridFile::sample(double lon, double lat) {
  std::lock_guard<std::mutex> lk(mutex_);
  if (!f_) return NAN;
  double x = (lon - lon0_) / dlon_;
  if (periodic_) {
    const double w = 360.0 / dlon_;
    x = std::fmod(x, w);
    if (x < 0) x += w;
  }
  const double y = (lat - lat0_) / dlat_;
  const long i = (long)std::floor(x), j = (long)std::floor(y);
  const double fx = x - i, fy = y - j;
  std::string err;
  double acc = 0, wsum = 0;
  for (int dj = 0; dj <= 1; ++dj)
    for (int di = 0; di <= 1; ++di) {
      const double v = node(i + di, j + dj, err);
      if (std::isnan(v)) continue;
      const double w = (di ? fx : 1 - fx) * (dj ? fy : 1 - fy);
      acc += w * v;
      wsum += w;
    }
  return wsum > 1e-9 ? acc / wsum : NAN;
}

bool GeoGridFile::loadRegion(double lonMin, double lonMax, double latMin, double latMax,
                             Grid2D& g, std::string& err) {
  std::lock_guard<std::mutex> lk(mutex_);
  g = Grid2D();
  if (!f_) { err = pd::tr("Grille .pdgrid non ouverte"); return false; }
  double lo0 = lonMin;
  if (periodic_)
    while (lo0 < lon0_) lo0 += 360.0;
  long i0 = (long)std::floor((lo0 - lon0_) / dlon_) - 1;
  long i1 = (long)std::ceil((lo0 + (lonMax - lonMin) - lon0_) / dlon_) + 1;
  long j0 = std::max(0L, (long)std::floor((latMin - lat0_) / dlat_) - 1);
  long j1 = std::min((long)ny_ - 1, (long)std::ceil((latMax - lat0_) / dlat_) + 1);
  if (!periodic_) { i0 = std::max(0L, i0); i1 = std::min((long)nx_ - 1, i1); }
  if (i1 < i0 || j1 < j0) { err = pd::tr("Zone hors de la grille .pdgrid"); return false; }
  if ((i1 - i0 + 1) * (j1 - j0 + 1) > 16000000) {
    err = pd::tr("Zone trop grande pour la grille .pdgrid");
    return false;
  }
  g.nx = (int)(i1 - i0 + 1);
  g.ny = (int)(j1 - j0 + 1);
  g.dlon = dlon_;
  g.dlat = dlat_;
  g.lonW = lon0_ + i0 * dlon_;
  if (g.lonW >= lonMin + 180.0) g.lonW -= 360.0;  // même plage que la demande
  g.latN = lat0_ + j1 * dlat_;
  g.v.assign((size_t)g.nx * g.ny, NAN);
  std::string e;
  for (long j = j0; j <= j1; ++j)
    for (long i = i0; i <= i1; ++i) {
      const double v = node(i, j, e);
      if (!e.empty()) { err = e; g = Grid2D(); return false; }
      g.v[(size_t)(j1 - j) * g.nx + (i - i0)] = (float)v;
    }
  return true;
}

}  // namespace pd

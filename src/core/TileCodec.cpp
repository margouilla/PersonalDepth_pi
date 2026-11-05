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

#include "TileCodec.h"
#include "Lang.h"

#include <algorithm>
#include <cstring>

#include "miniz.h"

namespace pd {

size_t DepthTile::count() const {
  size_t n = 0;
  for (int32_t x : v) n += x != kEmpty;
  return n;
}

template <class T> static void Put(std::vector<uint8_t>& o, T v) {
  const uint8_t* p = reinterpret_cast<const uint8_t*>(&v);
  o.insert(o.end(), p, p + sizeof v);  // petit-boutiste (x86, ARM)
}
template <class T> static T Get(const uint8_t* p) {
  T v;
  std::memcpy(&v, p, sizeof v);
  return v;
}

// Prédicteur MED (JPEG-LS) : médiane de gauche, haut et gauche + haut - diagonale
static inline int64_t Predict(const int64_t* q, int i, int j) {
  if (j == 0) return i ? q[i - 1] : 0;
  if (i == 0) return q[(j - 1) * 256];
  const int64_t a = q[j * 256 + i - 1], b = q[(j - 1) * 256 + i], c = q[(j - 1) * 256 + i - 1];
  const int64_t mx = a > b ? a : b, mn = a < b ? a : b;
  if (c >= mx) return mn;
  if (c <= mn) return mx;
  return a + b - c;
}
static inline uint32_t ZigZag(int64_t r) { return (uint32_t)((r << 1) ^ (r >> 63)); }
static inline int64_t UnZigZag(uint32_t u) { return (int64_t)(u >> 1) ^ -(int64_t)(u & 1); }

bool EncodeTile(const DepthTile& t, std::vector<uint8_t>& out) {
  out.clear();
  if (!t.allocated()) return false;
  const int N = DepthTile::kN;
  int32_t zmin = INT32_MAX, zmax = INT32_MIN;
  size_t valid = 0;
  for (int32_t x : t.v)
    if (x != DepthTile::kEmpty) { ++valid; zmin = std::min(zmin, x); zmax = std::max(zmax, x); }
  if (!valid) return false;
  // unité commune : mm, cm, dm ou m (valeurs toutes multiples)
  int unit = 0;
  for (int u = 3, d = 1000; u > 0; --u, d /= 10) {
    bool ok = true;
    for (int32_t x : t.v)
      if (x != DepthTile::kEmpty && x % d) { ok = false; break; }
    if (ok) { unit = u; break; }
  }
  static const int kDiv[4] = {1, 10, 100, 1000};
  const bool partial = valid < (size_t)N;
  const bool plane = t.srcIds.size() > 1 && t.src.size() == (size_t)N;
  std::vector<uint8_t> raw;
  raw.reserve((partial ? N / 8 : 0) + 4 * N + (plane ? N : 0));
  if (partial) {
    raw.assign(N / 8, 0);
    for (int k = 0; k < N; ++k)
      if (t.v[k] != DepthTile::kEmpty) raw[k >> 3] |= (uint8_t)(1u << (k & 7));
  }
  // résidus de prédiction ; un pixel vide prend la valeur prédite (résidu nul)
  std::vector<int64_t> q(N);
  std::vector<uint32_t> r(N);
  for (int j = 0; j < 256; ++j)
    for (int i = 0; i < 256; ++i) {
      const int k = j * 256 + i;
      const int64_t p = Predict(q.data(), i, j);
      q[k] = t.v[k] == DepthTile::kEmpty ? p : t.v[k] / kDiv[unit];
      r[k] = ZigZag(q[k] - p);
    }
  const size_t base = raw.size();
  raw.resize(base + 4 * N);
  for (int k = 0; k < N; ++k)
    for (int b = 0; b < 4; ++b) raw[base + (size_t)b * N + k] = (uint8_t)(r[k] >> (8 * b));
  if (plane) raw.insert(raw.end(), t.src.begin(), t.src.end());

  out.push_back('P'); out.push_back('T'); out.push_back(2);
  out.push_back((uint8_t)((partial ? 1 : 0) | (plane ? 2 : 0) | (unit << 2)));
  Put<int32_t>(out, zmin);
  Put<int32_t>(out, zmax);
  Put<uint16_t>(out, (uint16_t)t.srcIds.size());
  for (int64_t id : t.srcIds) Put<int64_t>(out, id);
  Put<uint32_t>(out, (uint32_t)raw.size());
  mz_ulong clen = mz_compressBound((mz_ulong)raw.size());
  const size_t h = out.size();
  out.resize(h + clen);
  if (mz_compress2(out.data() + h, &clen, raw.data(), (mz_ulong)raw.size(), 1) != MZ_OK) {
    out.clear();
    return false;
  }
  out.resize(h + clen);
  return true;
}

bool DecodeTile(const uint8_t* p, size_t n, DepthTile& t, std::string& err) {
  t.reset();
  if (n < 16 || p[0] != 'P' || p[1] != 'T' || p[2] != 2) { err = "Tuile illisible"; return false; }
  const bool partial = p[3] & 1, plane = (p[3] & 2) != 0;
  static const int kDiv[4] = {1, 10, 100, 1000};
  const int mul = kDiv[(p[3] >> 2) & 3];
  const uint16_t nIds = Get<uint16_t>(p + 12);
  size_t pos = 14;
  if (n < pos + 8u * nIds + 4) { err = pd::tr("Tuile tronquée"); return false; }
  t.srcIds.resize(nIds);
  for (int k = 0; k < nIds; ++k, pos += 8) t.srcIds[k] = Get<int64_t>(p + pos);
  const uint32_t rawLen = Get<uint32_t>(p + pos);
  pos += 4;
  const int N = DepthTile::kN;
  const size_t want = (partial ? N / 8 : 0) + 4u * N + (plane ? N : 0);
  if (rawLen != want) { err = pd::tr("Tuile incohérente"); return false; }
  std::vector<uint8_t> raw(rawLen);
  mz_ulong len = rawLen;
  if (mz_uncompress(raw.data(), &len, p + pos, (mz_ulong)(n - pos)) != MZ_OK || len != rawLen) {
    err = "Tuile corrompue";
    return false;
  }
  const uint8_t* mask = partial ? raw.data() : nullptr;
  const uint8_t* planes = raw.data() + (partial ? N / 8 : 0);
  std::vector<int64_t> q(N);
  for (int j = 0; j < 256; ++j)
    for (int i = 0; i < 256; ++i) {
      const int k = j * 256 + i;
      const uint32_t u = (uint32_t)planes[k] | ((uint32_t)planes[N + k] << 8) |
                         ((uint32_t)planes[2 * N + k] << 16) | ((uint32_t)planes[3 * N + k] << 24);
      q[k] = Predict(q.data(), i, j) + UnZigZag(u);
      if (!mask || (mask[k >> 3] >> (k & 7) & 1)) {
        const int64_t v = q[k] * mul;
        if (DepthTile::plausible((double)v)) t.v[k] = (int32_t)v;
      }
    }
  if (plane) t.src.assign(planes + 4 * N, planes + 5 * N);
  return true;
}

}  // namespace pd

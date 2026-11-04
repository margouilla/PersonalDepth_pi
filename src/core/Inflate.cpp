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

// Décodeur deflate canonique (même principe que puff.c de M. Adler) :
// blocs stockés, Huffman fixes et dynamiques.
#include "Inflate.h"

#include <cstring>

namespace pd {
namespace {

struct Huff {
  uint16_t count[16];  // nombre de codes par longueur
  uint16_t sym[320];   // symboles triés par code
};

struct State {
  const uint8_t* in;
  size_t n, pos = 0;
  uint32_t bitbuf = 0;
  int bitcnt = 0;
  std::vector<uint8_t>* out;
  bool err = false;

  int bits(int need) {
    uint32_t v = bitbuf;
    while (bitcnt < need) {
      if (pos >= n) { err = true; return 0; }
      v |= (uint32_t)in[pos++] << bitcnt;
      bitcnt += 8;
    }
    bitbuf = v >> need;
    bitcnt -= need;
    return (int)(v & ((1u << need) - 1));
  }

  int decode(const Huff& h) {
    int code = 0, first = 0, index = 0;
    for (int len = 1; len <= 15; ++len) {
      code |= bits(1);
      if (err) return -1;
      const int count = h.count[len];
      if (code - count < first) return h.sym[index + (code - first)];
      index += count;
      first += count;
      first <<= 1;
      code <<= 1;
    }
    err = true;
    return -1;
  }
};

bool Build(Huff& h, const uint8_t* length, int n) {
  std::memset(h.count, 0, sizeof h.count);
  for (int s = 0; s < n; ++s) h.count[length[s]]++;
  if (h.count[0] == n) return true;  // aucun code (permis pour les distances)
  int left = 1;
  for (int len = 1; len <= 15; ++len) {
    left <<= 1;
    left -= h.count[len];
    if (left < 0) return false;  // sur-souscrit
  }
  uint16_t offs[16];
  offs[1] = 0;
  for (int len = 1; len < 15; ++len) offs[len + 1] = offs[len] + h.count[len];
  for (int s = 0; s < n; ++s)
    if (length[s]) h.sym[offs[length[s]]++] = (uint16_t)s;
  return true;
}

const uint16_t kLBase[29] = {3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31,
                             35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258};
const uint8_t kLExt[29] = {0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2,
                           3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0};
const uint16_t kDBase[30] = {1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129,
                             193, 257, 385, 513, 769, 1025, 1537, 2049, 3073,
                             4097, 6145, 8193, 12289, 16385, 24577};
const uint8_t kDExt[30] = {0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6,
                           7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13};

bool Codes(State& s, const Huff& lc, const Huff& dc) {
  std::vector<uint8_t>& o = *s.out;
  for (;;) {
    int sym = s.decode(lc);
    if (sym < 0) return false;
    if (sym < 256) {
      o.push_back((uint8_t)sym);
    } else if (sym == 256) {
      return true;
    } else {
      sym -= 257;
      if (sym >= 29) return false;
      const size_t len = kLBase[sym] + s.bits(kLExt[sym]);
      const int ds = s.decode(dc);
      if (ds < 0 || ds >= 30) return false;
      const size_t dist = kDBase[ds] + s.bits(kDExt[ds]);
      if (s.err || dist > o.size()) return false;
      size_t from = o.size() - dist;
      for (size_t k = 0; k < len; ++k) o.push_back(o[from + k]);
    }
  }
}

bool Stored(State& s) {
  s.bitbuf = 0;
  s.bitcnt = 0;  // alignement sur l'octet
  if (s.pos + 4 > s.n) return false;
  const unsigned len = s.in[s.pos] | (s.in[s.pos + 1] << 8);
  const unsigned nlen = s.in[s.pos + 2] | (s.in[s.pos + 3] << 8);
  s.pos += 4;
  if (len != (~nlen & 0xffffu) || s.pos + len > s.n) return false;
  s.out->insert(s.out->end(), s.in + s.pos, s.in + s.pos + len);
  s.pos += len;
  return true;
}

struct FixedTables {
  Huff lc, dc;
  FixedTables() {
    uint8_t l[288];
    int k = 0;
    for (; k < 144; ++k) l[k] = 8;
    for (; k < 256; ++k) l[k] = 9;
    for (; k < 280; ++k) l[k] = 7;
    for (; k < 288; ++k) l[k] = 8;
    Build(lc, l, 288);
    for (k = 0; k < 30; ++k) l[k] = 5;
    Build(dc, l, 30);
  }
};

bool Fixed(State& s) {
  static const FixedTables t;  // initialisation thread-safe (C++11)
  return Codes(s, t.lc, t.dc);
}

bool Dynamic(State& s) {
  static const uint8_t order[19] = {16, 17, 18, 0, 8, 7, 9, 6, 10, 5,
                                    11, 4, 12, 3, 13, 2, 14, 1, 15};
  const int nlen = s.bits(5) + 257, ndist = s.bits(5) + 1, ncode = s.bits(4) + 4;
  if (s.err || nlen > 286 || ndist > 30) return false;
  uint8_t lengths[320] = {0};
  for (int k = 0; k < ncode; ++k) lengths[order[k]] = (uint8_t)s.bits(3);
  Huff lencode, distcode;
  if (!Build(lencode, lengths, 19)) return false;
  int idx = 0;
  while (idx < nlen + ndist) {
    int sym = s.decode(lencode);
    if (sym < 0) return false;
    if (sym < 16) {
      lengths[idx++] = (uint8_t)sym;
    } else {
      uint8_t len = 0;
      int rep;
      if (sym == 16) {
        if (idx == 0) return false;
        len = lengths[idx - 1];
        rep = 3 + s.bits(2);
      } else if (sym == 17) {
        rep = 3 + s.bits(3);
      } else {
        rep = 11 + s.bits(7);
      }
      if (s.err || idx + rep > nlen + ndist) return false;
      while (rep--) lengths[idx++] = len;
    }
  }
  if (lengths[256] == 0) return false;
  if (!Build(lencode, lengths, nlen)) return false;
  if (!Build(distcode, lengths + nlen, ndist)) return false;
  return Codes(s, lencode, distcode);
}

}  // namespace

bool ZlibInflate(const uint8_t* src, size_t n, std::vector<uint8_t>& out, size_t expected) {
  out.clear();
  if (expected) out.reserve(expected);
  if (n < 6) return false;
  const unsigned cmf = src[0], flg = src[1];
  if ((cmf & 0x0f) != 8 || ((cmf << 8) | flg) % 31 || (flg & 0x20)) return false;
  State s;
  s.in = src;
  s.n = n - 4;  // adler32 final ignoré
  s.pos = 2;
  s.out = &out;
  int last;
  do {
    last = s.bits(1);
    const int type = s.bits(2);
    if (s.err) return false;
    bool ok = type == 0 ? Stored(s) : type == 1 ? Fixed(s) : type == 2 ? Dynamic(s) : false;
    if (!ok || s.err) return false;
  } while (!last);
  return true;
}

}  // namespace pd

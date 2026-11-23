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

// Tests du cœur (sans OpenCPN) : précision, recoloration sans recalcul, perfs.
#include <algorithm>
#include <cassert>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <map>
#include <cstring>
#include "sqlite3.h"
#include <random>

#include <filesystem>
#include <string>
#include "../src/core/BathyGrid.h"
#include "../src/core/BathyLayer.h"
#include "../src/core/Contours.h"
#include "../src/core/Projection.h"
#include "../src/core/VerticalRef.h"
#include "../src/core/Inflate.h"
#include "../src/core/TideFES.h"
#include "../src/core/TideField.h"
#include "../src/core/BathyDb.h"
#include "../src/core/TileCodec.h"
#include "../src/core/Lang.h"
#include "../src/core/Triangulate.h"
#include "../src/core/WebMerc.h"
#include <thread>
#include "../src/core/ColorRamp.h"
#include "../src/core/PointCloud.h"
#include "../src/core/Soundings.h"
#include "../src/core/WaterLevel.h"
#include "../src/core/SurveyShape.h"
#include "../src/core/GeoTiff.h"
#include "../src/core/Imagery.h"
#include "../src/core/GeoGrid.h"
#include "../src/core/LandMask.h"
#include "../src/core/Tracks.h"
#include "../src/core/SideScan.h"
#include "../src/core/Shoreline.h"
#include "../src/core/Json.h"
#include "../src/core/Sha256.h"
#include "../src/core/Archive.h"
#include "../src/core/Refmar.h"
#include "../src/core/TideGauge.h"
#include "../src/core/NoaaCoops.h"
#include "../src/core/Catalog.h"
#include "test_archives.inc"
#include <cstring>
#include "../src/core/Projection.h"

using namespace pd;
using clk = std::chrono::steady_clock;
static std::string Tmp(const char* n) {
  return (std::filesystem::temp_directory_path() / n).string();
}
static std::string Tmp(const std::string& n) { return Tmp(n.c_str()); }
static double ms(clk::time_point a) {
  return std::chrono::duration<double, std::milli>(clk::now() - a).count();
}
#define CHECK(c) do { if (!(c)) { std::printf("ÉCHEC %s:%d %s\n", __FILE__, __LINE__, #c); return 1; } } while (0)

// ---- GeoTIFF de test : écrit une image (bandes ou tuiles), compressions et
// prédicteurs au choix, petit- ou gros-boutien
namespace tiffw {
struct Img {
  int w = 0, h = 0, bits = 32, fmt = 3;  // 1 non signé, 2 signé, 3 réel
  std::vector<double> v;                 // w × h, ligne 0 en haut
  bool be = false, tiled = false;
  int bw = 16, bh = 16;                  // tuile, ou lignes par bande (bh)
  int comp = 1, pred = 1;                // 1, 5 (LZW), 8 (Deflate) ; prédicteur 1, 2, 3
  double sx = 1, sy = 1, x0 = 0, y0 = 0; // pas et coin nord-ouest
  std::vector<uint16_t> keys;            // GeoKeyDirectory (sans l'entête)
  std::vector<double> dbl;               // GeoDoubleParams
  std::string nodata;
};
static void put(std::vector<uint8_t>& b, uint64_t v, int n, bool be) {
  for (int k = 0; k < n; ++k) b.push_back((uint8_t)(v >> (8 * (be ? n - 1 - k : k))));
}
static std::vector<uint8_t> lzw(const std::vector<uint8_t>& d) {
  std::vector<uint8_t> out;
  uint32_t acc = 0; int nb = 0, width = 9, next = 258;
  auto emit = [&](int code) {
    acc = (acc << width) | (uint32_t)code; nb += width;
    while (nb >= 8) { out.push_back((uint8_t)(acc >> (nb - 8))); nb -= 8; }
  };
  std::map<std::pair<int, int>, int> dict;
  emit(256);
  int w = -1;
  for (uint8_t c : d) {
    if (w < 0) { w = c; continue; }
    auto it = dict.find({w, c});
    if (it != dict.end()) { w = it->second; continue; }
    emit(w);
    dict[{w, c}] = next++;
    if (next > 511 && width == 9) width = 10;
    if (next > 1023 && width == 10) width = 11;
    if (next > 2047 && width == 11) width = 12;
    if (next >= 4093) { emit(256); dict.clear(); next = 258; width = 9; }
    w = c;
  }
  if (w >= 0) emit(w);
  emit(257);
  if (nb > 0) out.push_back((uint8_t)(acc << (8 - nb)));
  return out;
}
static std::vector<uint8_t> zstore(const std::vector<uint8_t>& d) {  // zlib, blocs non compressés
  std::vector<uint8_t> o = {0x78, 0x01};
  size_t i = 0;
  do {
    const size_t n = std::min<size_t>(65535, d.size() - i);
    o.push_back(i + n >= d.size() ? 1 : 0);
    put(o, n, 2, false); put(o, n ^ 0xFFFF, 2, false);
    o.insert(o.end(), d.begin() + i, d.begin() + i + n);
    i += n;
  } while (i < d.size());
  uint32_t a = 1, b = 0;
  for (uint8_t c : d) { a = (a + c) % 65521; b = (b + a) % 65521; }
  put(o, (b << 16) | a, 4, true);
  return o;
}
// bloc (x0, y0, bw × rows) -> octets du fichier
static std::vector<uint8_t> block(const Img& m, int bx, int by, int bw, int rows) {
  const int nbytes = m.bits / 8;
  std::vector<uint8_t> raw;
  for (int y = 0; y < rows; ++y) {
    std::vector<uint8_t> row;
    std::vector<uint64_t> iv(bw);
    for (int x = 0; x < bw; ++x) {
      const int X = bx + x, Y = by + y;
      const double v = (X < m.w && Y < m.h) ? m.v[(size_t)Y * m.w + X] : 0;
      uint64_t u = 0;
      if (m.fmt == 3) {
        if (m.bits == 32) { float f = (float)v; uint32_t t; std::memcpy(&t, &f, 4); u = t; }
        else std::memcpy(&u, &v, 8);
      } else {
        u = (uint64_t)(int64_t)std::llround(v);
      }
      iv[x] = u;
    }
    if (m.pred == 2)  // différences (entiers), de droite à gauche
      for (int x = bw - 1; x > 0; --x) iv[x] = iv[x] - iv[x - 1];
    if (m.pred == 3) {  // octets regroupés (poids fort d'abord) puis différences
      std::vector<uint8_t> t((size_t)bw * nbytes);
      for (int x = 0; x < bw; ++x)
        for (int b = 0; b < nbytes; ++b) t[(size_t)b * bw + x] = (uint8_t)(iv[x] >> (8 * (nbytes - 1 - b)));
      for (size_t k = t.size() - 1; k > 0; --k) t[k] = (uint8_t)(t[k] - t[k - 1]);
      row = t;
    } else {
      for (int x = 0; x < bw; ++x) put(row, iv[x], nbytes, m.be);
    }
    raw.insert(raw.end(), row.begin(), row.end());
  }
  if (m.comp == 5) return lzw(raw);
  if (m.comp == 8) return zstore(raw);
  return raw;
}
static bool write(const std::string& path, const Img& m) {
  std::vector<std::vector<uint8_t>> blocks;
  if (m.tiled) {
    for (int by = 0; by < m.h; by += m.bh)
      for (int bx = 0; bx < m.w; bx += m.bw) blocks.push_back(block(m, bx, by, m.bw, m.bh));
  } else {
    for (int by = 0; by < m.h; by += m.bh) blocks.push_back(block(m, 0, by, m.w, std::min(m.bh, m.h - by)));
  }
  // disposition : entête, données des blocs, tableaux, IFD
  std::vector<uint8_t> f;
  f.push_back(m.be ? 'M' : 'I'); f.push_back(m.be ? 'M' : 'I');
  put(f, 42, 2, m.be); put(f, 0, 4, m.be);
  std::vector<uint32_t> off, cnt;
  for (auto& b : blocks) { off.push_back((uint32_t)f.size()); cnt.push_back((uint32_t)b.size()); f.insert(f.end(), b.begin(), b.end()); }
  struct Tag { int tag, typ; uint32_t count; std::vector<uint8_t> data; };
  std::vector<Tag> tags;
  auto t16 = [&](int tag, std::vector<uint64_t> v) { Tag t{tag, 3, (uint32_t)v.size(), {}}; for (auto x : v) put(t.data, x, 2, m.be); tags.push_back(t); };
  auto t32 = [&](int tag, std::vector<uint64_t> v) { Tag t{tag, 4, (uint32_t)v.size(), {}}; for (auto x : v) put(t.data, x, 4, m.be); tags.push_back(t); };
  auto tdbl = [&](int tag, std::vector<double> v) { Tag t{tag, 12, (uint32_t)v.size(), {}}; for (double x : v) { uint64_t u; std::memcpy(&u, &x, 8); put(t.data, u, 8, m.be); } tags.push_back(t); };
  t32(256, {(uint64_t)m.w}); t32(257, {(uint64_t)m.h}); t16(258, {(uint64_t)m.bits}); t16(259, {(uint64_t)m.comp});
  t16(262, {1});
  if (!m.tiled) { t32(273, std::vector<uint64_t>(off.begin(), off.end())); t32(278, {(uint64_t)m.bh}); t32(279, std::vector<uint64_t>(cnt.begin(), cnt.end())); }
  t16(277, {1}); t16(284, {1});
  if (m.pred != 1) t16(317, {(uint64_t)m.pred});
  if (m.tiled) { t32(322, {(uint64_t)m.bw}); t32(323, {(uint64_t)m.bh}); t32(324, std::vector<uint64_t>(off.begin(), off.end())); t32(325, std::vector<uint64_t>(cnt.begin(), cnt.end())); }
  t16(339, {(uint64_t)m.fmt});
  tdbl(33550, {m.sx, m.sy, 0});
  tdbl(33922, {0, 0, 0, m.x0, m.y0, 0});
  if (!m.keys.empty()) {
    std::vector<uint64_t> k = {1, 1, 0, (uint64_t)(m.keys.size() / 4)};
    k.insert(k.end(), m.keys.begin(), m.keys.end());
    t16(34735, k);
  }
  if (!m.dbl.empty()) tdbl(34736, m.dbl);
  if (!m.nodata.empty()) { Tag t{42113, 2, (uint32_t)m.nodata.size() + 1, std::vector<uint8_t>(m.nodata.begin(), m.nodata.end())}; t.data.push_back(0); tags.push_back(t); }
  std::sort(tags.begin(), tags.end(), [](const Tag& a, const Tag& b) { return a.tag < b.tag; });
  for (auto& t : tags) if (t.data.size() > 4) { const uint32_t o = (uint32_t)f.size(); f.insert(f.end(), t.data.begin(), t.data.end()); if (f.size() & 1) f.push_back(0); t.data.clear(); put(t.data, o, 4, m.be); t.typ = -t.typ; }
  const uint32_t ifd = (uint32_t)f.size();
  put(f, tags.size(), 2, m.be);
  for (auto& t : tags) {
    put(f, t.tag, 2, m.be); put(f, (uint64_t)std::abs(t.typ), 2, m.be); put(f, t.count, 4, m.be);
    std::vector<uint8_t> d = t.data; d.resize(4, 0); f.insert(f.end(), d.begin(), d.end());
  }
  put(f, 0, 4, m.be);
  for (int k = 0; k < 4; ++k) f[4 + k] = (uint8_t)(ifd >> (8 * (m.be ? 3 - k : k)));
  std::ofstream o(path, std::ios::binary);
  o.write((const char*)f.data(), (std::streamsize)f.size());
  return (bool)o;
}
}  // namespace tiffw


int main(int argc, char** argv) {
  {  // base d'imagerie (.pdi) : mosaïque 8 bits à fond blanc, étirement, moyenne, valeurs d'origine
    std::string err;
    const std::string folder = Tmp("pd_test_img");
    std::filesystem::remove_all(folder);
    tiffw::Img im;
    im.w = 300; im.h = 200; im.bits = 8; im.fmt = 1; im.tiled = true; im.bw = 64; im.bh = 64; im.comp = 5;
    im.sx = 2e-5; im.sy = 2e-5; im.x0 = -4.50; im.y0 = 48.20;
    im.keys = {1024, 0, 1, 2, 2048, 0, 1, 4326};
    size_t inside = 0;
    for (int y = 0; y < im.h; ++y)
      for (int x = 0; x < im.w; ++x) {
        const bool in = x > 40 && x < 260 && y > 30 && y < 170;  // levé au milieu, fond blanc autour
        im.v.push_back(in ? 20 + (x * 7 + y * 3) % 200 : 255);
        inside += in;
      }
    const std::string f = Tmp("pd_test_img.tif");
    CHECK(tiffw::write(f, im));
    BathyDb img;
    CHECK(img.open(folder, err, 0, DbKind::Imagery));
    DbSource m;
    m.name = "IMG"; m.date = "2000-05-01";
    int64_t id = 0;
    ImageryInfo inf;
    CHECK(ImportImagery(img, f, ImageryOptions{}, m, DbPriority::ResolutionDate, err, nullptr, nullptr, &id, &inf));
    const DbSource* s = img.source(id);
    CHECK(s && inf.hasBackground && inf.backgroundAuto && inf.background == 255 && inf.pixels == (long long)inside);
    CHECK(s->vunit == pd::tr("niveau") && s->vlo >= 20 && s->vhi <= 219 && s->vhi > s->vlo + 100);
    // fichiers .pdi seulement ; la base de bathymétrie du même dossier reste vide
    bool pdi = false, pdb = false;
    for (auto& e : std::filesystem::directory_iterator(folder)) {
      pdi |= e.path().extension() == ".pdi";
      pdb |= e.path().extension() == ".pdb";
    }
    CHECK(pdi && !pdb);
    BathyDb dep;
    CHECK(dep.open(folder, err) && dep.sources().empty());
    // vue : valeurs ramenées à 0 – 1, fond absent ; valeur d'origine = vlo + n (vhi - vlo)
    PointCloud v;
    double dl, dt;
    int used;
    std::vector<int64_t> sid;
    CHECK(img.buildView(-4.4995, -4.4945, 48.1975, 48.1985, s->level, 20000000, v, dl, dt, used, err, nullptr, &sid));
    float nmin = 9, nmax = -9;
    for (size_t k = 0; k < v.size(); ++k) { nmin = std::min(nmin, v.z[k]); nmax = std::max(nmax, v.z[k]); }
    const double back = s->vlo + v.z[0] * (s->vhi - s->vlo);
    std::printf("imagerie : %lld pixels, fond %g, étirement %g – %g %s, niveau %d, vue %.2f … %.2f\n", inf.pixels,
                inf.background, s->vlo, s->vhi, s->vunit.c_str(), s->level, nmin, nmax);
    CHECK(nmin > -0.2f && nmax < 1.2f && back >= 20 && back <= 219 && sid[0] == id);
    // niveau plus grossier : moyenne (et non la plus faible valeur)
    CHECK(img.buildView(-4.4995, -4.4945, 48.1975, 48.1985, s->level - 3, 20000000, v, dl, dt, used, err));
    double mean = 0;
    for (float z : v.z) mean += z;
    mean /= std::max<size_t>(1, v.size());
    CHECK(used == s->level - 3 && mean > 0.25 && mean < 0.75);
    std::printf("Base d'imagerie .pdi (fond écarté, étirement, moyenne) : OK\n");
  }
  {  // Sonar latéral SL3 (canal 5) : mosaïque géoréférencée
    // Bateau vers le nord à 1 m/s, GPS à 1 Hz, pings à 10 Hz, 10 m d'eau,
    // portée ±40 m ; fond 60 jusqu'à 30 m au sol, bruit (5) au-delà ; cible
    // brillante à tribord, 20 m au sol, 100 m après le départ ; un parasite
    // sur un seul ping à bâbord.
    const std::string f = Tmp("pd_sidescan.sl3");
    const double lon0 = -4.5, lat0 = 48.3, R = 6356752.3142, depth = 10, range = 40;
    const int n = 400, pings = 2000;
    {
      std::vector<uint8_t> F(8, 0);
      const uint16_t hdr[4] = {3, 1, 3200, 0};
      std::memcpy(F.data(), hdr, 8);
      uint32_t idx = 0;
      for (int k = 0; k < pings; ++k) {
        const int gps = k / 10;  // position du dernier point GPS (1 Hz)
        const double lat = lat0 + gps * 1.0 / 111194.92664;
        std::vector<uint8_t> h(168 + 2 * n, 0);
        const uint32_t pos = (uint32_t)F.size();
        const uint16_t size = (uint16_t)h.size(), ch = 5, pk = (uint16_t)(2 * n);
        const float up = (float)(-range / 0.3048), lo = (float)(range / 0.3048), ft = (float)(depth / 0.3048);
        const int32_t e = (int32_t)std::lround(lon0 * M_PI / 180 * R);
        const int32_t nn = (int32_t)std::lround(R * std::log(std::tan(M_PI / 4 + lat * M_PI / 360)));
        const int32_t ms = k * 100;
        const float cog = 0.f;
        std::memcpy(&h[0], &pos, 4); std::memcpy(&h[8], &size, 2); std::memcpy(&h[12], &ch, 2);
        std::memcpy(&h[16], &idx, 4); std::memcpy(&h[20], &up, 4); std::memcpy(&h[24], &lo, 4);
        std::memcpy(&h[44], &pk, 2); std::memcpy(&h[48], &ft, 4); std::memcpy(&h[92], &e, 4);
        std::memcpy(&h[96], &nn, 4); std::memcpy(&h[104], &cog, 4); std::memcpy(&h[124], &ms, 4);
        ++idx;
        const double along = k * 0.1;
        for (int side = 0; side < 2; ++side)
          for (int j = 0; j < n; ++j) {
            const double r = (j + 0.5) * range / n;
            const double g = r > depth ? std::sqrt(r * r - depth * depth) : -1;
            int v = 20;  // colonne d'eau
            if (g >= 0) v = g < 30 ? 55 + (int)((j * 7 + k * 13) % 11) : 4 + (j + k) % 3;
            if (side == 1 && std::fabs(g - 20) < 1.0 && std::fabs(along - 100) < 1.5) v = 250;
            if (side == 0 && k == 500 && j > 150 && j < 170) v = 255;  // parasite
            h[168 + (side == 0 ? n - 1 - j : n + j)] = (uint8_t)v;
          }
        F.insert(F.end(), h.begin(), h.end());
      }
      FILE* o = std::fopen(f.c_str(), "wb");
      std::fwrite(F.data(), 1, F.size(), o);
      std::fclose(o);
    }
    CHECK(IsSonarLogFile(f));
    SideScanMosaic m;
    SideScanInfo si;
    std::string err;
    CHECK(BuildSideScanMosaic(f, SideScanOptions{}, m, si, err));
    std::printf("sonar latéral : %lld pings, portée %.1f m, pixel %.2f m, route %.3f km, %lld pixels, étirement %.1f – %.1f\n",
                si.used, si.rangeM, si.pixel, si.lengthKm, si.pixels, si.lo, si.hi);
    CHECK(si.kind == "SL3" && si.pings == pings && si.used > pings - 20 && std::fabs(si.rangeM - range) < 0.1);
    CHECK(std::fabs(si.pixel - 0.1) < 1e-9 && std::fabs(si.lengthKm - 0.1999) < 0.002);
    CHECK(si.picked > pings - 50);  // hauteur lue sur l'image (premier écho du fond)
    // cible : centre des pixels brillants à 20 m à l'est de la route, 100 m au nord
    const double kx = 111194.92664 * std::cos(lat0 * M_PI / 180);
    double sx = 0, sy = 0, cnt = 0, far = 0, port500 = 0;
    m.forEach([&](double lon, double lat, float v) {
      const double x = (lon - lon0) * kx, y = (lat - lat0) * 111194.92664;
      if (std::fabs(x) > 33) ++far;  // au-delà de la portée utile (bruit coupé)
      if (v > 200) { sx += x; sy += y; ++cnt; }
      if (x < -5 && x > -20 && std::fabs(y - 50) < 0.3 && v > 200) ++port500;
    });
    CHECK(cnt > 50);
    const double tx = sx / std::max(1.0, cnt), ty = sy / std::max(1.0, cnt);
    std::printf("cible : %.0f pixels, x %.2f m, y %.2f m ; hors portée %.0f\n", cnt, tx, ty, far);
    CHECK(std::fabs(tx - 20) < 0.6 && std::fabs(ty - 100) < 0.8 && far == 0 && port500 == 0);
    // import dans la base d'imagerie
    const std::string folder = Tmp("pd_test_sss");
    std::filesystem::remove_all(folder);
    std::filesystem::create_directories(folder);
    BathyDb img;
    CHECK(img.open(folder, err, 0, DbKind::Imagery));
    DbSource ms;
    ms.name = "SSS";
    ms.date = "2024-08-28";
    int64_t id = 0;
    CHECK(ImportSideScan(img, f, SideScanOptions{}, ms, DbPriority::ResolutionDate, err, nullptr, nullptr, &id));
    const DbSource* d = img.source(id);
    CHECK(d && d->vunit == pd::tr("niveau") && std::fabs(d->res - 0.1) < 1e-9 && d->vhi > d->vlo &&
          d->latMax > lat0 + 0.0015 && d->lonMax > lon0 + 0.0003);
    std::printf("Sonar latéral SL3 (mosaïque, distance au sol, parasites, portée utile) : OK\n");
  }
  {  // Sonar latéral : décalage antenne GPS / sonde estimé sur des lignes en sens opposés
    // Levé en allers-retours (4 lignes nord / sud, 20 m d'écart) ; la sonde est
    // 3 m en arrière de l'antenne et 1 m sur tribord ; fond texturé
    const std::string f = Tmp("pd_sidescan_lines.sl3");
    const double lon0 = -4.5, lat0 = 48.3, R = 6356752.3142, depth = 8, range = 40;
    const double kx = 111194.92664 * std::cos(lat0 * M_PI / 180), ky = 111194.92664;
    const double offF = -3, offS = 1;
    auto hash = [](int a, int b) {
      uint32_t h = (uint32_t)a * 73856093u ^ (uint32_t)b * 19349663u;
      h ^= h >> 13; h *= 0x5bd1e995u; h ^= h >> 15;
      return (h & 0xffff) / 65535.0;
    };
    auto field = [&](double x, double y) {  // bruit de valeurs, mailles de 1,5 m
      const double u = x / 1.5, v = y / 1.5;
      const int i = (int)std::floor(u), j = (int)std::floor(v);
      const double a = u - i, b = v - j;
      const double h = hash(i, j) * (1 - a) * (1 - b) + hash(i + 1, j) * a * (1 - b) +
                       hash(i, j + 1) * (1 - a) * b + hash(i + 1, j + 1) * a * b;
      return 40 + 80 * h;
    };
    const int n = 400;
    {
      std::vector<uint8_t> F(8, 0);
      const uint16_t hdr[4] = {3, 1, 3200, 0};
      std::memcpy(F.data(), hdr, 8);
      uint32_t idx = 0;
      int ms = 0;
      for (int line = 0; line < 4; ++line) {
        const bool north = line % 2 == 0;
        const double lx = line * 20.0;  // abscisse de la ligne (m, est)
        const double hdg = north ? 0 : M_PI;
        const double fx = std::sin(hdg), fy = std::cos(hdg), sx = std::cos(hdg), sy = -std::sin(hdg);
        for (int k = 0; k < 1500; ++k, ms += 100) {
          // sonde : x = lx, y le long de la ligne ; antenne = sonde − décalage
          const double ty = north ? k * 0.1 : 150 - k * 0.1;
          const int kg = (k / 10) * 10;  // dernier point GPS (1 Hz)
          const double gy = (north ? kg * 0.1 : 150 - kg * 0.1);
          const double ax = lx - offF * fx - offS * sx, ay = gy - offF * fy - offS * sy;
          const double lat = lat0 + ay / ky, lon = lon0 + ax / kx;
          std::vector<uint8_t> h(168 + 2 * n, 0);
          const uint32_t pos = (uint32_t)F.size();
          const uint16_t size = (uint16_t)h.size(), ch = 5, pk = (uint16_t)(2 * n);
          const float up = (float)(-range / 0.3048), lo = (float)(range / 0.3048), ft = (float)(depth / 0.3048);
          const int32_t e = (int32_t)std::lround(lon * M_PI / 180 * R);
          const int32_t nn = (int32_t)std::lround(R * std::log(std::tan(M_PI / 4 + lat * M_PI / 360)));
          const float cog = (float)hdg;
          std::memcpy(&h[0], &pos, 4); std::memcpy(&h[8], &size, 2); std::memcpy(&h[12], &ch, 2);
          std::memcpy(&h[16], &idx, 4); std::memcpy(&h[20], &up, 4); std::memcpy(&h[24], &lo, 4);
          std::memcpy(&h[44], &pk, 2); std::memcpy(&h[48], &ft, 4); std::memcpy(&h[92], &e, 4);
          std::memcpy(&h[96], &nn, 4); std::memcpy(&h[104], &cog, 4); std::memcpy(&h[124], &ms, 4);
          ++idx;
          for (int side = 0; side < 2; ++side)
            for (int j = 0; j < n; ++j) {
              const double r = (j + 0.5) * range / n;
              int v = 15;
              if (r > depth) {
                const double g = std::sqrt(r * r - depth * depth), sg = side == 0 ? -1 : 1;
                v = g < 30 ? (int)field(lx + sg * g * sx, ty + sg * g * sy) : 4;
              }
              h[168 + (side == 0 ? n - 1 - j : n + j)] = (uint8_t)v;
            }
          F.insert(F.end(), h.begin(), h.end());
        }
        ms += 20000;  // demi-tour
      }
      FILE* o = std::fopen(f.c_str(), "wb");
      std::fwrite(F.data(), 1, F.size(), o);
      std::fclose(o);
    }
    SideScanMosaic m;
    SideScanInfo si;
    std::string err;
    SideScanOptions so;
    so.pixel = 0.25;
    CHECK(BuildSideScanMosaic(f, so, m, si, err));
    std::printf("décalage estimé : avant %.2f m, tribord %.2f m (corrélation %.2f, %s)\n", si.offsetFwd, si.offsetStbd,
                si.offsetScore, si.offsetAuto ? "estimé" : "non estimé");
    CHECK(si.offsetAuto && std::fabs(si.offsetFwd - offF) < 0.35 && std::fabs(si.offsetStbd - offS) < 0.35);
    // mosaïque recalée : écart au fond vrai plus faible qu'avec un décalage nul
    auto err2 = [&](const SideScanMosaic& mm) {
      double e = 0, c = 0, mean = 0;
      mm.forEach([&](double lon, double lat, float v) { mean += v; ++c; });
      mean /= std::max(1.0, c);
      double fm = 0;
      long long fc = 0;
      mm.forEach([&](double lon, double lat, float) {
        fm += field((lon - lon0) * kx, (lat - lat0) * ky); ++fc; });
      fm /= std::max<long long>(1, fc);
      mm.forEach([&](double lon, double lat, float v) {
        const double x = (lon - lon0) * kx, y = (lat - lat0) * ky;
        if (y < 10 || y > 140) return;
        e += std::fabs((v - mean) - (field(x, y) - fm));
      });
      return e / std::max(1.0, c);
    };
    SideScanMosaic m0;
    SideScanInfo s0;
    so.autoOffset = false;
    CHECK(BuildSideScanMosaic(f, so, m0, s0, err) && s0.offsetFwd == 0 && !s0.offsetAuto);
    const double e1 = err2(m), e0 = err2(m0);
    std::printf("écart moyen au fond : %.1f recalé, %.1f sans décalage\n", e1, e0);
    CHECK(e1 < 0.7 * e0);
    std::printf("Sonar latéral : décalage antenne / sonde estimé sur les allers-retours : OK\n");
  }
  {  // Traces de sondeur : décalage sonde / antenne (patch test sur les profondeurs, valeurs saisies)
    // Allers-retours nord / sud tous les 15 m au-dessus d'une cuvette (pente
    // qui change de sens le long des lignes : sur une pente uniforme, le
    // décalage se confond avec un écart de niveau) ; sonde 3 m en arrière
    std::vector<double> x, y, h;
    std::vector<float> z;
    const double F = -3;
    auto Z = [](double xx, double yy) { return 3 + 10 * std::sin(M_PI * yy / 300) + 0.6 * std::sin(xx / 25); };
    for (int line = 0; line < 8; ++line) {
      const bool north = line % 2 == 0;
      for (int k = 0; k <= 300; ++k) {
        const double yt = north ? k : 300 - k, xt = line * 15.0;
        const double fy = north ? 1 : -1;
        x.push_back(xt);
        y.push_back(yt - F * fy);  // antenne = sonde − décalage
        z.push_back((float)Z(xt, yt));
        h.push_back(north ? 0 : M_PI);
      }
    }
    double off = 0, q = 0;
    CHECK(EstimateTrackOffset(x, y, z, h, off, q));
    std::printf("patch test (profondeurs) : décalage vers l'avant %.2f m (qualité %.1f)\n", off, q);
    CHECK(std::fabs(off - F) < 0.3);
    // fond plat : rien à estimer
    std::vector<float> zf(z.size(), 7.f);
    CHECK(!EstimateTrackOffset(x, y, zf, h, off, q));
    // valeurs saisies : trace GPX vers le nord, sonde 2 m en arrière
    const std::string gpx = Tmp("pd_trace_off.gpx");
    {
      FILE* f = std::fopen(gpx.c_str(), "wb");
      std::fputs("<?xml version=\"1.0\"?><gpx xmlns:gpxx=\"x\"><trk><trkseg>", f);
      for (int k = 0; k < 40; ++k)
        std::fprintf(f, "<trkpt lat=\"%.8f\" lon=\"-4.5\"><extensions><gpxx:Depth>5</gpxx:Depth></extensions></trkpt>",
                     48.3 + k / 111194.92664);
      std::fputs("</trkseg></trk></gpx>", f);
      std::fclose(f);
    }
    LoadOptions lo;
    lo.antFwd = -2;
    PointCloud pc;
    std::string err;
    CHECK(LoadTrack(gpx, lo, pc, err) && pc.size() == 40);
    double lat0 = 1e9;
    for (size_t k = 0; k < pc.size(); ++k) lat0 = std::min(lat0, pc.lat(k));
    CHECK(std::fabs((lat0 - 48.3) * 111194.92664 + 2) < 0.05 && pc.posInfo.find("-2.0") != std::string::npos);
    CHECK(!pc.antEstOk);  // saisi : pas d'estimation affichée
    std::printf("Traces : décalage sonde / antenne (patch test, valeurs saisies) : OK\n");
  }
  {  // grille en degrés à 51,6° N : maille 4,0 × 6,4 m, pixels de 2,96 m -> aucun trou entre les nœuds
    const std::string folder = Tmp("pd_test_gridstep");
    std::filesystem::remove_all(folder);
    std::string err;
    BathyDb db;
    CHECK(db.open(folder, err));
    PointCloud g;
    const double lat0 = 51.65, dla = 5.8e-5, dlo = 5.8e-5;  // grille « 5 m » Infomar
    for (int j = 0; j < 150; ++j)
      for (int i = 0; i < 150; ++i) g.add(-9.70 + i * dlo, lat0 + j * dla, 10.f + 0.01f * (i + j));
    g.computeBounds();
    SurveyShape sh = AnalyzeSurvey(g);
    CHECK(sh.isGrid() && sh.storeRes < 4.2);
    DbSource m;
    m.name = "G"; m.date = "2020-01-01"; m.res = sh.storeRes;
    m.gridStepX = sh.dx; m.gridStepY = sh.dy;
    int64_t id = 0;
    CHECK(db.importCloud(g, m, true, DbPriority::ResolutionDate, err, nullptr, nullptr, &id));
    const int lev = db.source(id)->level;
    PointCloud v;
    double dl, dt;
    int used;
    // intérieur de la grille (marge d'un nœud) : toutes les cellules remplies
    const double w = -9.70 + 2 * dlo, e = -9.70 + 147 * dlo, so = lat0 + 2 * dla, n = lat0 + 147 * dla;
    std::vector<uint8_t> vi;
    CHECK(db.buildView(w, e, so, n, lev, 20000000, v, dl, dt, used, err, nullptr, nullptr, &vi));
    const double cells = std::floor((e - w) / dl) * std::floor((n - so) / dt);
    // comblement linéaire : valeur = plan 10 + 0,01 (i + j) des nœuds, sans paliers
    double emax = 0;
    size_t nFill = 0, nInterp = 0;
    for (size_t k = 0; k < v.size(); ++k) {
      const double zi = 10.0 + 0.01 * ((v.lon(k) + 9.70) / dlo + (v.lat(k) - lat0) / dla);
      emax = std::max(emax, std::fabs(v.z[k] - zi));
      nFill += vi[k] == 2;
      nInterp += vi[k] == 1;
    }
    std::printf("grille 4,0 × 6,4 m au niveau %d : %zu cellules remplies sur ~%.0f, %zu comblées, écart au plan %.4f m\n",
                used, v.size(), cells, nFill, emax);
    CHECK(used == 15 && v.size() >= cells * 0.995 && nFill > v.size() / 4 && nInterp == 0);
    CHECK(emax < 0.0075);  // plus proche voisin (paliers) : ~0,01 ; pixel décalé du nœud : 0,006
    {  // tuile : drapeau « comblé » conservé à l'encodage
      DepthTile t;
      t.reset();
      t.v[5] = 1000; t.v[6] = 1100; t.setFill(6, true); t.v[7] = 1200; t.setInterp(7, true);
      std::vector<uint8_t> buf;
      CHECK(EncodeTile(t, buf));
      DepthTile r;
      CHECK(DecodeTile(buf.data(), buf.size(), r, err));
      CHECK(!r.isFill(5) && r.isFill(6) && !r.isInterp(6) && r.isInterp(7) && !r.isFill(7) && r.v[6] == 1100);
    }
    {  // grille d'affichage : cellules comblées jamais prises comme sondes
      BathyGrid gg;
      GridParams gp;
      gp.forceDLon = dl; gp.forceDLat = dt;
      CHECK(gg.build(v, gp, nullptr, err, &vi));
      size_t nf = 0;
      const GridLevel& g0 = gg.levels[0];
      for (size_t c = 0; c < g0.z.size(); ++c) nf += g0.isFill(c);
      CHECK(nf > 0 && !g0.isInterp(0));
    }
  }
  {  // lecteur GeoTIFF intégré : tuiles, bandes, LZW, Deflate, prédicteurs, systèmes
    std::string err;
    // 1. réels 32 bits en tuiles, géographique, NoData ; profondeurs en altitudes (signe auto)
    tiffw::Img a;
    a.w = 37; a.h = 21; a.tiled = true; a.bw = 16; a.bh = 16;
    a.sx = 0.001; a.sy = 0.001; a.x0 = -4.6; a.y0 = 48.4;
    a.keys = {1024, 0, 1, 2, 2048, 0, 1, 4326};
    a.nodata = "-9999";
    for (int y = 0; y < a.h; ++y)
      for (int x = 0; x < a.w; ++x) a.v.push_back((x + y) % 7 == 0 ? -9999 : -(1.0 + x * 0.25 + y));
    for (int variant = 0; variant < 4; ++variant) {  // sans compression, LZW, Deflate, gros-boutien + prédicteur 3
      tiffw::Img b = a;
      b.comp = variant == 1 ? 5 : variant == 2 ? 8 : variant == 3 ? 5 : 1;
      if (variant == 3) { b.be = true; b.pred = 3; }
      const std::string f = Tmp("pd_test_a.tif");
      CHECK(tiffw::write(f, b));
      PointCloud pc;
      double cl, ca;
      std::string used;
      CHECK(LoadGeoTiff(f, LoadOptions{}, pc, cl, ca, used, err));
      size_t expect = 0;
      for (double v : a.v) expect += v != -9999;
      CHECK(pc.size() == expect);
      CHECK(std::fabs(cl - 0.001) < 1e-12 && std::fabs(ca - 0.001) < 1e-12);
      // pixel (x=5, y=3) : centre -4.6 + 5.5e-3, 48.4 - 3.5e-3 ; valeur 1 + 1.25 + 3 (profondeur)
      bool found = false;
      for (size_t k = 0; k < pc.size(); ++k)
        if (std::fabs(pc.lon(k) - (-4.6 + 0.0055)) < 1e-7 && std::fabs(pc.lat(k) - (48.4 - 0.0035)) < 1e-7)
          found = std::fabs(pc.z[k] - 5.25f) < 1e-5;
      CHECK(found);
    }
    // 2. entiers 16 bits signés en bandes, LZW + prédicteur 2, UTM 30N (EPSG:32630)
    tiffw::Img u;
    u.w = 300; u.h = 40; u.bits = 16; u.fmt = 2; u.bh = 7; u.comp = 5; u.pred = 2;
    u.sx = 5; u.sy = 5; u.x0 = 400000; u.y0 = 5360000;
    u.keys = {1024, 0, 1, 1, 3072, 0, 1, 32630};
    for (int y = 0; y < u.h; ++y)
      for (int x = 0; x < u.w; ++x) u.v.push_back(100 + (x * 37 + y * 11) % 900);
    {
      const std::string f = Tmp("pd_test_u.tif");
      CHECK(tiffw::write(f, u));
      PointCloud pc;
      double cl, ca;
      std::string used;
      LoadOptions lo;
      lo.zsign = LoadOptions::ZSign::DepthPositive;
      CHECK(LoadGeoTiff(f, lo, pc, cl, ca, used, err));
      CHECK(pc.size() == (size_t)u.w * u.h && used == "EPSG:32630");
      CHECK(std::fabs(pc.srcStepX - 5) < 1e-9);
      Crs c;
      CHECK(Crs::Parse("EPSG:32630", c, err));
      double lon, lat;
      CHECK(c.toWGS84(400000 + 2.5 + 5 * 123, 5360000 - 2.5 - 5 * 17, lon, lat));
      size_t k = (size_t)17 * u.w + 123;  // ordre des points : lignes puis colonnes
      CHECK(std::fabs(pc.lon(k) - lon) < 2e-7 && std::fabs(pc.lat(k) - lat) < 2e-7);
      CHECK(pc.z[k] == (float)(100 + (123 * 37 + 17 * 11) % 900));
    }
    // 3. Mercator défini par l'utilisateur (parallèle standard 46°, comme l'Ifremer), 8 bits Deflate
    tiffw::Img mc;
    mc.w = 50; mc.h = 20; mc.bits = 8; mc.fmt = 1; mc.bh = 20; mc.comp = 8;
    mc.sx = 2.333; mc.sy = 2.333; mc.x0 = -366762.39; mc.y0 = 4253893.16;
    mc.keys = {1024, 0, 1, 1, 3072, 0, 1, 32767, 3075, 0, 1, 7, 3076, 0, 1, 9001,
               3080, 34736, 1, 1, 3081, 34736, 1, 0, 3082, 34736, 1, 3, 3083, 34736, 1, 4,
               3092, 34736, 1, 2, 2057, 34736, 1, 5, 2059, 34736, 1, 6};
    mc.dbl = {46.0, 0.0, 1.0, 0.0, 0.0, 6378137.0, 298.257223563};
    for (int k = 0; k < mc.w * mc.h; ++k) mc.v.push_back(1 + k % 200);
    {
      const std::string f = Tmp("pd_test_m.tif");
      CHECK(tiffw::write(f, mc));
      GeoTiff t;
      CHECK(t.open(f, err));
      CHECK(t.info().crsLabel == "Mercator 46°" && !t.info().geographic);
      PointCloud pc;
      double cl, ca;
      std::string used;
      LoadOptions lo;
      lo.zsign = LoadOptions::ZSign::DepthPositive;
      CHECK(LoadGeoTiff(f, lo, pc, cl, ca, used, err));
      // coin nord-ouest des métadonnées de l'Ifremer : -4.73466°, 48.228707°
      CHECK(std::fabs(pc.lon(0) - -4.73466) < 1e-4 && std::fabs(pc.lat(0) - 48.228707) < 1e-4);
    }
    std::printf("GeoTIFF intégré (tuiles, bandes, LZW, Deflate, prédicteurs 2 et 3, gros-boutien, UTM, Mercator) : OK\n");
  }
  {  // cellules interpolées près d'une cellule mesurée : pas éclaircies
    GridLevel g;
    g.nx = 10; g.ny = 3;
    g.z.assign(30, 1.f);
    g.interp.assign(30, 1);
    g.interp[10] = 0;  // mesurée (0, 1)
    g.markInterpNear();
    CHECK(!g.isInterp(10) && g.isInterp(11) && !g.isInterpFar(11) && !g.isInterpFar(2) &&
          !g.isInterpFar(22) && g.isInterpFar(3) && g.isInterpFar(29) && g.isInterp(29));
    std::printf("Interpolé près du mesuré non éclairci : OK\n");
  }
  size_t N = argc > 1 ? std::strtoul(argv[1], nullptr, 10) : 3000000;
  // PD_TEST_LIGHT : version allégée (émulation lente, ex. armhf sous QEMU)
  if (std::getenv("PD_TEST_LIGHT")) N = std::min<size_t>(N, 50000);
  const std::string xyzS = Tmp("pd_test.xyz");
  const char* xyz = xyzS.c_str();
  // Fond synthétique : pente 2..40 m, chenal, haut-fond, lignes de levé (trous)
  std::mt19937 rng(42);
  std::uniform_real_distribution<double> U(0, 1);
  FILE* f = std::fopen(xyz, "w");
  CHECK(f);
  std::fprintf(f, "# lon lat elevation (négatif = profondeur)\n");
  double refLon = 0, refLat = 0, refZ = 0;
  for (size_t k = 0; k < N; ++k) {
    double u = U(rng), v = U(rng);
    if (std::fmod(v * 40, 1.0) > 0.7) continue;  // lacunes entre profils
    double lon = -4.50 + 0.10 * u, lat = 48.30 + 0.06 * v;
    double z = 2 + 38 * u + 6 * std::exp(-std::pow((v - 0.5) / 0.05, 2));
    z -= 9 * std::exp(-(std::pow((u - 0.3) / 0.03, 2) + std::pow((v - 0.6) / 0.03, 2)));
    z = std::round(z * 100) / 100;  // 2 décimales comme un levé
    if (refZ == 0 && k > 700) { refLon = lon; refLat = lat; refZ = z; }
    std::fprintf(f, "%.7f %.7f %.2f\n", lon, lat, -z);
  }
  std::fclose(f);

  PointCloud pc;
  std::string err;
  auto t = clk::now();
  CHECK(LoadXYZ(xyz, LoadOptions{}, pc, err));
  double tLoad = ms(t);
  std::printf("Lecture XYZ : %zu sondes en %.0f ms (%.0f Mo en mémoire)\n", pc.size(), tLoad,
              pc.memoryBytes() / 1e6);
  CHECK(pc.zMin > -10 && pc.zMax > 30);  // signe auto : profondeurs positives

  // 1) La sonde d'origine est conservée à l'identique (au cm près)
  SoundingIndex idx;
  t = clk::now();
  idx.build(pc);
  std::printf("Index spatial : %.0f ms\n", ms(t));
  uint32_t k;
  CHECK(idx.nearest(pc, refLon, refLat, 1e-6, k));
  std::printf("Sonde témoin : fichier %.2f m / lue %.2f m / position écart %.2e°\n", refZ,
              pc.z[k], std::fabs(pc.lon(k) - refLon));
  CHECK(std::fabs(pc.z[k] - refZ) < 0.005);
  CHECK(std::fabs(pc.lon(k) - refLon) < 2e-7);

  // 2) Cache binaire
  t = clk::now();
  CHECK(pc.saveCache(Tmp("pd_test.pdpc").c_str()));
  PointCloud pc2;
  CHECK(pc2.loadCache(Tmp("pd_test.pdpc").c_str()));
  std::printf("Cache binaire écrit+relu : %.0f ms\n", ms(t));
  CHECK(pc2.size() == pc.size() && pc2.z[k] == pc.z[k]);

  // 3) Grilles : plus faible sonde, puis TIN
  for (GridMode m : {GridMode::Shoalest, GridMode::Tin}) {
    BathyGrid g;
    GridParams gp; gp.mode = m;
    t = clk::now();
    CHECK(g.build(pc, gp, nullptr, err));
    double tg = ms(t);
    t = clk::now();
    g.computeHillshade();
    size_t nan0 = 0;
    for (float v : g.levels[0].z) nan0 += std::isnan(v);
    std::printf("Grille %s : %dx%d, %zu niveaux, %.1f%% vides, %.0f ms (+ombrage %.0f ms)\n",
                m == GridMode::Tin ? "TIN" : "plus faible sonde", g.levels[0].nx,
                g.levels[0].ny, g.levels.size(), 100.0 * nan0 / g.levels[0].z.size(), tg, ms(t));
    if (m == GridMode::Shoalest) {
      // la cellule contient une valeur <= sonde témoin, et c'est une sonde réelle
      int i, j;
      CHECK(g.levels[0].cellOf(refLon, refLat, i, j));
      CHECK(g.levels[0].at(i, j) <= refZ + 1e-4);
      // pyramide : plus faible valeur conservée à tous les niveaux
      float mn0 = 1e9, mnL = 1e9;
      for (float v : g.levels[0].z) if (!std::isnan(v)) mn0 = std::min(mn0, v);
      for (float v : g.levels.back().z) if (!std::isnan(v)) mnL = std::min(mnL, v);
      CHECK(mn0 == mnL);
    } else {
      CHECK(nan0 < g.levels[0].z.size() / 10);  // trous entre profils comblés
    }

    // 4) Changement de palette : seule la LUT + recoloration est recalculée
    const GridLevel& L = g.levels[0];
    std::vector<uint32_t> img((size_t)L.nx * L.ny);
    for (const auto& name : ColorRamp::PresetNames()) {
      ColorRamp r = ColorRamp::Preset(name);
      t = clk::now();
      ColorLUT lut; lut.build(r, g.zMin, g.zMax);
      Colorize(L, 0, 0, L.nx, L.ny, lut, 0.6f, img.data(), L.nx);
      std::printf("  palette « %s » : %.1f ms pour %.1f M cellules\n", name.c_str(), ms(t),
                  img.size() / 1e6);
    }
  }

  // 5) Seuils : exactitude de la LUT autour d'un palier
  ColorRamp r = ColorRamp::Preset("Carte marine (paliers)");
  ColorLUT lut; lut.build(r, -10, 60);
  CHECK(lut(4.99f) == r.colorAt(4.99f) && lut(5.01f) == r.colorAt(5.01f));
  CHECK(r.colorAt(4.99f) != r.colorAt(5.01f));
  r.safetyOn = true; r.safetyDepth = 3.f;
  CHECK(r.colorAt(2.9f) != ColorRamp::Preset("Carte marine (paliers)").colorAt(2.9f));

  // 6) Sérialisation + CPT aller-retour
  ColorRamp a = ColorRamp::Preset("Sécurité tirant d'eau"), b;
  CHECK(b.deserialize(a.serialize()) && b == a);
  for (auto name : {"Carte marine (paliers)", "Arc-en-ciel"}) {
    ColorRamp c = ColorRamp::Preset(name), d;
    CHECK(c.saveCPT(Tmp("pd.cpt").c_str()));
    CHECK(d.loadCPT(Tmp("pd.cpt").c_str(), err));
    CHECK(d.mode == c.mode);
    for (float z : {-3.f, 0.5f, 4.f, 12.f, 33.f})
      CHECK(d.colorAt(z) == c.colorAt(z));
  }

  // 7) Sélection des sondes à l'écran (vue zoomée 800x600)
  BathyGrid g; GridParams gp; CHECK(g.build(pc, gp, nullptr, err));
  ScreenMapper sm;
  double lo[3] = {-4.47, -4.45, -4.47}, la[3] = {48.33, 48.33, 48.34};
  double xs[3] = {0, 800, 0}, ys[3] = {600, 600, 0};
  CHECK(sm.fit(lo, la, xs, ys));
  t = clk::now();
  auto labels = SelectSoundings(pc, idx, g, sm, -4.47, -4.45, 48.33, 48.34, 800, 600, 28);
  std::printf("Sondes affichées (zoom) : %zu en %.1f ms, réelles=%s\n", labels.size(), ms(t),
              labels.empty() || !labels[0].fromGrid ? "oui" : "non");
  CHECK(!labels.empty() && !labels[0].fromGrid);
  // vue large : bascule sur la grille si trop de sondes
  auto labels2 = SelectSoundings(pc, idx, g, sm, -4.6, -4.3, 48.2, 48.5, 800, 600, 28, 1000);
  std::printf("Sondes affichées (dézoom) : %zu, issues de la grille=%s\n", labels2.size(),
              !labels2.empty() && labels2[0].fromGrid ? "oui" : "non");

  // 8) ESRI ASCII et LandXML
  f = std::fopen(Tmp("pd.asc").c_str(), "w");
  std::fprintf(f, "ncols 4\nnrows 3\nxllcorner -4.5\nyllcorner 48.3\ncellsize 0.001\n"
                  "NODATA_value -9999\n1.25 2 3 4\n5 -9999 7 8\n9 10 11 12.75\n");
  std::fclose(f);
  PointCloud pa; double cl, ca;
  CHECK(LoadESRIAscii(Tmp("pd.asc").c_str(), LoadOptions{}, pa, cl, ca, err));
  BathyGrid ga; GridParams gpa; gpa.forceDLon = cl; gpa.forceDLat = ca;
  CHECK(ga.build(pa, gpa, nullptr, err));
  CHECK(ga.levels[0].nx == 4 && ga.levels[0].ny == 3);
  CHECK(ga.levels[0].at(0, 0) == 1.25f && ga.levels[0].at(3, 2) == 12.75f);
  CHECK(std::isnan(ga.levels[0].at(1, 1)));
  std::printf("ESRI ASCII : OK (4x3, valeurs exactes, NODATA respecté)\n");

  f = std::fopen(Tmp("pd.xml").c_str(), "w");
  std::fprintf(f, "<LandXML><Surface><Definition><Pnts>"
                  "<P id=\"1\">48.300 -4.500 -2.0</P><P id=\"2\">48.300 -4.490 -12.0</P>"
                  "<P id=\"3\">48.310 -4.500 -8.0</P><P id=\"4\">48.310 -4.490 -20.0</P>"
                  "</Pnts><Faces><F>1 2 3</F><F>2 4 3</F></Faces></Definition></Surface></LandXML>");
  std::fclose(f);
  PointCloud pt; std::vector<uint32_t> tri;
  CHECK(LoadLandXMLTin(Tmp("pd.xml").c_str(), LoadOptions{}, pt, tri, err));
  CHECK(pt.size() == 4 && tri.size() == 6 && pt.zMin == 2.f);
  BathyGrid gt; GridParams gpt; gpt.cellMeters = 20;
  CHECK(gt.build(pt, gpt, &tri, err));
  size_t nn = 0; for (float v : gt.levels[0].z) nn += std::isnan(v);
  std::printf("TIN LandXML : grille %dx%d, %zu cellules vides, centre %.2f m\n", gt.levels[0].nx,
              gt.levels[0].ny, nn, gt.levels[0].sampleBilinear(-4.495, 48.305));
  CHECK(nn == 0);
  // 9) Couche asynchrone + cache binaire
  {
    std::error_code ec;
    for (auto& e : std::filesystem::directory_iterator(std::filesystem::temp_directory_path()))
      if (e.path().extension() == ".pdpc") std::filesystem::remove(e.path(), ec);
    for (int pass = 0; pass < 2; ++pass) {
      LayerParams lp; lp.path = xyz;
      BathyLayer layer(lp);
      t = clk::now();
      layer.startLoad();
      while (layer.state() == BathyLayer::Loading) std::this_thread::sleep_for(std::chrono::milliseconds(5));
      CHECK(layer.state() == BathyLayer::Ready);
      std::printf("Couche (%s) : %.0f ms — %s\n", pass ? "depuis le cache" : "premier chargement",
                  ms(t), layer.summary().c_str());
      float gz, sz; double dist;
      CHECK(layer.probe(refLon, refLat, 1e-5, gz, sz, dist));
      CHECK(std::fabs(sz - refZ) < 0.005 && dist < 0.05);
    }
  }
  // 10) Fichier déjà maillé au pas 2e-5° (cas réel : levé PFE, 1,5 m x 2,2 m)
  {
    PointCloud g2;
    for (int j = 0; j < 400; ++j)
      for (int i = 0; i < 700; ++i) {
        if (i > 300 && i < 320) continue;  // une bande sans donnée
        double lon = -4.48951496 + i * 2e-5, lat = 48.32374186 - j * 2e-5;
        // arrondi à 8 décimales comme dans le fichier
        lon = std::round(lon * 1e8) / 1e8; lat = std::round(lat * 1e8) / 1e8;
        g2.add(lon, lat, 20.f + 0.01f * i);
      }
    g2.computeBounds();
    BathyGrid gg; GridParams gpp;
    CHECK(gg.build(g2, gpp, nullptr, err));
    const GridLevel& L0 = gg.levels[0];
    size_t holes = 0;
    for (int j = 0; j < L0.ny; ++j)
      for (int i = 0; i < L0.nx; ++i)
        if (std::isnan(L0.at(i, j)) && !(i > 300 && i < 320)) ++holes;
    std::printf("Grille native : détectée=%s, %dx%d, pas %.2e/%.2e°, trous parasites=%zu\n",
                gg.nativeLattice ? "oui" : "non", L0.nx, L0.ny, L0.dlon, L0.dlat, holes);
    CHECK(gg.nativeLattice && L0.nx == 700 && L0.ny == 400 && holes == 0);
    CHECK(std::fabs(L0.dlat - 2e-5) < 2e-9);
    // valeur exacte : une sonde par cellule
    int ci, cj;
    CHECK(L0.cellOf(-4.48951496 + 10 * 2e-5, 48.32374186 - 5 * 2e-5, ci, cj));
    CHECK(std::fabs(L0.at(ci, cj) - 20.1f) < 1e-4);

    // même fichier avec une cellule forcée de 2 m : repliement comblé
    BathyGrid gf; GridParams gpf; gpf.cellMeters = 2;
    CHECK(gf.build(g2, gpf, nullptr, err));
    const GridLevel& F0 = gf.levels[0];
    size_t fh = 0;
    for (int j = 1; j + 1 < F0.ny; ++j)
      for (int i = 0; i < F0.nx; ++i) {
        double lo = F0.cellLon(i);
        if (lo > -4.48951496 + 299 * 2e-5 && lo < -4.48951496 + 321 * 2e-5) continue;
        if (std::isnan(F0.at(i, j))) ++fh;
      }
    std::printf("Cellule 2 m forcée : %zu trous comblés, %zu restants\n", gf.filledGaps, fh);
    CHECK(gf.filledGaps > 0 && fh == 0);
  }

  // 11) Désencombrement des sondes : aucune superposition, priorité au moins profond
  {
    std::vector<Label> v;
    std::mt19937 r2(7);
    std::uniform_real_distribution<float> X(0, 800), Y(0, 600), Z(5, 30);
    for (int k = 0; k < 5000; ++k) v.push_back(Label{X(r2), Y(r2), Z(r2), false, 40.f});
    v.push_back(Label{400, 300, 1.5f, false, 40.f});  // la plus faible
    Declutter(v, 14.f, 4.f);
    bool shoal = false, overlap = false;
    for (size_t a = 0; a < v.size(); ++a) {
      if (v[a].depth == 1.5f) shoal = true;
      for (size_t b = a + 1; b < v.size(); ++b)
        if (std::fabs(v[a].x - v[b].x) < 44.f && std::fabs(v[a].y - v[b].y) < 18.f) overlap = true;
    }
    std::printf("Désencombrement : 5001 -> %zu étiquettes, chevauchement=%s, plus faible gardée=%s\n",
                v.size(), overlap ? "OUI" : "non", shoal ? "oui" : "NON");
    CHECK(!overlap && shoal && v.size() > 300);
  }
  // 12) Isobathes
  {
    GridLevel g; g.nx = 100; g.ny = 80; g.lonW = -4.5; g.latN = 48.4; g.dlon = 1e-4; g.dlat = 1e-4;
    g.z.resize((size_t)g.nx * g.ny);
    for (int j = 0; j < g.ny; ++j) for (int i = 0; i < g.nx; ++i) g.z[(size_t)j * g.nx + i] = (float)i;
    auto L = ComputeIsolines(g, {10.5f});
    CHECK(L.size() == 1 && !L[0].closed && (int)L[0].size() == g.ny);
    for (size_t k = 0; k < L[0].size(); ++k) CHECK(std::fabs(L[0].pts[2 * k] - (g.lonW + 11 * g.dlon)) < 1e-9);
    // trou au milieu : la ligne est coupée en deux, rien n'est inventé
    g.z[(size_t)40 * g.nx + 10] = NAN;
    L = ComputeIsolines(g, {10.5f});
    CHECK(L.size() == 2);
    // cône : cercles fermés, un par niveau
    for (int j = 0; j < g.ny; ++j) for (int i = 0; i < g.nx; ++i)
      g.z[(size_t)j * g.nx + i] = 2.f + 0.5f * std::hypot(i - 50.f, j - 40.f);
    auto lv = RegularLevels(2.f, 28.f, 5.f);
    CHECK(lv.size() == 5 && lv[0] == 5.f && lv.back() == 25.f);
    t = clk::now();
    L = ComputeIsolines(g, lv);
    int closed = 0; for (auto& l : L) closed += l.closed;
    std::printf("Isobathes cône : %zu lignes, %d fermées (niveaux 5,10,15,20 dans la grille)\n", L.size(), closed);
    CHECK(closed == 4);  // 25 m sort de la grille -> lignes ouvertes au bord
    for (auto& l : L) if (l.closed) {
      double r = std::hypot((l.pts[0] - g.lonW) / g.dlon - 0.5 - 50, (g.latN - l.pts[1]) / g.dlat - 0.5 - 40);
      CHECK(std::fabs(r - (l.level - 2.f) / 0.5f) < 0.6);
      Isoline sm = l; SmoothIsoline(sm, 2);
      CHECK(sm.closed && sm.size() > l.size() && sm.pts[0] == sm.pts[sm.pts.size() - 2]);
    }
    // performance sur la grille du levé synthétique 2,1 M sondes
    BathyGrid gb; GridParams gpb; CHECK(gb.build(pc, gpb, nullptr, err));
    t = clk::now();
    auto big = ComputeIsolines(gb.levels[0], RegularLevels(gb.zMin, gb.zMax, 1.f));
    size_t npts = 0; for (auto& l : big) npts += l.size();
    std::printf("Isobathes 1 m sur %dx%d : %zu lignes, %zu points, %.0f ms\n", gb.levels[0].nx,
                gb.levels[0].ny, big.size(), npts, ms(t));
  }
  // 13) Systèmes de coordonnées
  {
    Crs l93; CHECK(Crs::Parse("EPSG:2154", l93, err));
    double lo, la;
    CHECK(l93.toWGS84(700000, 6600000, lo, la));
    CHECK(std::fabs(lo - 3) < 1e-9 && std::fabs(la - 46.5) < 1e-9);
    Crs bad; CHECK(!Crs::Parse("EPSG:99999", bad, err));
    CHECK(!Crs::Parse("+proj=laea +lat_0=52 +lon_0=10", bad, err));
    // .prj ESRI sans autorité, WKT avec autorité, UTM
    CHECK(EpsgFromWkt("PROJCS[\"RGF93_Lambert_93\",GEOGCS[\"GCS_RGF_1993\"]]") == 2154);
    CHECK(EpsgFromWkt("PROJCS[\"WGS_1984_UTM_Zone_31N\"]") == 32631);
    CHECK(EpsgFromWkt("PROJCS[\"ETRS89 / UTM zone 30N\"]") == 25830);
    CHECK(EpsgFromWkt("PROJCRS[\"x\",ID[\"EPSG\",27700]]") == 27700);
    CHECK(EpsgFromWkt("GEOGCS[\"GCS_WGS_1984\"]") == 4326);
    CHECK(EpsgFromWkt("PROJCS[\"NAD_1983_StatePlane_Massachusetts_Mainland_FIPS_2001_Feet\",GEOGCS[\"GCS_North_American_1983\"]]") == 2249);
    CHECK(EpsgFromWkt("PROJCS[\"NAD_1983_StatePlane_Louisiana_North_FIPS_1701\",GEOGCS[\"GCS_North_American_1983\"]]") == 26981);
    CHECK(EpsgFromWkt("PROJCS[\"NAD_1983_StatePlane_California_VI_FIPS_0406_Feet\",GEOGCS[\"GCS_North_American_1983\"]]") == 2230);
    // .prj sans code EPSG reconnu : projection lue dans le WKT (QGIS, ESRI, WKT2)
    {
      auto wgs = [&](const std::string& wkt, double x, double y, double lon, double lat, double tol) {
        const std::string def = CrsDefFromPrj(wkt);
        Crs c;
        if (!Crs::Parse(def, c, err)) { std::printf("  WKT -> %s : %s\n", def.c_str(), err.c_str()); return false; }
        double lo, la;
        if (!c.toWGS84(x, y, lo, la)) return false;
        const double d = std::hypot((lo - lon) * 111320 * std::cos(lat * M_PI / 180), (la - lat) * 110540);
        if (d > tol) std::printf("  WKT -> %s : écart %.2f m\n", def.c_str(), d);
        return d <= tol;
      };
      // EPSG:3395 exporté par QGIS (ESRI, sans autorité) ; références pyproj
      CHECK(wgs("PROJCS[\"WGS_1984_World_Mercator\",GEOGCS[\"GCS_WGS_1984\",DATUM[\"D_WGS_1984\",SPHEROID[\"WGS_1984\",6378137.0,298.257223563]],PRIMEM[\"Greenwich\",0.0],UNIT[\"Degree\",0.0174532925199433]],PROJECTION[\"Mercator\"],PARAMETER[\"False_Easting\",0.0],PARAMETER[\"False_Northing\",0.0],PARAMETER[\"Central_Meridian\",0.0],PARAMETER[\"Standard_Parallel_1\",0.0],UNIT[\"Meter\",1.0]]",
                11633044.5, 6800000, 104.501416752, 52.185733397, 0.01));
      // Mercator personnalisé (parallèle standard 52°, méridien 105°)
      CHECK(wgs("PROJCS[\"Mercator_Baikal\",GEOGCS[\"GCS_WGS_1984\",DATUM[\"D_WGS_1984\",SPHEROID[\"WGS_1984\",6378137.0,298.257223563]],PRIMEM[\"Greenwich\",0.0],UNIT[\"Degree\",0.0174532925199433]],PROJECTION[\"Mercator\"],PARAMETER[\"False_Easting\",0.0],PARAMETER[\"False_Northing\",0.0],PARAMETER[\"Central_Meridian\",105.0],PARAMETER[\"Standard_Parallel_1\",52.0],UNIT[\"Meter\",1.0]]",
                -30000, 3900000, 104.563178995, 49.463997738, 0.01));
      // WKT2 sans identifiant (Mercator variante A)
      CHECK(wgs("PROJCRS[\"WGS 84 / World Mercator\",BASEGEOGCRS[\"WGS 84\",DATUM[\"World Geodetic System 1984\",ELLIPSOID[\"WGS 84\",6378137,298.257223563,LENGTHUNIT[\"metre\",1]]],PRIMEM[\"Greenwich\",0]],CONVERSION[\"World Mercator\",METHOD[\"Mercator (variant A)\"],PARAMETER[\"Latitude of natural origin\",0],PARAMETER[\"Longitude of natural origin\",0],PARAMETER[\"Scale factor at natural origin\",1],PARAMETER[\"False easting\",0],PARAMETER[\"False northing\",0]],CS[Cartesian,2],AXIS[\"(E)\",east,ORDER[1],LENGTHUNIT[\"metre\",1]],AXIS[\"(N)\",north,ORDER[2],LENGTHUNIT[\"metre\",1]]]",
                11633044.5, 6800000, 104.501416752, 52.185733397, 0.01));
      // Pulkovo 1942 / Gauss-Krüger zone 18 (datum sans TOWGS84 : paramètres moyens, quelques m)
      CHECK(wgs("PROJCS[\"Pulkovo 1942 / Gauss-Kruger zone 18\",GEOGCS[\"Pulkovo 1942\",DATUM[\"Pulkovo_1942\",SPHEROID[\"Krassowsky 1940\",6378245,298.3]],PRIMEM[\"Greenwich\",0],UNIT[\"degree\",0.0174532925199433]],PROJECTION[\"Transverse_Mercator\"],PARAMETER[\"latitude_of_origin\",0],PARAMETER[\"central_meridian\",105],PARAMETER[\"scale_factor\",1],PARAMETER[\"false_easting\",18500000],PARAMETER[\"false_northing\",0],UNIT[\"metre\",1]]",
                18400000, 5760000, 103.545457586, 51.960667634, 15));
      // Web Mercator ESRI (sphère auxiliaire)
      CHECK(wgs("PROJCS[\"WGS_1984_Web_Mercator_Auxiliary_Sphere\",GEOGCS[\"GCS_WGS_1984\",DATUM[\"D_WGS_1984\",SPHEROID[\"WGS_1984\",6378137.0,298.257223563]],PRIMEM[\"Greenwich\",0.0],UNIT[\"Degree\",0.0174532925199433]],PROJECTION[\"Mercator_Auxiliary_Sphere\"],PARAMETER[\"False_Easting\",0.0],PARAMETER[\"False_Northing\",0.0],PARAMETER[\"Central_Meridian\",0.0],PARAMETER[\"Standard_Parallel_1\",0.0],PARAMETER[\"Auxiliary_Sphere_Type\",0.0],UNIT[\"Meter\",1.0]]",
                11633044.5, 6800000, 104.501416752, 51.999306159, 0.01));
      CHECK(CrsDefFromPrj("GEOGCS[\"GCS_WGS_1984\",DATUM[\"D_WGS_1984\",SPHEROID[\"WGS_1984\",6378137.0,298.257223563]]]").empty());
      CHECK(CrsDefFromWkt("PROJCS[\"x\",PROJECTION[\"Bonne\"]]").empty());
    }
    for (auto& e : CrsCatalog()) {  // chaque entrée du catalogue est valide
      Crs c; CHECK(Crs::Parse(CrsDefinition(e, e.zoneMin ? e.zoneMin : 1, e.zoneSouthDefault), c, err));
    }
    std::printf("Catalogue : %zu systèmes valides\n", CrsCatalog().size());
    {  // NAD83 State Plane en pieds US : l'origine de la projection redonne lat_0 / lon_0
      Crs sp;
      CHECK(Crs::Parse("EPSG:2249", sp, err));  // Massachusetts Mainland (ftUS)
      double lo, la;
      const double ft = 1200.0 / 3937.0;
      CHECK(sp.toWGS84(200000.0001016002 / ft, 750000.0 / ft, lo, la));
      CHECK(std::fabs(lo + 71.5) < 1e-7 && std::fabs(la - 41.0) < 1e-7);
      CHECK(Crs::Parse("EPSG:26958", sp, err));  // Florida East (m), Mercator transverse
      CHECK(sp.toWGS84(200000.0, 0.0, lo, la) && std::fabs(lo + 81.0) < 1e-7 &&
            std::fabs(la - 24.3333333333) < 1e-6);
    }

    // Levé maillé à 2 m en Lambert-93 (rade de Brest), lu en EPSG:2154
    const std::string l93f = Tmp("pd_l93.xyz");
    FILE* fl = std::fopen(l93f.c_str(), "w");
    double x0, y0;
    CHECK(l93.fromWGS84(-4.49, 48.33, x0, y0));
    x0 = std::round(x0); y0 = std::round(y0);
    for (int j = 0; j < 300; ++j)
      for (int i = 0; i < 400; ++i)
        std::fprintf(fl, "%.2f %.2f %.2f\n", x0 + 2 * i, y0 + 2 * j, -(10.0 + 0.01 * i));
    std::fclose(fl);
    // lu par erreur en WGS84 : message d'aide
    PointCloud pw; LoadOptions lw;
    CHECK(!LoadXYZ(l93f, lw, pw, err) && err.find("Lambert-93") != std::string::npos);
    std::printf("Message si mauvais système : %s\n", err.c_str());
    LayerParams lp; lp.path = l93f; lp.crsDef = "EPSG:2154"; lp.useCache = false;
    BathyLayer layer(lp);
    t = clk::now();
    layer.startLoad();
    while (layer.state() == BathyLayer::Loading) std::this_thread::sleep_for(std::chrono::milliseconds(5));
    CHECK(layer.state() == BathyLayer::Ready);
    std::printf("Lambert-93 : %s (%.0f ms)\n", layer.summary().c_str(), ms(t));
    CHECK(std::fabs(layer.cloud.srcStepX - 2) < 1e-6 && std::fabs(layer.cloud.srcStepY - 2) < 1e-6);
    CHECK(layer.cloud.size() == 120000);
    double tl, ta; CHECK(l93.toWGS84(x0 + 2 * 100, y0 + 2 * 50, tl, ta));
    float gz, sz; double dist;
    CHECK(layer.probe(tl, ta, 1e-6, gz, sz, dist) && std::fabs(sz - 11.0f) < 1e-4 && dist < 0.02);
    const GridLevel& L0 = layer.grid.levels[0];
    size_t holes = 0, tot = 0;
    for (int j = 2; j + 2 < L0.ny; ++j)
      for (int i = 2; i + 2 < L0.nx; ++i) {
        double lo2, la2, xx, yy;
        lo2 = L0.cellLon(i); la2 = L0.cellLat(j);
        l93.fromWGS84(lo2, la2, xx, yy);
        if (xx < x0 + 4 || xx > x0 + 794 || yy < y0 + 4 || yy > y0 + 594) continue;  // intérieur
        ++tot; holes += std::isnan(L0.at(i, j));
      }
    std::printf("Grille issue du Lambert-93 : %zu cellules intérieures, %zu vides\n", tot, holes);
    CHECK(holes * 200 < tot);  // < 0,5 %
    // .prj voisin prioritaire
    FILE* fp = std::fopen((l93f.substr(0, l93f.size() - 4) + ".prj").c_str(), "w");
    std::fprintf(fp, "PROJCS[\"RGF93_Lambert_93\"]");
    std::fclose(fp);
    LayerParams lp2; lp2.path = l93f; lp2.useCache = false;
    BathyLayer layer2(lp2);
    layer2.startLoad();
    while (layer2.state() == BathyLayer::Loading) std::this_thread::sleep_for(std::chrono::milliseconds(5));
    CHECK(layer2.state() == BathyLayer::Ready && layer2.crsUsed() == "EPSG:2154 (.prj)");
    std::remove((l93f.substr(0, l93f.size() - 4) + ".prj").c_str());
  }
  // 14) Référence verticale : C = h_ZH - N, profondeur_ZH = profondeur + C
  {
    // grille « géoïde » .tac : N = 50 + 0,1*(lon+5) (2 valeurs par nœud)
    const std::string tac = Tmp("pd_geoid.tac"), zhf = Tmp("pd_zh.glhi");
    FILE* ft = std::fopen(tac.c_str(), "w");
    std::fprintf(ft, "-5.0 -4.0 48.0 49.0 0.1 0.1 2 0 1 1 0. essai\n");
    for (int j = 0; j <= 10; ++j)      // du nord au sud
      for (int i = 0; i <= 10; ++i)    // longitude la plus rapide
        std::fprintf(ft, "%.4f 99%c", 50 + 0.1 * (i * 0.1), i == 10 ? '\n' : ' ');
    std::fclose(ft);
    Grid2D gt; CHECK(LoadTacGrid(tac, gt, err));
    CHECK(std::fabs(gt.sample(-4.5, 48.3) - 50.05) < 1e-6);
    // surface ZH en points dispersés : h_ZH = 46,4 (constante)
    FILE* fz = std::fopen(zhf.c_str(), "w");
    std::mt19937 r3(3);
    std::uniform_real_distribution<double> U2(0, 1);
    for (int k = 0; k < 300; ++k)
      std::fprintf(fz, "%.6f %.6f %.3f 0.1\n", -5.0 + U2(r3), 48.0 + U2(r3), 46.4);
    std::fclose(fz);
    VerticalRefParams vp; vp.mode = VerticalRefParams::Mode::Surfaces;
    vp.zhFiles = {zhf}; vp.refGrid = tac;
    VerticalCorrection vc;
    CHECK(vc.build(vp, -4.6, -4.4, 48.3, 48.5, err));
    const double C = vc.at(-4.5, 48.4);   // 46,4 - 50,05 = -3,65
    std::printf("Référence verticale : C(-4.5, 48.4) = %.4f m (attendu -3.6500)\n", C);
    CHECK(std::fabs(C + 3.65) < 1e-3);
    PointCloud pv; pv.add(-4.5, 48.4, 10.f); pv.computeBounds();
    float cmn, cmx; CHECK(vc.apply(pv, cmn, cmx) == 0 && std::fabs(pv.z[0] - 6.35f) < 1e-3);
    // hauteurs ellipsoïdales : pas de géoïde, C = h_ZH
    VerticalRefParams ve; ve.mode = VerticalRefParams::Mode::Surfaces; ve.zhFiles = {zhf};
    VerticalCorrection vce;
    CHECK(vce.build(ve, -4.6, -4.4, 48.3, 48.5, err) && std::fabs(vce.at(-4.5, 48.4) - 46.4) < 1e-3);
    // grille d'écart directe, signe inversé
    VerticalRefParams vd; vd.mode = VerticalRefParams::Mode::DirectGrid;
    vd.directFile = zhf; vd.directInvert = true;
    VerticalCorrection vc2; CHECK(vc2.build(vd, -4.6, -4.4, 48.3, 48.5, err));
    CHECK(std::fabs(vc2.at(-4.5, 48.4) + 46.4) < 1e-3);
    // .asc et .gtx
    const std::string ascf = Tmp("pd_geoid.asc");
    FILE* fa = std::fopen(ascf.c_str(), "w");
    std::fprintf(fa, "ncols 3\nnrows 2\nxllcorner 0\nyllcorner 0\ncellsize 1\nNODATA_value -9999\n1 2 3\n4 5 6\n");
    std::fclose(fa);
    Grid2D ga; CHECK(LoadAscGrid(ascf, ga, err));
    CHECK(std::fabs(ga.sample(0.5, 1.5) - 1) < 1e-6 && std::fabs(ga.sample(1.0, 1.0) - 3.0) < 1e-6);
    const std::string gtxf = Tmp("pd_geoid.gtx");
    FILE* fg = std::fopen(gtxf.c_str(), "wb");
    auto be = [&](const void* p, int n) { const unsigned char* c = (const unsigned char*)p; for (int i = n - 1; i >= 0; --i) std::fputc(c[i], fg); };
    double hd[4] = {48.0, 355.0, 1.0, 1.0}; int32_t hi[2] = {2, 2};
    for (double d : hd) be(&d, 8);
    for (int32_t v : hi) be(&v, 4);
    float fv[4] = {10, 11, 20, 21};  // sud puis nord
    for (float f : fv) be(&f, 4);
    std::fclose(fg);
    Grid2D gg; CHECK(LoadGtxGrid(gtxf, gg, err));
    CHECK(std::fabs(gg.lonW + 5) < 1e-9 && std::fabs(gg.sample(-5, 49) - 20) < 1e-6 && std::fabs(gg.sample(-4.5, 48.5) - 15.5) < 1e-5);
    std::printf("Grilles .tac / .asc / .gtx et surface en points : OK\n");
  }
  // 15) Format IGN complet (.mnt) : 4 ordres, coordonnées par nœud, précision,
  //     translation ; géoïdes en cascade (RAC prioritaire, RAF ailleurs)
  {
    const int nx = 4, ny = 3;  // lon 8.5..8.8, lat 41.5..41.7
    auto val = [](int i, int jn) { return 10.0 + i + 0.1 * jn; };  // jn : ligne depuis le nord
    for (int order = 1; order <= 4; ++order)
      for (int coords = 0; coords <= 1; ++coords) {
        const std::string f = Tmp("pd_ign.mnt");
        FILE* fm = std::fopen(f.c_str(), "w");
        std::fprintf(fm, "8.5 8.8 41.5 41.7 0.1 0.1 %d %d 1 1 0.5 RAC essai\n", order, coords);
        auto emit = [&](int i, int jn) {
          if (coords) std::fprintf(fm, "%.2f %.2f ", 8.5 + 0.1 * i, 41.7 - 0.1 * jn);
          std::fprintf(fm, "%.3f 3\n", val(i, jn) - 0.5);  // translation 0.5 dans l'entête
        };
        // si les coordonnées sont présentes, l'ordre est volontairement mélangé
        if (order == 1) for (int i = 0; i < nx; ++i) for (int js = 0; js < ny; ++js) emit(coords ? nx - 1 - i : i, ny - 1 - js);
        if (order == 2) for (int jn = 0; jn < ny; ++jn) for (int i = 0; i < nx; ++i) emit(i, jn);
        if (order == 3) for (int i = 0; i < nx; ++i) for (int jn = 0; jn < ny; ++jn) emit(i, jn);
        if (order == 4) for (int js = 0; js < ny; ++js) for (int i = 0; i < nx; ++i) emit(i, ny - 1 - js);
        std::fclose(fm);
        Grid2D gm; CHECK(LoadTacGrid(f, gm, err));
        CHECK(gm.nx == nx && gm.ny == ny);
        for (int jn = 0; jn < ny; ++jn)
          for (int i = 0; i < nx; ++i) CHECK(std::fabs(gm.at(i, jn) - val(i, jn)) < 1e-4);
      }
    // cascade : « RAC » couvre 8.5..8.8 (N = 10+…), « RAF » ailleurs (N = 50)
    const std::string raf = Tmp("pd_raf.tac");
    FILE* fr = std::fopen(raf.c_str(), "w");
    std::fprintf(fr, "7.0 9.0 41.0 42.0 0.5 0.5 2 0 1 1 0. RAF essai\n");
    for (int k = 0; k < 5 * 3; ++k) std::fprintf(fr, "50.000 99\n");
    std::fclose(fr);
    const std::string zh2 = Tmp("pd_zh2.glhi");
    FILE* fz = std::fopen(zh2.c_str(), "w");
    for (double lo = 7.0; lo <= 9.01; lo += 0.1)
      for (double la = 41.0; la <= 42.01; la += 0.1) std::fprintf(fz, "%.3f %.3f 46.0\n", lo, la);
    std::fclose(fz);
    VerticalRefParams vp; vp.mode = VerticalRefParams::Mode::Surfaces;
    vp.zhFiles = {zh2}; vp.refGrid = Tmp("pd_ign.mnt") + ";" + raf;
    VerticalCorrection vc;
    CHECK(vc.build(vp, 7.5, 8.7, 41.4, 41.8, err));
    CHECK(std::fabs(vc.at(8.6, 41.6) - (46.0 - val(1, 1))) < 1e-3);  // RAC
    CHECK(std::fabs(vc.at(7.6, 41.6) - (46.0 - 50.0)) < 1e-3);       // RAF
    std::printf("Grilles IGN .mnt (ordres 1-4, coordonnées, translation) et cascade RAC/RAF : OK\n");
  }
  // 16) Marée FES : décompression, arguments astronomiques (référence pyfes),
  //     atlas .pdtide synthétique, plus basse mer
  {
    // flux zlib « stocké » (deflate type 0) écrit à la main
    auto zstore = [](const std::vector<uint8_t>& d) {
      std::vector<uint8_t> z = {0x78, 0x01};
      size_t p = 0;
      do {
        const size_t n = std::min<size_t>(65535, d.size() - p);
        const bool last = p + n == d.size();
        z.push_back(last ? 1 : 0);
        z.push_back(n & 255); z.push_back(n >> 8);
        z.push_back(~n & 255); z.push_back((~n >> 8) & 255);
        z.insert(z.end(), d.begin() + p, d.begin() + p + n);
        p += n;
      } while (p < d.size());
      for (int k = 0; k < 4; ++k) z.push_back(0);  // adler32 non vérifié
      return z;
    };
    std::vector<uint8_t> raw(70000);
    for (size_t k = 0; k < raw.size(); ++k) raw[k] = (uint8_t)(k * 7 + k / 300);
    std::vector<uint8_t> back;
    const auto zs = zstore(raw);
    CHECK(ZlibInflate(zs.data(), zs.size(), back) && back == raw);
    // flux compressé par zlib (niveau 9, Huffman et répétitions)
    const uint8_t dyn[] = {0x78, 0xda, 0x4b, 0x4c, 0x4a, 0x4e, 0x44, 0xa0, 0x94, 0x44,
                           0x54, 0x2e, 0x00, 0xb2, 0x1d, 0x0b, 0x7f};
    CHECK(ZlibInflate(dyn, sizeof dyn, back));
    CHECK(std::string(back.begin(), back.end()) == "abcabcabcabcabdabcabcabcabcabd");
    // f et V+u au 2026-11-02 00:00 UTC (pyfes 2026.5, Schureman ordre 1)
    struct R { const char* n; double f, vu; };
    const R ref[] = {{"M2", 0.969483, 190.2155}, {"K1", 1.098007, 315.6721}, {"O1", 1.158438, 232.7690},
                     {"L2", 1.308656, 47.5097}, {"K2", 1.263752, 91.8254}, {"Mf", 1.383739, 264.6776},
                     {"MKS2", 1.225187, 282.0409}, {"R2", 1.0, 117.8866}, {"Sa", 1.0, 221.2880},
                     // ondes mineures déduites
                     {"2Q1", 1.158438, 169.7779}, {"OO1", 1.651059, 222.1241}, {"Eta2", 1.395234, 126.3886},
                     {"Pi1", 1.0, 110.8254}, {"Theta1", 1.145354, 96.9398}};
    std::vector<const TideWave*> ws;
    for (const R& r : ref) { ws.push_back(FindTideWave(r.n)); CHECK(ws.back()); }
    TideNodal nod;
    nod.compute(ws, 1793577600.0);
    for (size_t k = 0; k < ws.size(); ++k) {
      double a = std::atan2(nod.sinArg[k], nod.cosArg[k]) * 180 / M_PI;
      a = std::fmod(a - ref[k].vu + 540.0, 360.0) - 180.0;
      CHECK(std::fabs(nod.f[k] - ref[k].f) < 5e-4 && std::fabs(a) < 0.1);
    }
    // atlas synthétique : M2 seule, A = 2 m, G = 30°, 40×40 nœuds, tuiles 32
    const std::string atl = Tmp("pd_test.pdtide");
    {
      const int T = 32, nx = 40, ny = 40, ntx = 2, nty = 2;
      FILE* fa = std::fopen(atl.c_str(), "wb");
      auto put = [&](const void* p, size_t n) { std::fwrite(p, 1, n, fa); };
      uint32_t W = 1, TT = T; int32_t dims[4] = {nx, ny, ntx, nty};
      double geo[5] = {-5.0, 47.0, 1.0 / 30, 1.0 / 30, 0.001};
      uint32_t fl[2] = {0, 0};
      put("PDTIDE01", 8); put(&W, 4); put(&TT, 4); put(dims, 16); put(geo, 40); put(fl, 8);
      char nm[16] = "M2"; put(nm, 16);
      const short re = (short)std::lround(2000 * std::cos(30 * M_PI / 180));
      const short im = (short)std::lround(2000 * std::sin(30 * M_PI / 180));
      std::vector<std::vector<uint8_t>> tiles;
      for (int tj = 0; tj < nty; ++tj)
        for (int ti = 0; ti < ntx; ++ti) {
          std::vector<uint8_t> d;
          const bool partial = ti == 1 || tj == 1;
          if (partial) {  // masque : nœuds hors grille absents
            std::vector<uint8_t> m((T * T + 7) / 8, 0);
            for (int r = 0; r < T; ++r)
              for (int q = 0; q < T; ++q)
                if (ti * T + q < nx && tj * T + r < ny) m[(r * T + q) >> 3] |= 1 << ((r * T + q) & 7);
            d = m;
          }
          for (short v : {re, im}) {
            std::vector<int16_t> delta(T * T, 0);
            for (int r = 0; r < T; ++r) delta[r * T] = v;  // valeur constante : 1re différence seule
            std::vector<uint8_t> lo(T * T), hi(T * T);
            for (int k = 0; k < T * T; ++k) { lo[k] = (uint8_t)(delta[k] & 255); hi[k] = (uint8_t)((uint16_t)delta[k] >> 8); }
            d.insert(d.end(), lo.begin(), lo.end());
            d.insert(d.end(), hi.begin(), hi.end());
          }
          tiles.push_back(zstore(d));
        }
      uint64_t off = 80 + 16 + 16 * 4;
      for (int k = 0; k < 4; ++k) {
        uint32_t sz = (uint32_t)tiles[k].size();
        uint8_t kind = (k == 0) ? 1 : 2, pad[3] = {0, 0, 0};
        put(&off, 8); put(&sz, 4); put(&kind, 1); put(pad, 3);
        off += sz;
      }
      for (auto& t : tiles) put(t.data(), t.size());
      std::fclose(fa);
    }
    TideAtlas at;
    CHECK(at.open(atl, err) && at.waveNames().size() == 1);
    TideRegion rg;
    CHECK(at.loadRegion(-4.8, -4.0, 47.2, 48.0, rg, err));
    const double tq = 1793577600.0 + 5 * 3600;
    std::vector<const TideWave*> m2 = {FindTideWave("M2")};
    TideNodal nm2; nm2.compute(m2, tq);
    const double expect = nm2.f[0] * 2.0 * std::cos(std::atan2(nm2.sinArg[0], nm2.cosArg[0]) - 30 * M_PI / 180);
    CHECK(std::fabs(rg.heightAt(-4.5, 47.5, tq) - expect) < 2e-3);
    CHECK(std::isnan(rg.heightAt(-3.5, 47.5, tq)));  // hors du modèle (au-delà de 40 nœuds)
    // plus basse mer d'une M2 pure : -2 m × f(M2) maximal sur 19 ans (≈ 1,037)
    FesTide fes;
    TideZ0Params zp;
    CHECK(fes.prepare(at, -4.6, -4.5, 47.4, 47.5, zp, err));
    const double z0 = fes.z0At(-4.55, 47.45);
    std::printf("Marée FES : M2 pure, Z0 (plus basse mer) = %.3f m (attendu ~2,07)\n", z0);
    CHECK(z0 > 2.06 && z0 < 2.08);
    TideField tf; fes.field(tq, tf);
    CHECK(std::fabs(tf.at(-4.55, 47.45) - (expect + z0)) < 5e-3);
    std::printf("Atlas .pdtide, décompression, arguments astronomiques, plus basse mer : OK\n");
  }
  // 17) Base de bathymétrie : codec de tuile, import, priorités, retrait, vue
  {
    DepthTile a;
    a.reset();
    std::mt19937 rr(5);
    for (int k = 0; k < DepthTile::kN; ++k)
      if (rr() % 3) a.v[k] = 12000 + (int)(k % 256) * 10 - (int)(rr() % 50) * 10;
    a.srcIds = {7, 9};
    a.src.assign(DepthTile::kN, 0);
    for (int k = 0; k < DepthTile::kN; k += 7) a.src[k] = 1;
    std::vector<uint8_t> blob;
    DepthTile b;
    CHECK(EncodeTile(a, blob) && DecodeTile(blob.data(), blob.size(), b, err));
    CHECK(b.v == a.v && b.src == a.src && b.srcIds == a.srcIds);
    a.v[5] = -2345671;  // valeur au mm, négative
    CHECK(EncodeTile(a, blob) && DecodeTile(blob.data(), blob.size(), b, err) && b.v == a.v);

    const std::string folder = Tmp("pd_testdb");
    std::filesystem::remove_all(folder);
    BathyDb db;
    CHECK(db.open(folder, err));
    // A : 2 m, 2010, 10 m partout ; B : 0,5 m, 2020, 8 m sur la moitié est
    auto grid = [](double lo0, double lo1, double la0, double la1, double stepM, float z) {
      PointCloud pc;
      const double dla = stepM / 110540.0, dlo = stepM / (111320.0 * std::cos(48.3 * M_PI / 180));
      for (double la = la0; la <= la1; la += dla)
        for (double lo = lo0; lo <= lo1; lo += dlo) pc.add(lo, la, z);
      pc.computeBounds();
      return pc;
    };
    PointCloud A = grid(-4.50, -4.48, 48.30, 48.31, 2.0, 10.f), B = grid(-4.49, -4.47, 48.30, 48.31, 0.5, 8.f);
    bool gA = false, gB = false;
    DbSource ma, mb;
    ma.name = "A"; ma.date = "2010-06-01"; ma.res = EstimateResolution(A, gA);
    mb.name = "B"; mb.date = "2020-06-01"; mb.res = EstimateResolution(B, gB);
    CHECK(gA && gB && std::fabs(ma.res - 2.0) < 0.05 && std::fabs(mb.res - 0.5) < 0.02);
    int64_t ida = 0, idb = 0;
    CHECK(db.importCloud(A, ma, gA, DbPriority::ResolutionDate, err, nullptr, nullptr, &ida));
    CHECK(db.importCloud(B, mb, gB, DbPriority::ResolutionDate, err, nullptr, nullptr, &idb));
    CHECK(db.source(ida)->level == 16 && db.source(idb)->level == 18);
    const double plo = -4.485, pla = 48.305;  // recouvrement
    CHECK(db.sourceAt(plo, pla, 18) == idb && db.sourceAt(-4.495, pla, 18) == ida);
    CHECK(db.setRanks({ida, idb}, DbPriority::Manual, err) && db.sourceAt(plo, pla, 18) == ida);
    CHECK(db.remergeAll(DbPriority::Shoalest, err) && db.sourceAt(plo, pla, 18) == idb);
    CHECK(db.remergeAll(DbPriority::Newest, err) && db.sourceAt(plo, pla, 18) == idb);
    CHECK(db.setVisible(idb, false, DbPriority::ResolutionDate, err) && db.sourceAt(plo, pla, 18) == ida);
    CHECK(db.setVisible(idb, true, DbPriority::ResolutionDate, err) && db.sourceAt(plo, pla, 18) == idb);
    // vue : une seule surface, 8 m à l'est, 10 m à l'ouest, sources indiquées
    PointCloud v;
    std::vector<int64_t> sv;
    double dl, dt;
    int used;
    CHECK(db.buildView(-4.50, -4.47, 48.30, 48.31, 18, 20000000, v, dl, dt, used, err, nullptr, &sv));
    std::printf("vue : niveau %d, %zu cellules, %.2f..%.2f m\n", used, v.size(), v.zMin, v.zMax);
    CHECK(used == 18 && sv.size() == v.size() && v.zMin == 8.f && v.zMax == 10.f);
    // réouverture : sources relues depuis les fichiers
    {
      BathyDb db2;
      CHECK(db2.open(folder, err) && db2.sources().size() == 2);
    }
    CHECK(GuessSourceGroup("/x/LITTO3D_FRA_0145_6830_MNT_2014.asc") == "LITTO3D" &&
          GuessSourceGroup("20210325-31_PFE-G3_Leve-L93.xyz") == "PFE");
    CHECK(db.setGroup({ida, idb}, "Essai", err));
    {
      BathyDb db3;
      CHECK(db3.open(folder, err) && db3.sources().size() == 2 && db3.sources()[0].group == "Essai");
    }
    CHECK(db.setVisible(std::vector<int64_t>{ida, idb}, false, DbPriority::ResolutionDate, err) &&
          db.sourceAt(plo, pla, 18) == 0);
    CHECK(db.setVisible(std::vector<int64_t>{ida, idb}, true, DbPriority::ResolutionDate, err));
    CHECK(db.removeSource(idb, DbPriority::ResolutionDate, err) && db.sourceAt(plo, pla, 18) == ida);
    CHECK(db.sourceAt(-4.475, pla, 18) == 0);  // B seul : plus rien
    CHECK(db.removeSources({ida}, DbPriority::ResolutionDate, err) && db.sources().empty() &&
          db.sourceAt(-4.495, pla, 18) == 0);
    {  // dernière source retirée : plus aucun fichier de cellule
      int pdb = 0;
      for (auto& e : std::filesystem::directory_iterator(std::filesystem::u8path(folder)))
        if (e.path().extension() == ".pdb") ++pdb;
      CHECK(pdb == 0 && db.cellInfo().empty());
    }
    std::printf("Base : codec, import, priorités, masquage, retrait, vue : OK\n");
  }
  // 18) Palette automatique
  {
    auto t = AutoThresholds(0.3f, 23.7f, 10, true);  // pas rond : 2,5 m
    CHECK(t.size() <= 10 && t.size() >= 8 && t.front() <= 0.3f && t.back() >= 22.f &&
          std::fabs(t[1] - t[0] - 2.5f) < 1e-4);
    auto u = AutoThresholds(-3.f, 12.f, 6, false);
    CHECK(u.size() == 6 && u.front() == -3.f && std::fabs(u.back() - 12.f) < 1e-4);
    ColorRamp base = ColorRamp::Preset("TimeZero PBG (bleus)"), keep = base;
    keep.mode = ColorRamp::Mode::Stepped;
    ColorRamp a = AutoRamp(base, keep, -2.f, 30.f, 9, true);
    CHECK(a.mode == ColorRamp::Mode::Stepped && a.stops.size() >= 5);
    // estran (< 0) avec la couleur « terre », premier seuil marin = couleur d'eau peu profonde
    CHECK(a.stops.front().depth < 0 && a.stops.front().rgba == base.stops.front().rgba);
    for (const auto& st : a.stops)
      if (st.depth >= 0) { CHECK(st.rgba == base.stops[1].rgba); break; }
    CHECK(a.stops.back().rgba == base.stops.back().rgba);
    std::printf("Palette automatique : %zu seuils de %.1f à %.1f m : OK\n", a.stops.size(),
                a.stops.front().depth, a.stops.back().depth);
  }
  // 19) Valeurs de remplissage : lecture ASC, plage acceptée, nettoyage de la base
  {
    const std::string asc = Tmp("pd_fill.asc");
    {
      std::ofstream o(asc);
      o << "ncols 20\nnrows 20\nxllcorner -4.5\nyllcorner 48.3\ncellsize 0.0001\n"
           "NODATA_value -3.4028234663852886e+38\n";
      for (int r = 0; r < 20; ++r) {
        for (int c = 0; c < 20; ++c) {
          if (r == 0 && c < 5) o << "-3.40282e+38 ";      // NODATA écrit en simple précision
          else if (r == 1 && c < 3) o << "999 ";          // remplissage non déclaré
          else o << -(5.0 + 0.01 * c) << ' ';             // altitudes : -5 m -> 5 m de fond
        }
        o << '\n';
      }
    }
    LoadOptions lo;
    lo.zsign = LoadOptions::ZSign::Elevation;
    PointCloud pc;
    double cl, ca;
    CHECK(LoadESRIAscii(asc, lo, pc, cl, ca, err));
    CHECK(pc.size() == 395 && std::fabs(pc.zMin + 999.f) < 1e-3 && pc.zMax < 5.2f);
    {  // fichier en pieds : converti en mètres dans le nuage (base toujours en m)
      LoadOptions lf = lo;
      lf.zToMeters = 0.3048;
      PointCloud pf;
      CHECK(LoadESRIAscii(asc, lf, pf, cl, ca, err) && pf.size() == pc.size() &&
            std::fabs(pf.zMin - pc.zMin * 0.3048f) < 1e-3f && std::fabs(pf.zMax - pc.zMax * 0.3048f) < 1e-3f);
      lf.zOffset = 1.f;  // décalage en mètres, après conversion
      CHECK(LoadESRIAscii(asc, lf, pf, cl, ca, err) && std::fabs(pf.zMax - (pc.zMax * 0.3048f + 1.f)) < 1e-3f);
    }
    lo.nodata = {-9999, 999};  // NODATA de l'utilisateur, en Z brut
    CHECK(LoadESRIAscii(asc, lo, pc, cl, ca, err) && pc.size() == 392 && pc.zMin >= 5.f);
    // bornes d'import sur la profondeur finale (couche complète)
    {
      LayerParams lp;
      lp.path = asc;
      lp.cloudOnly = true;
      lp.load.zsign = LoadOptions::ZSign::Elevation;
      lp.depthBounds = true; lp.depthMin = 5.f; lp.depthMax = 5.095f;
      BathyLayer L(lp);
      L.loadSync();
      CHECK(L.state() == BathyLayer::Ready && L.cloud.size() == 192 &&
            L.cloud.zMax <= 5.095f);
    }
    // filtre et renumérotation des triangles
    {
      PointCloud t;
      for (int k = 0; k < 4; ++k) t.add(k * 0.001, 0, (float)k);
      std::vector<uint32_t> tr = {0, 1, 2, 1, 2, 3};
      CHECK(t.keepDepths(0.5f, 10.f, &tr) == 1 && t.size() == 3 && tr == std::vector<uint32_t>({0, 1, 2}));
    }

    // base : un point aberrant (1e7 m) refusé ; -999 m retiré par le nettoyage
    const std::string folder = Tmp("pd_testdb_fill");
    std::filesystem::remove_all(folder);
    BathyDb db;
    CHECK(db.open(folder, err));
    PointCloud g;
    const double dla = 1.0 / 110540.0, dlo = 1.0 / (111320.0 * std::cos(48.3 * M_PI / 180));
    for (int j = 0; j < 400; ++j)
      for (int i = 0; i < 400; ++i) {
        float z = 6.f + 0.01f * i;
        if (i == 10 && j < 50) z = -999.f;
        if (i == 20 && j == 20) z = 1e7f;
        g.add(-4.50 + i * dlo, 48.30 + j * dla, z);
      }
    g.computeBounds();
    bool gr = false;
    DbSource m;
    m.name = "F"; m.date = "2020-01-01"; m.res = EstimateResolution(g, gr);
    int64_t id = 0;
    CHECK(db.importCloud(g, m, gr, DbPriority::ResolutionDate, err, nullptr, nullptr, &id));
    CHECK(std::fabs(db.source(id)->zmin + 999.f) < 1e-3 && db.source(id)->zmax < 11.f);
    PointCloud v;
    double dl, dt;
    int used;
    CHECK(db.buildView(-4.501, -4.494, 48.299, 48.304, 13, 20000000, v, dl, dt, used, err));
    CHECK(v.zMin < -900.f);  // niveau grossier : la plus faible sonde (-999) l'emporte
    long long removed = 0;
    CHECK(db.cleanSources({id}, -200.f, 200.f, DbPriority::ResolutionDate, err, nullptr, nullptr,
                          &removed));
    std::printf("nettoyage : %lld sondes retirées, zmin %.2f\n", removed, db.source(id)->zmin);
    CHECK(removed >= 40 && db.source(id)->zmin >= 6.f);
    for (int lev : {13, 16, 20}) {
      CHECK(db.buildView(-4.501, -4.494, 48.299, 48.304, lev, 20000000, v, dl, dt, used, err));
      CHECK(v.zMin >= 6.f && v.zMax < 11.f && v.size() > 0);
    }
    {  // base antérieure au filtre : statistiques aberrantes recalculées à l'ouverture
      for (auto& e : std::filesystem::directory_iterator(folder)) {
        if (e.path().extension() != ".pdb") continue;
        sqlite3* h = nullptr;
        CHECK(sqlite3_open(e.path().string().c_str(), &h) == SQLITE_OK);
        sqlite3_exec(h, "UPDATE sources SET zmin=-2000000", nullptr, nullptr, nullptr);
        sqlite3_close(h);
      }
      BathyDb db2;
      CHECK(db2.open(folder, err) && db2.sources().size() == 1 && db2.needsRepair());
      CHECK(db2.repairStats(err) && !db2.needsRepair() && db2.sources()[0].zmin >= 6.f);
      BathyDb db3;
      CHECK(db3.open(folder, err) && !db3.needsRepair());
    }
    std::printf("Valeurs de remplissage et nettoyage : OK\n");
  }
  // 20) Découpage des fichiers, redécoupage, fichiers masqués, arrêt d'un import
  {
    CHECK(CellFileName(-4.3, 48.3, 1.0) == "PD_N48W005.pdb");
    CHECK(CellFileName(-4.3, 48.3, 0.25) == "PD_N4815W00430.pdb");
    CHECK(CellFileName(-4.3, 48.3, 0.1) == "PD_N4818W00418.pdb");
    double lw, ls;
    bool lg;
    CHECK(ParseCellFileName("PD_N4815W00430.pdb", lw, ls, lg) && !lg && std::fabs(lw + 4.5) < 1e-9 &&
          std::fabs(ls - 48.25) < 1e-9);
    CHECK(ParseCellFileName("PD_N48W005.pdb", lw, ls, lg) && lg && lw == -5 && ls == 48);

    const std::string folder = Tmp("pd_testdb_split");
    std::filesystem::remove_all(folder);
    auto grid = [](double lo0, double lo1, double la0, double la1, double stepM, float z) {
      PointCloud pc;
      const double dla = stepM / 110540.0, dlo = stepM / (111320.0 * std::cos(48.3 * M_PI / 180));
      for (double la = la0; la <= la1; la += dla)
        for (double lo = lo0; lo <= lo1; lo += dlo) pc.add(lo, la, z + 0.001f * (float)((lo - lo0) / dlo));
      pc.computeBounds();
      return pc;
    };
    size_t before = 0;
    {
      BathyDb db;
      CHECK(db.open(folder, err, 1.0) && db.cellDeg() == 1.0);  // ancienne base, cellules de 1°
      // à cheval sur deux cellules de 0,25° (lon -4,5)
      PointCloud A = grid(-4.52, -4.48, 48.30, 48.32, 4.0, 10.f);
      bool g = false;
      DbSource m;
      m.name = "A"; m.date = "2015-01-01"; m.res = EstimateResolution(A, g);
      CHECK(db.importCloud(A, m, g, DbPriority::ResolutionDate, err));
      auto ci = db.cellInfo();
      CHECK(ci.size() == 1 && ci[0].name == "PD_N48W005.pdb" && ci[0].sources == 1);
      // arrêt pendant l'import : rien n'est gardé
      std::atomic<bool> stop(true);
      DbSource m2 = m;
      m2.name = "B";
      CHECK(!db.importCloud(A, m2, g, DbPriority::ResolutionDate, err, nullptr, &stop) &&
            err == "Annulé" && db.sources().size() == 1);
      PointCloud v;
      double dl, dt;
      int used;
      CHECK(db.buildView(-4.53, -4.47, 48.29, 48.33, 16, 20000000, v, dl, dt, used, err));
      before = v.size();
      {  // écran tourné de 45° : seules ses cellules, et le niveau tient dans le budget
        const double cx = -4.50, cy = ScreenMapper::mercY(48.31), r = 0.015;
        ViewQuad q;
        for (int k = 0; k < 4; ++k) {
          const double a = M_PI / 4 + k * M_PI / 2;
          q.x[k] = cx + r * std::cos(a); q.y[k] = cy + r * std::sin(a);
        }
        PointCloud vq;
        double dl2, dt2;
        int used2 = 0, usedR = 0;
        const double la0 = std::atan(std::sinh((cy - r) * M_PI / 180)) * 180 / M_PI;
        const double la1 = std::atan(std::sinh((cy + r) * M_PI / 180)) * 180 / M_PI;
        int L = 0;  // niveau propre des données
        CHECK(db.buildView(cx - r, cx + r, la0, la1, 18, 100000000, vq, dl2, dt2, L, err));
        const size_t cellsQ = (size_t)(q.area() / std::pow(360.0 / std::ldexp(256.0, L), 2));
        CHECK(db.buildView(cx - r, cx + r, la0, la1, L, cellsQ + 100, vq, dl2, dt2, used2, err, nullptr,
                          nullptr, nullptr, false, Datum::Msl, nullptr, &q));
        ViewQuad q2 = q;  // tolérance : une cellule au bord
        for (int k = 0; k < 4; ++k) {
          q2.x[k] = cx + (q.x[k] - cx) * (r + 2 * dl2) / r;
          q2.y[k] = cy + (q.y[k] - cy) * (r + 2 * dl2) / r;
        }
        bool in = true;
        for (size_t k = 0; k < vq.size(); ++k) in &= q2.contains(vq.lon(k), ScreenMapper::mercY(vq.lat(k)));
        PointCloud vr;
        CHECK(db.buildView(cx - r, cx + r, la0, la1, L, cellsQ + 100, vr, dl2, dt2, usedR, err));
        std::printf("vue tournée : %zu cellules (rectangle %zu), niveau %d (rectangle %d)\n", vq.size(),
                    vr.size(), used2, usedR);
        CHECK(used2 == L && usedR < L && in && vq.size() > 0);
      }
      // redécoupage en 0,25°
      CHECK(db.resplit(0.25, err));
      CHECK(db.cellDeg() == 0.25 && db.sources().size() == 1);
      ci = db.cellInfo();
      std::printf("redécoupage : %zu fichiers (%s, %s)\n", ci.size(), ci[0].name.c_str(),
                  ci.size() > 1 ? ci[1].name.c_str() : "-");
      CHECK(ci.size() >= 2);
      for (const auto& c : ci) CHECK(c.sources == 1 && c.deg == 0.25);
      CHECK(db.buildView(-4.53, -4.47, 48.29, 48.33, 16, 20000000, v, dl, dt, used, err) &&
            v.size() == before);
      // fichier masqué : la moitié ouest (lon < -4,5) disparaît
      db.setHiddenCells({"PD_N4815W00445.pdb"});
      CHECK(db.buildView(-4.53, -4.47, 48.29, 48.33, 16, 20000000, v, dl, dt, used, err) &&
            v.size() < before && v.lonMin >= -4.5);
      CHECK(db.sourceAt(-4.51, 48.31, 16) == 0 && db.sourceAt(-4.49, 48.31, 16) != 0);
    }
    {
      BathyDb db;
      CHECK(db.open(folder, err) && db.cellDeg() == 0.25 && db.sources().size() == 1);
      CHECK(!std::filesystem::exists(folder + "/PD_N48W005.pdb") &&
            !std::filesystem::exists(folder + "/_redecoupage"));
    }
    std::printf("Découpage, redécoupage, masquage, arrêt d'import : OK\n");
  }
  // 21) Date du levé modifiable, réglages d'import gardés avec la source
  {
    const std::string folder = Tmp("pd_testdb_meta");
    std::filesystem::remove_all(folder);
    BathyDb db;
    CHECK(db.open(folder, err));
    auto grid = [](double lo0, double lo1, float z, double step) {
      PointCloud pc;
      const double dla = step / 110540.0, dlo = step / (111320.0 * std::cos(48.3 * M_PI / 180));
      for (double la = 48.30; la <= 48.305; la += dla)
        for (double lo = lo0; lo <= lo1; lo += dlo) pc.add(lo, la, z);
      pc.computeBounds();
      return pc;
    };
    PointCloud A = grid(-4.50, -4.49, 10.f, 2.0), B = grid(-4.495, -4.485, 8.f, 2.0);
    bool g = false;
    DbSource ma, mb;
    ma.name = "A"; ma.date = "2010-01-01"; ma.res = EstimateResolution(A, g); ma.params = "zFeet=1\nvrefMode=2\n";
    mb.name = "B"; mb.date = "2020-01-01"; mb.res = EstimateResolution(B, g);
    int64_t ida = 0, idb = 0;
    CHECK(db.importCloud(A, ma, g, DbPriority::Newest, err, nullptr, nullptr, &ida));
    CHECK(db.importCloud(B, mb, g, DbPriority::Newest, err, nullptr, nullptr, &idb));
    const double plo = -4.4925, pla = 48.3025;  // recouvrement
    CHECK(db.sourceAt(plo, pla, 18) == idb);     // B plus récent
    CHECK(db.setDates({ida}, "2024-06-01", DbPriority::Newest, err));
    CHECK(db.sourceAt(plo, pla, 18) == ida);     // A devient le plus récent : refusion
    CHECK(db.setParams({idb}, "zFeet=0\n", err) && db.source(idb)->params == "zFeet=0\n");
    BathyDb db2;
    CHECK(db2.open(folder, err) && db2.source(ida)->date == "2024-06-01" &&
          db2.source(ida)->params == "zFeet=1\nvrefMode=2\n" && db2.source(idb)->params == "zFeet=0\n");
    std::printf("Date du levé et réglages d'import : OK\n");
  }
  {  // résolution d'un levé en lignes (sondeur monofaisceau) : pas le long de la route
    PointCloud tr, sh;
    const double lat0 = 46.3, kx = 1.0 / (111320.0 * std::cos(lat0 * M_PI / 180)), ky = 1.0 / 110540.0;
    std::mt19937 noise(3);
    std::uniform_real_distribution<double> gps(-0.5, 0.5);  // bruit GPS (m)
    for (int line = 0; line < 20; ++line)        // routes est-ouest espacées de 100 m
      for (int i = 0; i < 1000; ++i)             // un point tous les 4 m
        tr.add(-60.4 + (i * 4.0 + gps(noise)) * kx, lat0 + (line * 100.0 + gps(noise)) * ky, 10.f);
    tr.computeBounds();
    bool g = false;
    const double rt = EstimateResolution(tr, g);
    CHECK(!g && rt > 3.0 && rt < 5.0);
    // même nuage en ordre aléatoire : l'analyse spatiale (plus proche voisin,
    // triangulation) ne dépend plus de l'ordre du fichier
    std::vector<size_t> ord(tr.size());
    for (size_t i = 0; i < ord.size(); ++i) ord[i] = i;
    std::mt19937 rng(7);
    std::shuffle(ord.begin(), ord.end(), rng);
    for (size_t i : ord) sh.add(tr.lon(i), tr.lat(i), tr.z[i]);
    sh.computeBounds();
    const double rs = EstimateResolution(sh, g);
    CHECK(!g && rs > 3.0 && rs < 5.0);
    const SurveyShape ss = AnalyzeSurvey(sh);
    CHECK(ss.lines && ss.maxEdge > 100 && ss.maxEdge < 160);  // lignes à 100 m
    // grille en degrés avec coordonnées à 6 décimales : pas une « grille de 11 cm »
    PointCloud r6;
    std::mt19937 rr(5);
    std::uniform_real_distribution<double> ux(0, 0.2), uy(0, 0.1);
    for (int i = 0; i < 20000; ++i)
      r6.add(std::round((-4.5 + ux(rr)) * 1e6) / 1e6, std::round((48.3 + uy(rr)) * 1e6) / 1e6, 5.f);
    r6.computeBounds();
    CHECK(!AnalyzeSurvey(r6).isGrid());
    // grille de 5 m en UTM 30N diffusée en lon/lat arrondis à 1e-7°, avec 2 % de sondes hors réseau
    {
      Crs utm;
      std::string e;
      CHECK(Crs::Parse("EPSG:32630", utm, e));
      PointCloud gu;
      std::uniform_real_distribution<double> off(0, 5);
      for (int j = 0; j < 300; ++j)
        for (int i = 0; i < 300; ++i) {
          if ((i * 7 + j * 3) % 5 == 0) continue;  // nœuds vides
          double x = 400000 + i * 5.0, y = 5350000 + j * 5.0, lo, la;
          if ((i + j) % 50 == 0) { x += off(rr); y += off(rr); }
          utm.toWGS84(x, y, lo, la);
          gu.add(lo, la, 10.f);
        }
      gu.computeBounds();
      const SurveyShape sg = AnalyzeSurvey(gu);
      CHECK(sg.isGrid() && std::fabs(sg.dx - 5) < 0.01 && std::fabs(sg.dy - 5) < 0.01 && sg.frame == "utm30N");
    }
    std::printf("Résolution des levés en lignes : %.2f m (ordre aléatoire : %.2f m), D %.0f m ; grilles OK\n", rt, rs, ss.maxEdge);
  }
  {  // format texte personnalisé : entête, date + heure, lat / lon, champs en trop
    const std::string path = Tmp("pd_fmt.pos");
    {
      std::ofstream o(path);
      o << "% programme : RTKPOST\n%\n"
           "%  UTC                   latitude(deg) longitude(deg)  height(m)   Q  ns\n"
           "2021/03/25 08:10:23.000   48.377952072   -4.489374333    55.2624   1   9\n"
           "2021/03/25 08:10:24.000   48.377951922   -4.489374499    55.2605   1   9\n"
           "2021/03/25 08:10:25.500   48.377951939   -4.489374485    55.2630   1   9\n";
    }
    std::vector<std::string> head;
    std::string e;
    CHECK(ReadTextHead(path, 100, head, e) && head.size() == 6);
    TextFormat f = GuessTextFormat(head);
    CHECK(f.custom && f.skipLines == 3 && f.colDate == 1 && f.colTime == 2 && f.colY == 3 &&
          f.colX == 4 && f.colZ == 5);
    TextFormat g;
    CHECK(g.deserialize(f.serialize()) && g == f);
    LoadOptions lo;
    lo.text = f;
    lo.zsign = LoadOptions::ZSign::DepthPositive;
    PointCloud pc;
    CHECK(LoadXYZ(path, lo, pc, e) && pc.size() == 3);
    CHECK(std::fabs(pc.lon(0) + 4.489374333) < 2e-7 && std::fabs(pc.lat(0) - 48.377952072) < 2e-7);
    CHECK(std::fabs(pc.z[0] - 55.2624f) < 1e-3f);
    CHECK(pc.tMin == 1616659823LL && pc.tMax == 1616659825LL);  // 2021-03-25 08:10:23 UTC
    // point-virgule, virgule décimale, date J/M/A
    {
      std::ofstream o(path);
      o << "lat;lon;prof;date\n48,1;-4,2;12,5;25/03/2021\n48,1001;-4,2001;12,7;26/03/2021\n";
    }
    CHECK(ReadTextHead(path, 100, head, e));
    f = GuessTextFormat(head);
    CHECK(f.sep == ';' && f.decimal == ',' && f.skipLines == 1 && f.colY == 1 && f.colX == 2 &&
          f.colZ == 3 && f.colDate == 4);
    lo.text = f;
    CHECK(LoadXYZ(path, lo, pc, e) && pc.size() == 2 && std::fabs(pc.z[1] - 12.7f) < 1e-4f);
    // NONNA-10 (ian5142) : tabulations, DMS, date AAAAMMJJ avant 1970
    {
      std::ofstream o(path);
      o << "Lat (DMS)\tLong (DMS)\tDepth (m)\tContributor\r\n"
           "46-13-07.195N\t060-29-59.273W\t7.32\t19451231\r\n"
           "46-13-10.226N\t060-29-59.596W\t12.97\t20000618\r\n"
           "46-13-10.387N\t060-29-59.588W\t13.05\t19620301\r\n";
    }
    {
      TextFormat nf;
      nf.custom = true; nf.skipLines = 1; nf.sep = '\t'; nf.colY = 1; nf.colX = 2; nf.colZ = 3;
      nf.colDate = 4; nf.dateFmt = "yyyyMMdd"; nf.xFmt = "DDD-MM-SS.SSSX"; nf.yFmt = "DD-MM-SS.SSSX";
      LoadOptions ln;
      ln.text = nf;
      ln.zsign = LoadOptions::ZSign::DepthPositive;
      PointCloud pn;
      CHECK(LoadXYZ(path, ln, pn, e) && pn.size() == 3);
      CHECK(std::fabs(pn.lon(0) + (60 + 29 / 60.0 + 59.273 / 3600)) < 2e-7);
      // plus ancienne et plus récente, dates avant 1970 comprises
      CHECK(pd::FormatTime(pn.tMin).substr(0, 10) == "1945-12-31");
      CHECK(pd::FormatTime(pn.tMax).substr(0, 10) == "2000-06-18");
    }
    // degrés-minutes-secondes avec lettre d'hémisphère, sans motif (Baikal)
    {
      std::ofstream o(path);
      o << "54\xC2\xB0 00' 28.4400\" N,81\xC2\xB0 18' 01.0800\" E,4.5\n"
           "54\xC2\xB0 00' 32.4000\" N,81\xC2\xB0 17' 56.4000\" E,6\n"
           "54\xC2\xB0 00' 28.4400\" N,81\xC2\xB0 18' 20.8800\" E,2.6\n";
    }
    CHECK(ReadTextHead(path, 100, head, e));
    f = GuessTextFormat(head);
    CHECK(f.sep == ',' && f.skipLines == 0 && f.colY == 1 && f.colX == 2 && f.colZ == 3);
    lo.text = f;
    CHECK(LoadXYZ(path, lo, pc, e) && pc.size() == 3);
    CHECK(std::fabs(pc.lat(0) - (54 + 28.44 / 3600)) < 2e-7 && std::fabs(pc.lon(0) - (81 + 18 / 60.0 + 1.08 / 3600)) < 2e-7);
    {
      double d;
      int ax = -1;
      CHECK(AutoParseCoord("54\xC2\xB0 00' 28.44\" S", '.', d, &ax) && ax == 1 && std::fabs(d + (54 + 28.44 / 3600)) < 1e-9);
      CHECK(AutoParseCoord("W 004 29.5", '.', d, &ax) && ax == 2 && std::fabs(d + 4.491666667) < 1e-8);
      CHECK(AutoParseCoord("-48:22:40,5", ',', d, &ax) && ax == 0 && std::fabs(d + (48 + 22 / 60.0 + 40.5 / 3600)) < 1e-9);
      CHECK(!AutoParseCoord("12.5", '.', d, &ax));         // simple nombre
      CHECK(!AutoParseCoord("54 61 00 N", '.', d, &ax));   // minutes >= 60
      CHECK(!AutoParseCoord("Brest", '.', d, &ax));
      CHECK(!AutoParseCoord("2013-08-10", '.', d, &ax));
      CHECK(!AutoParseCoord("95 00 00 N", '.', d, &ax));   // latitude > 90
    }
    long long t;
    CHECK(ParseDateTime("2021-06-26T19:41:24Z", "", 0, t) && t == 1624736484LL);
    CHECK(ParseDateTime("03/25/2021", "08:00", 0, t) && t == 1616659200LL);
    // gabarits de date / heure (exemples de l'aide)
    TextFormat df;
    df.custom = true;
    auto DT = [&](const char* txt, const char* pat, const char* tim, const char* tpat, long long& out) {
      df.dateFmt = pat;
      df.timeFmt = tpat;
      return ParseFieldDateTime(txt, tim, df, out);
    };
    CHECK(DT("02/11/1983", "M/d/y", "", "", t) && t == 413769600LL);                // 1983-02-11
    CHECK(DT("02/11/1983", "d/M/y", "", "", t) && t == 436579200LL);                // 1983-11-02
    CHECK(DT("02/11/1983 8:07:34PM", "M/d/y h:m:sAP", "", "", t) && t == 413769600LL + 20 * 3600 + 7 * 60 + 34);
    CHECK(DT("02/11/83 8:07:34", "M/d/yy h:m:s", "", "", t) && t == 413769600LL + 8 * 3600 + 7 * 60 + 34);
    CHECK(DT("February 11, 1983 8:07:34", "MMM d, y h:m:s", "", "", t) && t == 413769600LL + 29254);
    CHECK(DT("1983-Feb-11 8:07:34", "y-MMM-d h:m:s", "", "", t) && t == 413769600LL + 29254);
    CHECK(DT("11 févr. 1983", "d MMM. y", "", "", t) && t == 413769600LL);
    CHECK(DT("19830211080734", "yyyyMMddhhmmss", "", "", t) && t == 413769600LL + 29254);
    CHECK(DT("1983y02M11d 8h07m34s", "y\\yM\\Md\\d h\\hm\\ms\\s", "", "", t) && t == 413769600LL + 29254);
    CHECK(DT("2021/03/25", "y/M/d", "08:10:23.000", "h:m:s.s", t) && t == 1616659823LL);
    CHECK(!DT("2021/13/25", "y/M/d", "", "", t));
    // gabarits de coordonnées
    double dg;
    CHECK(ParseCoordPattern("45.95536", "D.D", dg) && std::fabs(dg - 45.95536) < 1e-9);
    CHECK(ParseCoordPattern("-66 38.90778", "D M.M", dg) && std::fabs(dg + (66 + 38.90778 / 60)) < 1e-9);
    CHECK(ParseCoordPattern("45 57 19.36", "D M S.S", dg) && std::fabs(dg - (45 + 57 / 60.0 + 19.36 / 3600)) < 1e-9);
    CHECK(ParseCoordPattern("66 38' 54.47\" W", "D M' S.S\\\" X", dg) && dg < -66.6);
    CHECK(ParseCoordPattern("45.95536 S", "D.D X", dg) && std::fabs(dg + 45.95536) < 1e-9);
    CHECK(ParseCoordPattern("-66.648463 W", "D.D X", dg) && std::fabs(dg - 66.648463) < 1e-9);
    CHECK(ParseCoordPattern("45D57M19.36S", "D\\DM\\MS.S\\S", dg) && std::fabs(dg - (45 + 57 / 60.0 + 19.36 / 3600)) < 1e-9);
    CHECK(ParseCoordPattern("663854.47W", "DDMMS.SX", dg) && std::fabs(dg + (66 + 38 / 60.0 + 54.47 / 3600)) < 1e-9);
    CHECK(ParseCoordPattern("4822.6771N", "DDMM.MX", dg) && std::fabs(dg - (48 + 22.6771 / 60)) < 1e-9);
    TextFormat sf;
    sf.custom = true; sf.dateFmt = "y/M/d"; sf.timeFmt = "h:m:s.s"; sf.xFmt = "D M' S.S\" X;=%"; sf.yFmt = "DDMM.MX";
    TextFormat sg;
    CHECK(sg.deserialize(sf.serialize()) && sg.xFmt == sf.xFmt && sg.yFmt == sf.yFmt && sg.dateFmt == "y/M/d");
    std::printf("Format texte personnalisé : OK\n");
  }
  {  // hauteurs d'eau observées : lecture, interpolation, correction des sondes
    const std::string wl = Tmp("pd_wl.txt");
    {
      std::ofstream o(wl);
      o << "[TIME],[DEPTH]\n2026-11-01 05:00:00,0.35\n2026-11-01 06:00:00,0.55\n"
           "2026-11-01 07:00:00,0.45\n";
    }
    std::string e;
    auto ser = std::make_shared<WaterLevelSeries>();
    CHECK(ser->load(wl, TextFormat(), 1.0, e) && ser->size() == 3 && ser->skippedLines() == 1);
    {  // même fichier par format personnalisé (deviné), en pieds, heure UTC+8
      std::vector<std::string> head;
      CHECK(ReadTextHead(wl, 50, head, e));
      TextFormat wf = GuessWaterLevelFormat(head);
      CHECK(wf.custom && wf.skipLines == 1 && wf.colDate == 1 && wf.colZ == 2 && wf.sep == ',');
      wf.tzMinutes = 480;
      TextFormat wg;
      CHECK(wg.deserialize(wf.serialize()) && wg.tzMinutes == 480);
      WaterLevelSeries s2;
      CHECK(s2.load(wl, wf, 0.3048, e) && s2.size() == 3 && s2.skippedLines() == 0);
      long long u;
      CHECK(ParseDateTime("2026-10-31", "21:00", 0, u) && s2.tMin() == u);  // 05:00 UTC+8
      CHECK(std::fabs(s2.heights()[0] - 0.35f * 0.3048f) < 1e-6f);
    }
    long long t0;
    CHECK(ParseDateTime("2026-11-01", "05:30", 0, t0));
    float h;
    CHECK(ser->at(t0, h) && std::fabs(h - 0.45f) < 1e-5f);
    CHECK(ser->at(t0 - 1800 - 3600, h) && std::fabs(h - 0.35f) < 1e-5f);  // tolérance 1 h
    CHECK(!ser->at(t0 - 1800 - 3601, h));
    long long tt; double hh;
    CHECK(WaterLevelSeries::ParseLine("2026-11-01,0.35", 0, tt, hh) && std::fabs(hh - 0.35) < 1e-9);
    CHECK(WaterLevelSeries::ParseLine("2026-11-01;05:00:00;0,35", 0, tt, hh) && std::fabs(hh - 0.35) < 1e-9);
    CHECK(FormatTime(t0) == "2026-11-01 05:30");
    // sondes horodatées, profondeurs positives puis altitudes négatives
    const std::string pts = Tmp("pd_wl_pts.txt");
    {
      std::ofstream o(pts);
      o << "lon,lat,z,date\n106.5,52.3,10.00,2026-11-01T05:30:00\n106.5001,52.3,12.00,2026-11-01T06:00:00\n"
           "106.5002,52.3,11.00,2026-11-03T06:00:00\n";
    }
    LoadOptions lo;
    lo.text.custom = true; lo.text.sep = ','; lo.text.skipLines = 1;
    lo.text.colX = 1; lo.text.colY = 2; lo.text.colZ = 3; lo.text.colDate = 4;
    lo.zsign = LoadOptions::ZSign::DepthPositive;
    lo.water = ser;
    PointCloud pc;
    CHECK(LoadXYZ(pts, lo, pc, e) && pc.size() == 2 && pc.waterMiss == 1 && pc.waterApplied);
    CHECK(std::fabs(pc.z[0] - 9.55f) < 1e-4f && std::fabs(pc.z[1] - 11.45f) < 1e-4f);
    {
      std::ofstream o(pts);
      o << "lon,lat,z,date\n106.5,52.3,-10.00,2026-11-01T05:30:00\n106.5001,52.3,-12.00,2026-11-01T06:00:00\n";
    }
    lo.zsign = LoadOptions::ZSign::Auto;  // altitudes négatives détectées
    CHECK(LoadXYZ(pts, lo, pc, e) && pc.size() == 2 && std::fabs(pc.z[0] - 9.55f) < 1e-4f);
    lo.waterSubtract = false;
    CHECK(LoadXYZ(pts, lo, pc, e) && std::fabs(pc.z[0] - 10.45f) < 1e-4f);
    // sondes sans heure (lon lat z) : niveau du jour du levé (Baikal)
    {
      std::ofstream o(pts);
      o << "106.5 52.3 10.00\n106.5001 52.3 12.00\n";
    }
    LoadOptions la;
    la.zsign = LoadOptions::ZSign::DepthPositive;
    la.water = ser;
    CHECK(!LoadXYZ(pts, la, pc, e));  // sans date du levé : aucune sonde corrigée
    CHECK(ParseDateTime("2026-11-01", "06:00", 0, la.waterTime));
    CHECK(LoadXYZ(pts, la, pc, e) && pc.size() == 2 && pc.waterApplied);
    CHECK(std::fabs(pc.z[0] - 9.45f) < 1e-4f && std::fabs(pc.z[1] - 11.45f) < 1e-4f);
    CHECK(SurveyDateFromName("/a/b/leve_20130810_selenga.xyz") == "2013-08-10");
    CHECK(SurveyDateFromName("/a/b/leve.xyz").empty());
    std::printf("Hauteurs d'eau observées : OK\n");
  }
  {  // langues : russe, repli sur l'anglais, retour au français
    pd::SetLanguage(pd::kRussian);
    CHECK(std::string(pd::tr("Marée")) == "Прилив");
    CHECK(std::string(pd::tr("Distance max")) == "Макс. расстояние");
    CHECK(std::string(pd::tr("texte sans traduction")) == "texte sans traduction");
    char b[64];
    std::snprintf(b, sizeof b, pd::tr("%u fichier%s"), 3u, "s");
    CHECK(std::string(b) == "файлов: 3");
    pd::SetLanguage(pd::kEnglish);
    CHECK(std::string(pd::tr("Marée")) == "Tide" && pd::IsEnglish() && !pd::IsRussian());
    pd::SetLanguage(pd::kFrench);
    CHECK(std::string(pd::tr("Marée")) == "Marée");
    std::printf("Langues (fr / en / ru) : OK\n");
  }
  {  // fichiers de traduction .po (anglais -> langue), lus au démarrage
    CHECK(pd::FormatSignature("%d de %s, %.2f %% %lld") == "dsfd");
    // aller-retour : modèle russe écrit puis relu = table compilée
    const std::string po = Tmp("pd_ru.po");
    std::string e;
    CHECK(pd::WriteTranslationTemplate(po, pd::kRussian, e));
    CHECK(pd::LoadTranslations({po}) > 700);
    static const char* const fr[][2] = {
#include "../src/core/LangTable.inc"
    };
    pd::SetLanguage(pd::kRussian);
    std::vector<std::string> viaPo;
    for (const auto& x : fr) viaPo.push_back(pd::tr(x[0]));
    pd::ClearTranslations();
    size_t same = 0;
    for (size_t i = 0; i < viaPo.size(); ++i) same += viaPo[i] == pd::tr(fr[i][0]);
    CHECK(same == viaPo.size());
    // deux textes français de même anglais (« Soundings ») restent distincts
    CHECK(pd::LoadTranslations({po}) > 700);
    CHECK(std::string(pd::tr("Sondes chiffrées")) != pd::tr("Sondes"));
    // fichier de l'utilisateur : remplace une entrée ; format faux : ignoré
    const std::string user = Tmp("pd_ru_user.po");
    if (FILE* f = std::fopen(user.c_str(), "wb")) {
      std::fputs("msgid \"Tide\"\nmsgstr \"Прилив (исправлено)\"\n\n"
                 "msgid \"%u file%s\"\nmsgstr \"файлов %s\"\n\n"
                 "msgid \"Max distance\"\nmsgstr \"\"\n", f);
      std::fclose(f);
    }
    std::string notes;
    CHECK(pd::LoadTranslations({po, user}, &notes) > 700);
    CHECK(std::string(pd::tr("Marée")) == "Прилив (исправлено)");
    char b[64];
    std::snprintf(b, sizeof b, pd::tr("%u fichier%s"), 3u, "s");
    CHECK(std::string(b) == "файлов: 3");  // entrée fautive ignorée
    CHECK(notes.find("%u file%s") != std::string::npos);
    CHECK(std::string(pd::tr("Distance max")) == "Макс. расстояние");  // vide : ignorée
    // autre langue, sans table compilée : fichier seul, sinon anglais
    pd::ClearTranslations();
    if (FILE* f = std::fopen(user.c_str(), "wb")) {
      std::fputs("msgid \"Tide\"\nmsgstr \"Gezeiten\"\n", f);
      std::fclose(f);
    }
    CHECK(pd::LoadTranslations({user}) == 1);
    pd::SetLanguage(pd::kOther);
    CHECK(std::string(pd::tr("Marée")) == "Gezeiten" && std::string(pd::tr("Distance max")) == "Max distance");
    CHECK(pd::LoadTranslations({Tmp("absent.po")}) == -1);
    pd::ClearTranslations();
    pd::SetLanguage(pd::kFrench);
    std::printf("Fichiers de traduction .po : OK\n");
  }
  {  // interpolation entre les lignes d'un levé monofaisceau, à l'import
    std::string err;
    DepthTile a, b;
    a.reset();
    for (int k = 0; k < 1000; ++k) { a.v[k] = 1000 + k; a.setInterp(k, k % 3 == 0); }
    std::vector<uint8_t> blob;
    CHECK(EncodeTile(a, blob) && DecodeTile(blob.data(), blob.size(), b, err));
    CHECK(b.v == a.v && b.isInterp(3) && !b.isInterp(4));
    const std::string folder = Tmp("pd_testdb_interp");
    std::filesystem::remove_all(folder);
    BathyDb db;
    CHECK(db.open(folder, err));
    // lignes est-ouest espacées de 100 m, une sonde tous les 3 m ; fond en pente
    // vers le nord : 10 m + 0,01 m par mètre
    const double lat0 = 48.30, kx = 1.0 / (111320.0 * std::cos(lat0 * M_PI / 180)), ky = 1.0 / 110540.0;
    PointCloud L;
    std::mt19937 jit(11);
    std::uniform_real_distribution<double> gps(-0.4, 0.4);  // bruit GPS (m)
    for (int line = 0; line < 8; ++line)
      for (int i = 0; i < 300; ++i)
        L.add(-4.50 + (i * 3.0 + gps(jit)) * kx, lat0 + (line * 100.0 + gps(jit)) * ky, 10.f + 0.01f * line * 100.f);
    L.computeBounds();
    const SurveyShape sh = AnalyzeSurvey(L);
    CHECK(sh.lines && !sh.isGrid());
    DbSource m;
    m.name = "SBES";
    m.date = "2020-01-01";
    m.res = sh.storeRes;
    m.interpD = sh.maxEdge;
    int64_t id = 0;
    CHECK(db.importCloud(L, m, false, DbPriority::ResolutionDate, err, nullptr, nullptr, &id));
    const DbSource* sp = db.source(id);
    CHECK(sp && sp->zmin > 9.99f && sp->zmax < 17.01f);
    // vue entre les lignes 2 et 3 (y = 250 m) : valeur interpolée ~12,5 m
    PointCloud v;
    double dl, dt;
    int used;
    std::vector<int64_t> src;
    std::vector<uint8_t> ip;
    const double lo = -4.50 + 450 * kx, la = lat0 + 250 * ky;
    CHECK(db.buildView(lo - 5 * kx, lo + 5 * kx, la - 5 * ky, la + 5 * ky, 18, 100000, v, dl, dt, used, err,
                       nullptr, &src, &ip));
    CHECK(!v.empty() && ip.size() == v.size());
    size_t ni = 0;
    for (size_t k = 0; k < v.size(); ++k) {
      if (!ip[k]) continue;
      ++ni;
      CHECK(std::fabs(v.z[k] - 12.5f) < 0.15f);
    }
    CHECK(ni > v.size() / 2);
    // zones interpolées masquées
    CHECK(!db.buildView(lo - 5 * kx, lo + 5 * kx, la - 5 * ky, la + 5 * ky, 18, 100000, v, dl, dt, used, err,
                        nullptr, &src, &ip, true) || v.size() < ni);
    // au-delà de D (pas de ligne au nord de la dernière) : rien
    const double laN = lat0 + 900 * ky;
    CHECK(!db.buildView(lo - 5 * kx, lo + 5 * kx, laN - 5 * ky, laN + 5 * ky, 18, 100000, v, dl, dt, used, err,
                        nullptr, &src, &ip));
    // une source mesurée de plus basse priorité l'emporte sur l'interpolé
    PointCloud P;
    for (int j = -10; j <= 10; ++j)
      for (int i = -10; i <= 10; ++i) P.add(lo + i * 1.0 * kx, la + j * 1.0 * ky, 30.f);
    P.computeBounds();
    DbSource mp;
    mp.name = "ancien";
    mp.date = "1900-01-01";
    mp.res = 50;  // plus grossier : priorité plus basse
    CHECK(db.importCloud(P, mp, false, DbPriority::ResolutionDate, err));
    CHECK(db.buildView(lo - 2 * kx, lo + 2 * kx, la - 2 * ky, la + 2 * ky, 18, 100000, v, dl, dt, used, err,
                       nullptr, &src, &ip));
    bool sawOld = false;
    for (size_t k = 0; k < v.size(); ++k) sawOld = sawOld || (!ip[k] && std::fabs(v.z[k] - 30.f) < 0.01f);
    CHECK(sawOld);
    {  // surface stable au zoom : un point entre deux lignes garde sa valeur
       // interpolée (à 0,1 m près) quel que soit le niveau de la vue
      const double lp = -4.50 + 300 * kx, lq = lat0 + 350 * ky;
      for (int vz : {14, 15, 16, 17, 18, 20}) {
        PointCloud w;
        std::vector<uint8_t> wi;
        CHECK(db.buildView(lp - 40 * kx, lp + 40 * kx, lq - 40 * ky, lq + 40 * ky, vz, 100000, w, dl, dt, used,
                           err, nullptr, &src, &wi));
        size_t best = 0;
        double bd = 1e30;
        for (size_t k = 0; k < w.size(); ++k) {
          const double d = std::hypot((w.lon(k) - lp) / kx, (w.lat(k) - lq) / ky);
          if (d < bd) { bd = d; best = k; }
        }
        CHECK(bd < 40 && std::fabs(w.z[best] - 13.5f) < 0.2f);
      }
    }
    std::printf("Interpolation à l'import (lignes à 100 m, D %.0f m) : OK\n", m.interpD);
    // autres méthodes : même zone, valeurs voisines entre deux lignes (12,5 m)
    for (int meth : {1, 2}) {
      const std::string f2 = Tmp(meth == 1 ? "pd_testdb_nn" : "pd_testdb_idw");
      std::filesystem::remove_all(f2);
      BathyDb d2;
      CHECK(d2.open(f2, err));
      DbSource m2 = m;
      m2.interpMethod = meth;
      int64_t id2 = 0;
      CHECK(d2.importCloud(L, m2, false, DbPriority::ResolutionDate, err, nullptr, nullptr, &id2));
      PointCloud v2;
      std::vector<uint8_t> ip2;
      CHECK(d2.buildView(lo - 5 * kx, lo + 5 * kx, la - 5 * ky, la + 5 * ky, 18, 100000, v2, dl, dt, used, err,
                         nullptr, &src, &ip2));
      size_t n2 = 0;
      for (size_t k = 0; k < v2.size(); ++k) {
        if (!ip2[k]) continue;
        ++n2;
        CHECK(std::fabs(v2.z[k] - 12.5f) < 0.3f);
      }
      CHECK(n2 > v2.size() / 2);
      CHECK(d2.source(id2) &&  // méthode notée dans l'analyse de la source
            d2.source(id2)->info.find(meth == 1 ? "voisin naturel" : "IDW") != std::string::npos);
    }
    {  // InterpolatePixels : plan z = x + 2y sur une grille de sommets
      std::vector<double> vx, vv;
      for (int j = 0; j <= 40; j += 8)
        for (int i = 0; i <= 40; i += 8) { vx.push_back(i); vx.push_back(j); vv.push_back(i + 2.0 * j); }
      std::vector<int> pix = {12, 12, 20, 27, 31, 5, 7, 33};
      std::vector<double> r;
      InterpolatePixels(InterpMethod::NaturalNeighbour, vx, vv, pix, 12.0, r);
      for (size_t q = 0; q < r.size(); ++q) {
        const double ex = (pix[2 * q] + 0.5) + 2 * (pix[2 * q + 1] + 0.5);
        CHECK(std::isfinite(r[q]) && std::fabs(r[q] - ex) < 2.0);  // Sibson discret : ~plan
      }
      std::vector<int> onSite = {8, 16};  // centre du pixel à 0,5 du sommet (8, 16)
      InterpolatePixels(InterpMethod::Idw, vx, vv, onSite, 12.0, r);
      CHECK(std::isfinite(r[0]) && std::fabs(r[0] - 40.0) < 6.0);
      InterpolatePixels(InterpMethod::Tin, vx, vv, onSite, 12.0, r);
      CHECK(std::isnan(r[0]));  // TIN : valeurs calculées ailleurs
    }
    std::printf("Méthodes d'interpolation (TIN, voisin naturel, IDW) : OK\n");
  }
  {  // triangulation (delaunator) : jamais de boucle sans fin, toujours de Delaunay
    // Deux cas réels qui bloquaient l'ancienne version (parcours de l'enveloppe
    // par hull_next) : centres de pixels, coordonnées entières + 0,5.
    static const int kCase1[] = {
        6, 0, 18, 0, 30, 0, 66, 0, 72, 0, 90, 0, 96, 0, 30, 1, 60, 1, 78, 1,
        102, 1, 12, 2, 30, 2, 48, 2, 54, 2, 96, 2, 108, 2, 18, 3, 84, 3, 90, 3,
        102, 3, 108, 3, 84, 4, 12, 5, 24, 5, 36, 5, 78, 5, 84, 5, 102, 5,
    };
    static const int kCase2[] = {  // tuile d'un export Garmin Quickdraw
        -98, -106, -97, -106, -90, -106, -83, -106, -82, -106, -98, -102, -90, -102, -83, -102, -98, -99, -90, -99,
        -83, -99, -98, -95, -90, -95, -83, -95, -98, -92, -90, -92, -83, -92, -98, -89, -90, -89, -84, -89,
        -83, -89, -98, -85, -90, -85, -84, -85, -83, -85, -98, -82, -90, -82, -84, -82, -99, -78, -98, -78,
        -90, -78, -84, -78, -99, -75, -98, -75, -90, -75, -84, -75, -99, -72, -98, -72, -90, -72, -85, -72,
        -84, -72, -99, -68, -91, -68, -90, -68, -85, -68, -99, -65, -91, -65, -90, -65, -85, -65, -99, -62,
        -91, -62, -90, -62, -85, -62, -99, -58, -91, -58, -90, -58, -85, -58, -99, -55, -91, -55, -90, -55,
        -86, -55, -85, -55, -99, -51, -91, -51, -90, -51, -86, -51, -85, -51, -99, -48, -91, -48, -90, -48,
        -86, -48, -99, -45, -92, -45, -91, -45, -90, -45, -86, -45, -99, -41, -92, -41, -91, -41, -90, -41,
        -86, -41, -99, -38, -92, -38, -91, -38, -90, -38, -89, -38, -88, -38, -87, -38, -86, -38, -99, -34,
        -92, -34, -91, -34, -90, -34, -89, -34, -88, -34, -87, -34, -86, -34, -99, -31, -92, -31, -91, -31,
        -90, -31, -89, -31, -88, -31, -87, -31, -99, -28, -92, -28, -91, -28, -90, -28, -89, -28, -88, -28,
        -87, -28, -99, -24, -92, -24, -91, -24, -90, -24, -89, -24, -88, -24, -87, -24, -99, -21, -93, -21,
        -92, -21, -91, -21, -90, -21, -89, -21, -88, -21, -87, -21, -99, -18, -93, -18, -92, -18, -91, -18,
        -90, -18, -89, -18, -88, -18, -87, -18, -99, -14, -93, -14, -92, -14, -91, -14, -90, -14, -89, -14,
        -88, -14, -100, -11, -99, -11, -93, -11, -92, -11, -91, -11, -90, -11, -89, -11, -88, -11, -100, -7,
        -99, -7, -93, -7, -92, -7, -91, -7, -90, -7, -89, -7, -88, -7, -100, -4, -99, -4, -93, -4,
        -92, -4, -91, -4, -90, -4, -89, -4, -88, -4, -100, -1, -99, -1, -93, -1, -92, -1, -91, -1,
        -90, -1, -89, -1, 175, -106, 175, -102, 175, -99, 175, -95, 174, -72, 174, -68, 174, -65, 173, -62,
        174, -62, 173, -58, 174, -58, 173, -45, 172, -21, 172, -18, 171, -7, 171, -4, -100, 3, -94, 3,
        -93, 3, -92, 3, -91, 3, -90, 3, -89, 3, -100, 6, -94, 6, -93, 6, -92, 6, -91, 6,
        -90, 6, -89, 6, -100, 10, -94, 10, -93, 10, -92, 10, -91, 10, -90, 10, -89, 10, -94, 13,
        -93, 13, -92, 13, -91, 13, -90, 13, -100, 16, -94, 16, -93, 16, -92, 16, -91, 16, -90, 16,
        -100, 20, -94, 20, -93, 20, -92, 20, -91, 20, -90, 20, -101, 23, -95, 23, -94, 23, -93, 23,
        -92, 23, -91, 23, -90, 23, -101, 26, -95, 26, -94, 26, -93, 26, -92, 26, -91, 26, -101, 30,
        -95, 30, -94, 30, -93, 30, -92, 30, -91, 30, -101, 33, -95, 33, -94, 33, -93, 33, -92, 33,
        -91, 33, -101, 37, -95, 37, -94, 37, -93, 37, -92, 37, -91, 37, -102, 40, -96, 40, -95, 40,
        -94, 40, -93, 40, -92, 40, -102, 43, -96, 43, -95, 43, -94, 43, -93, 43, -92, 43, -102, 47,
        -96, 47, -95, 47, -94, 47, -93, 47, -92, 47, -102, 50, -96, 50, -95, 50, -94, 50, -93, 50,
        -92, 50, -103, 54, -102, 54, -96, 54, -95, 54, -94, 54, -93, 54, -103, 57, -102, 57, -97, 57,
        -96, 57, -95, 57, -94, 57, -93, 57, -103, 60, -102, 60, -97, 60, -96, 60, -95, 60, -94, 60,
        -93, 60, -103, 64, -102, 64, -97, 64, -96, 64, -95, 64, -94, 64, -93, 64, -103, 67, -102, 67,
        -97, 67, -96, 67, -95, 67, -94, 67, -103, 70, -102, 70, -97, 70, -96, 70, -95, 70, -94, 70,
        -103, 74, -102, 74, -98, 74, -97, 74, -96, 74, -95, 74, -94, 74, -103, 77, -102, 77, -98, 77,
        -97, 77, -96, 77, -95, 77, -104, 81, -103, 81, -102, 81, -98, 81, -97, 81, -96, 81, -95, 81,
        -104, 84, -103, 84, -102, 84, -98, 84, -97, 84, -96, 84, -95, 84, -104, 87, -103, 87, -102, 87,
        -101, 87, -100, 87, -99, 87, -98, 87, -97, 87, -96, 87, -104, 91, -103, 91, -102, 91, -101, 91,
        -100, 91, -99, 91, -98, 91, -97, 91, -96, 91, -104, 94, -103, 94, -102, 94, -101, 94, -100, 94,
        -99, 94, -98, 94, -97, 94, -105, 98, -104, 98, -103, 98, -102, 98, -101, 98, -100, 98, -99, 98,
        -98, 98, -97, 98, -105, 101, -104, 101, -103, 101, -102, 101, -101, 101, -100, 101, -105, 104, -104, 104,
        -103, 104, -102, 104, -101, 104, -105, 108, -104, 108, -103, 108, -102, 108, -105, 111, -104, 111, -103, 111,
        -102, 111, -106, 114, -105, 114, -104, 114, -103, 114, -102, 114, -106, 118, -105, 118, -104, 118, -103, 118,
        -106, 121, -105, 121, -104, 121, -103, 121, -106, 125, -105, 125, -104, 125, -103, 125, -106, 128, -105, 128,
        -104, 128, -103, 128, -107, 131, -106, 131, -105, 131, -104, 131, -103, 131, -107, 135, -106, 135, -105, 135,
        -104, 135, -103, 135, -107, 138, -106, 138, -105, 138, -104, 138, -103, 138, -107, 141, -106, 141, -105, 141,
        -104, 141, -103, 141, -107, 145, -106, 145, -105, 145, -104, 145, -103, 145, -107, 148, -106, 148, -105, 148,
        -104, 148, -103, 148, -108, 152, -107, 152, -106, 152, -105, 152, -104, 152, -103, 152, -108, 155, -107, 155,
        -106, 155, -105, 155, -104, 155, -103, 155, -108, 158, -107, 158, -106, 158, -105, 158, -104, 158, -103, 158,
        -108, 162, -107, 162, -106, 162, -105, 162, -104, 162, -103, 162, -108, 165, -107, 165, -106, 165, -105, 165,
        -104, 165, -103, 165, -108, 169, -107, 169, -106, 169, -105, 169, -104, 169, -103, 169, -108, 172, -107, 172,
        -106, 172, -105, 172, -104, 172, -103, 172, -108, 175, -107, 175, -106, 175, -105, 175, -104, 175, -103, 175,
        -108, 179, -107, 179, -106, 179, -105, 179, -104, 179, -103, 179, -108, 182, -107, 182, -106, 182, -105, 182,
        -104, 182, -103, 182, -108, 185, -107, 185, -106, 185, -105, 185, -104, 185, -103, 185, -108, 189, -107, 189,
        -106, 189, -105, 189, -104, 189, -103, 189, -108, 192, -107, 192, -106, 192, -105, 192, -104, 192, -103, 192,
        -108, 196, -107, 196, -106, 196, -105, 196, -104, 196, -108, 199, -107, 199, -106, 199, -105, 199, -104, 199,
        -108, 202, -107, 202, -106, 202, -105, 202, -104, 202, -108, 206, -107, 206, -106, 206, -105, 206, -104, 206,
        -108, 209, -107, 209, -106, 209, -105, 209, -108, 213, -107, 213, -106, 213, -108, 216, -107, 216, -106, 216,
        -108, 219, -107, 219, -108, 223, -108, 226, 170, 20, 170, 33, 170, 37, 168, 60, 169, 60, 168, 64,
        168, 67, 168, 70, 168, 74, 167, 98, 167, 101, 167, 104, 166, 108, 167, 108, 166, 111, 167, 111,
        165, 135, 166, 135, 165, 148, 164, 175, 163, 189, 163, 202, 163, 206, 161, 229, 162, 229, 161, 233,
        161, 236, 161, 240, 161, 243, 160, 270, 160, 273, 159, 284, 159, 287, 159, 297, 158, 324, 157, 348,
        157, 351, 156, 361,
    };
    auto run = [](const int* c, size_t n2) {
      std::vector<double> xy(n2);
      for (size_t i = 0; i < n2; ++i) xy[i] = c[i] + 0.5;
      return pd::Triangulate(xy).size() / 3;
    };
    CHECK(run(kCase1, sizeof kCase1 / sizeof(int)) == 40);    // = scipy.spatial.Delaunay
    CHECK(run(kCase2, sizeof kCase2 / sizeof(int)) == 1165);
    // grilles trouées aléatoires (comme des pixels de sondes) ; sur une partie,
    // propriété de Delaunay vérifiée en arithmétique entière exacte : aucun
    // sommet strictement dans le cercle circonscrit d'un triangle
    const bool light = std::getenv("PD_TEST_LIGHT") != nullptr;
    const int iters = light ? 3000 : 20000;
    std::mt19937 rg(12345);
    int done = 0, checked = 0, bad = 0;
    for (int it = 0; it < iters; ++it) {
      const int W = 5 + rg() % 200, H = 5 + rg() % 200;
      const double keep = 0.005 + (rg() % 1000) / 1000.0 * 0.5;
      const int sx = 1 + rg() % 9, sy = 1 + rg() % 9;
      std::vector<double> xy;
      std::vector<long long> P;  // coordonnées doublées (entières)
      for (int y = 0; y < H; y += sy)
        for (int x = 0; x < W; x += sx)
          if ((rg() % 100000) / 100000.0 < keep) {
            xy.push_back(x + 0.5); xy.push_back(y + 0.5);
            P.push_back(2 * x + 1); P.push_back(2 * y + 1);
          }
      if (xy.size() < 6) continue;
      const std::vector<size_t> t = pd::Triangulate(xy);
      ++done;
      const size_t n = xy.size() / 2;
      if (it % 10 != 0 || n > 400) continue;
      ++checked;
      for (size_t k = 0; k + 2 < t.size(); k += 3) {
        long long ax = P[2 * t[k]], ay = P[2 * t[k] + 1], bx = P[2 * t[k + 1]], by = P[2 * t[k + 1] + 1],
                  cx = P[2 * t[k + 2]], cy = P[2 * t[k + 2] + 1];
        const long long orient = (bx - ax) * (cy - ay) - (by - ay) * (cx - ax);
        if (orient == 0) { ++bad; continue; }  // triangle plat
        for (size_t q = 0; q < n; ++q) {
          const long long dx = P[2 * q], dy = P[2 * q + 1];
          const long long adx = ax - dx, ady = ay - dy, bdx = bx - dx, bdy = by - dy, cdx = cx - dx, cdy = cy - dy;
          const long long det = (adx * adx + ady * ady) * (bdx * cdy - cdx * bdy) -
                                (bdx * bdx + bdy * bdy) * (adx * cdy - cdx * ady) +
                                (cdx * cdx + cdy * cdy) * (adx * bdy - bdx * ady);
          if ((orient > 0 && det > 0) || (orient < 0 && det < 0)) ++bad;  // strictement dedans
        }
      }
    }
    CHECK(bad == 0 && done > iters / 2);
    std::printf("Triangulation : %d grilles aléatoires, %d vérifiées (Delaunay exact), cas réels : OK\n",
                done, checked);
  }
  {  // Grille .pdgrid (surface de référence verticale) : écriture minimale
     // (deflate « stocké »), lecture par tuiles, échantillonnage, zone
    const int T = 32, nx = 40, ny = 35, ntx = 2, nty = 2;  // 1° : lon 0..39, lat 10..44
    auto val = [](int i, int j) { return 100000 + 1000 * i - 700 * j + (i * j % 7) * 3; };  // mm
    auto zlibStored = [](const std::vector<uint8_t>& d) {
      std::vector<uint8_t> o = {0x78, 0x01};
      size_t p = 0;
      do {
        const size_t n = std::min<size_t>(65535, d.size() - p);
        o.push_back(p + n >= d.size() ? 1 : 0);
        o.push_back(n & 255); o.push_back(n >> 8); o.push_back(~n & 255); o.push_back((~n >> 8) & 255);
        o.insert(o.end(), d.begin() + p, d.begin() + p + n);
        p += n;
      } while (p < d.size());
      uint32_t a = 1, b = 0;
      for (uint8_t c : d) { a = (a + c) % 65521; b = (b + a) % 65521; }
      const uint32_t ad = (b << 16) | a;
      for (int k = 3; k >= 0; --k) o.push_back((ad >> (8 * k)) & 255);
      return o;
    };
    std::vector<uint8_t> head(272, 0), index, data;
    auto put = [](std::vector<uint8_t>& v, size_t at, const void* src, size_t n) {
      std::memcpy(v.data() + at, src, n);
    };
    std::memcpy(head.data(), "PDGRID01", 8);
    const uint32_t tt = T, flags = 0;
    const int32_t dims[4] = {nx, ny, ntx, nty};
    const double geo[5] = {0.0, 10.0, 1.0, 1.0, 0.001};
    put(head, 8, &tt, 4); put(head, 12, dims, 16); put(head, 28, &flags, 4); put(head, 32, geo, 40);
    std::memcpy(head.data() + 72, "MSS/WGS84", 9);
    std::memcpy(head.data() + 104, "TEST", 4);
    const long long base = 272 + 16LL * ntx * nty;
    for (int tj = 0; tj < nty; ++tj)
      for (int ti = 0; ti < ntx; ++ti) {
        std::vector<uint8_t> raw, mask(T * T / 8, 0);
        std::vector<int32_t> v(T * T);
        bool partial = false;
        for (int r = 0; r < T; ++r)
          for (int q = 0; q < T; ++q) {
            const int i = ti * T + q, j = tj * T + r;
            const bool in = i < nx && j < ny && !(i == 5 && j == 5);  // un trou
            if (in) mask[(r * T + q) >> 3] |= 1 << ((r * T + q) & 7); else partial = true;
            v[r * T + q] = in ? val(i, j) : (q ? v[r * T + q - 1] : (r ? v[(r - 1) * T] : 0));
          }
        if (partial) raw = mask;
        const bool big = (ti == 1 && tj == 1);  // tuile en int32 bruts
        if (big) {
          raw.resize(raw.size() + 4 * T * T);
          std::memcpy(raw.data() + raw.size() - 4 * T * T, v.data(), 4 * T * T);
        } else {
          const size_t at = raw.size();
          raw.resize(at + 4 + 2 * T * T);
          std::memcpy(raw.data() + at, &v[0], 4);
          for (int r = 0; r < T; ++r)
            for (int q = 0; q < T; ++q) {
              const int k = r * T + q;
              const int d = q ? v[k] - v[k - 1] : (r ? v[k] - v[k - T] : 0);
              raw[at + 4 + k] = (uint8_t)(d & 255);
              raw[at + 4 + T * T + k] = (uint8_t)((d >> 8) & 255);
            }
        }
        const std::vector<uint8_t> z = zlibStored(raw);
        uint8_t e[16] = {0};
        const uint64_t off = base + data.size();
        const uint32_t sz = (uint32_t)z.size();
        std::memcpy(e, &off, 8); std::memcpy(e + 8, &sz, 4);
        e[12] = big ? (partial ? 4 : 3) : (partial ? 2 : 1);
        index.insert(index.end(), e, e + 16);
        data.insert(data.end(), z.begin(), z.end());
      }
    {
      FILE* f = std::fopen(Tmp("pd_test.pdgrid").c_str(), "wb");
      CHECK(f);
      std::fwrite(head.data(), 1, head.size(), f);
      std::fwrite(index.data(), 1, index.size(), f);
      std::fwrite(data.data(), 1, data.size(), f);
      std::fclose(f);
    }
    GeoGridFile gf;
    CHECK(gf.open(Tmp("pd_test.pdgrid"), err));
    CHECK(gf.model() == "TEST" && gf.quantity() == "MSS/WGS84");
    CHECK(std::fabs(gf.sample(3, 12) - val(3, 2) * 0.001) < 1e-9);
    CHECK(std::fabs(gf.sample(35, 40) - val(35, 30) * 0.001) < 1e-9);  // tuile int32
    const double mid = 0.25 * (val(3, 2) + val(4, 2) + val(3, 3) + val(4, 3)) * 0.001;
    CHECK(std::fabs(gf.sample(3.5, 12.5) - mid) < 1e-9);
    CHECK(std::isnan(gf.sample(-1.5, 12)) && std::isnan(gf.sample(3, 50)));
    // trou : poids renormalisés sur les 3 nœuds présents
    const double w3 = (val(4, 5) + val(5, 6) + val(4, 6)) * 0.001 / 3.0;
    CHECK(std::fabs(gf.sample(4.5, 15.5) - w3) < 1e-9);
    Grid2D g2;
    CHECK(gf.loadRegion(30.2, 33.7, 38.1, 40.4, g2, err));
    CHECK(g2.nx == 7 && g2.ny == 6 && g2.lonW == 29.0 && g2.latN == 42.0);
    CHECK(std::fabs(g2.sample(31.25, 39.5) - gf.sample(31.25, 39.5)) < 1e-4);
    if (const char* mss = std::getenv("PD_MSS")) {  // fichier réel (facultatif)
      GeoGridFile m;
      CHECK(m.open(mss, err));
      const double b = m.sample(355.5, 48.36666666666666);
      const double b2 = m.sample(-4.5, 48.36666666666666);
      std::printf("MSS %s (%s) Brest : %.4f m\n", m.model().c_str(), m.period().c_str(), b);
      CHECK(std::fabs(b - 51.5101827) < 0.0006 && std::fabs(b2 - b) < 1e-9);
    }
    std::printf("Grille .pdgrid : OK\n");
  }
  {  // Référence verticale commune : niveau moyen, surfaces, migration, mise à jour
    const std::string folder = Tmp("pd_testdatum");
    std::filesystem::remove_all(folder);
    std::filesystem::create_directories(folder);
    // surfaces « BathyElli » de synthèse (points lon lat valeur) : NM/ell et ZH/ell
    auto nmF = [](double lo, double la) { return 51.0 + 0.5 * (lo + 4.5) + 0.2 * (la - 48.3); };
    auto zhF = [](double lo, double la) { return 47.0 + 0.3 * (lo + 4.5) - 0.1 * (la - 48.3); };
    auto writeSurf = [&](const std::string& f, double add, bool zh) {
      FILE* o = std::fopen(f.c_str(), "w");
      for (double la = 48.0; la <= 48.6 + 1e-9; la += 0.02)
        for (double lo = -4.9; lo <= -4.1 + 1e-9; lo += 0.02)
          std::fprintf(o, "%.4f %.4f %.4f\n", lo, la, (zh ? zhF(lo, la) : nmF(lo, la)) + add);
      std::fclose(o);
    };
    const std::string nmFile = Tmp("pd_nm.xyz"), zhFile = Tmp("pd_zh.xyz"), zh2File = Tmp("pd_zh2.xyz");
    writeSurf(nmFile, 0, false);
    writeSurf(zhFile, 0, true);
    writeSurf(zh2File, 0.2, true);  // ZH relevé de 20 cm (nouvelle version des surfaces)
    auto grid = [](double lo0, double lo1, double la0, double la1, double stepM, float z) {
      PointCloud pc;
      const double dla = stepM / 110540.0, dlo = stepM / (111320.0 * std::cos(48.3 * M_PI / 180));
      for (double la = la0; la <= la1; la += dla)
        for (double lo = lo0; lo <= lo1; lo += dlo) pc.add(lo, la, z);
      pc.computeBounds();
      return pc;
    };
    PointCloud A = grid(-4.52, -4.48, 48.30, 48.32, 5.0, 10.f);
    bool gA = false;
    DbSource ma;
    ma.name = "A"; ma.date = "2010-06-01"; ma.res = EstimateResolution(A, gA);
    int64_t ida = 0;
    {  // base « ancienne » : profondeurs au zéro des cartes, colonne datum vide
      BathyDb db;
      CHECK(db.open(folder, err));
      CHECK(db.importCloud(A, ma, gA, DbPriority::ResolutionDate, err, nullptr, nullptr, &ida));
    }
    for (const auto& e : std::filesystem::directory_iterator(folder)) {
      if (e.path().extension() != ".pdb") continue;
      sqlite3* sq = nullptr;
      CHECK(sqlite3_open(e.path().string().c_str(), &sq) == SQLITE_OK);
      sqlite3_exec(sq, "UPDATE sources SET datum=NULL", nullptr, nullptr, nullptr);
      sqlite3_close(sq);
    }
    std::filesystem::remove(std::filesystem::path(folder) / DatumStore::FileName());
    DatumConfig cfg;
    cfg.nmFiles = {nmFile};
    cfg.zhFiles = {zhFile};
    DatumProvider prov(cfg);
    const double plo = -4.50, pla = 48.31;
    const double z0 = nmF(plo, pla) - zhF(plo, pla);  // NM au-dessus du ZH
    PointCloud v;
    double dl, dt;
    int used, org = 0;
    {
      BathyDb db;
      CHECK(db.open(folder, err) && db.needsMigration());
      db.setDatumProvider(&prov);
      CHECK(!db.importCloud(A, ma, gA, DbPriority::ResolutionDate, err));  // migration d'abord
      // vue avant migration : valeurs d'origine, sans décalage
      CHECK(db.buildView(-4.52, -4.48, 48.30, 48.32, 16, 20000000, v, dl, dt, used, err, nullptr,
                         nullptr, nullptr, false, Datum::Cd) && v.zMin == 10.f && v.zMax == 10.f);
      const std::string bak = Tmp("pd_testdatum_v1");
      std::filesystem::remove_all(bak);
      CHECK(db.migrate(bak, DbPriority::ResolutionDate, err));
      CHECK(!db.needsMigration() && db.source(ida)->datum == (int)Datum::Cd);
      CHECK(std::filesystem::exists(bak) && !std::filesystem::is_empty(bak));
      // au niveau moyen : 10 + Z0 ; au zéro des cartes : 10 exactement
      CHECK(db.buildView(plo - 0.002, plo + 0.002, pla - 0.002, pla + 0.002, 16, 20000000, v, dl, dt, used,
                         err, nullptr, nullptr, nullptr, false, Datum::Msl));
      std::printf("Référence verticale : Z0 %.3f m, vue NM %.3f..%.3f m\n", z0, v.zMin, v.zMax);
      CHECK(std::fabs(v.zMin - (10 + z0)) < 0.02 && std::fabs(v.zMax - (10 + z0)) < 0.02);
      CHECK(db.buildView(-4.52, -4.48, 48.30, 48.32, 16, 20000000, v, dl, dt, used, err, nullptr,
                         nullptr, nullptr, false, Datum::Cd, &org));
      CHECK(std::fabs(v.zMin - 10.f) < 0.0011 && std::fabs(v.zMax - 10.f) < 0.0011 && org == kOrigBathyElli);
      // ellipsoïde : hauteur du fond = NM − profondeur_NM (surfaces calculées à la demande)
      CHECK(db.ensureDatumForSources(Datum::Ell, err));
      CHECK(db.buildView(plo - 0.002, plo + 0.002, pla - 0.002, pla + 0.002, 16, 20000000, v, dl, dt, used,
                         err, nullptr, nullptr, nullptr, false, Datum::Ell));
      CHECK(std::fabs(v.zMin - (10 + z0 - nmF(plo, pla))) < 0.03);
      // source au niveau moyen (GEBCO…) : affichée au ZH = valeur − Z0
      PointCloud G = grid(-4.46, -4.44, 48.30, 48.32, 5.0, 30.f);
      DbSource mg;
      mg.name = "G"; mg.res = 5; mg.datum = (int)Datum::Msl;
      int64_t idg = 0;
      CHECK(db.importCloud(G, mg, true, DbPriority::ResolutionDate, err, nullptr, nullptr, &idg));
      CHECK(db.buildView(-4.452, -4.448, 48.308, 48.312, 16, 20000000, v, dl, dt, used, err, nullptr,
                         nullptr, nullptr, false, Datum::Cd));
      const double z0g = nmF(-4.45, 48.31) - zhF(-4.45, 48.31);
      CHECK(std::fabs(v.zMin - (30 - z0g)) < 0.02);
      // LAT sans atlas FES : refusé
      DbSource ml;
      ml.name = "L"; ml.res = 5; ml.datum = (int)Datum::Lat;
      CHECK(!db.importCloud(G, ml, true, DbPriority::ResolutionDate, err) && err.find("FES") != std::string::npos);
      // hauteurs ellipsoïdales (en France : BathyElli NM/ell) : profondeur_NM = z + NM
      PointCloud E = grid(-4.43, -4.41, 48.30, 48.32, 5.0, -40.f);
      DbSource me;
      me.name = "E"; me.res = 5; me.datum = (int)Datum::Ell;
      CHECK(db.importCloud(E, me, true, DbPriority::ResolutionDate, err));
      CHECK(db.buildView(-4.422, -4.418, 48.308, 48.312, 16, 20000000, v, dl, dt, used, err, nullptr,
                         nullptr, nullptr, false, Datum::Msl));
      CHECK(std::fabs(v.zMin - (-40 + nmF(-4.42, 48.31))) < 0.02);
      // non rattachée : jamais décalée
      DbSource mf;
      mf.name = "F"; mf.res = 5; mf.datum = (int)Datum::Free;
      PointCloud F = grid(-4.40, -4.38, 48.30, 48.32, 5.0, 3.f);
      CHECK(db.importCloud(F, mf, true, DbPriority::ResolutionDate, err));
      CHECK(db.buildView(-4.392, -4.388, 48.308, 48.312, 16, 20000000, v, dl, dt, used, err, nullptr,
                         nullptr, nullptr, false, Datum::Cd) && v.zMin == 3.f && v.zMax == 3.f);
    }
    {  // nouvelles surfaces (ZH relevé de 20 cm) : sources au ZH corrigées, affichage au ZH inchangé
      DatumConfig cfg2 = cfg;
      cfg2.zhFiles = {zh2File};
      DatumProvider prov2(cfg2);
      BathyDb db;
      CHECK(db.open(folder, err));
      db.setDatumProvider(&prov2);
      int changed = 0;
      CHECK(db.updateDatum(DbPriority::ResolutionDate, err, nullptr, nullptr, &changed));
      CHECK(changed == 1);  // A (au ZH) ; G (NM), E (ellipsoïde), F : inchangées
      CHECK(db.buildView(-4.52, -4.48, 48.30, 48.32, 16, 20000000, v, dl, dt, used, err, nullptr,
                         nullptr, nullptr, false, Datum::Cd));
      CHECK(std::fabs(v.zMin - 10.f) < 0.0011 && std::fabs(v.zMax - 10.f) < 0.0011);
      CHECK(db.buildView(plo - 0.002, plo + 0.002, pla - 0.002, pla + 0.002, 16, 20000000, v, dl, dt, used,
                         err, nullptr, nullptr, nullptr, false, Datum::Msl));
      CHECK(std::fabs(v.zMin - (10 + z0 - 0.2)) < 0.02);
      CHECK(db.updateDatum(DbPriority::ResolutionDate, err, nullptr, nullptr, &changed) && changed == 0);
      // nettoyage : bornes au zéro des cartes local (10 m), pas au niveau moyen (~14 m)
      long long removed = -1;
      const bool cok = db.cleanSources({ida}, 9.5f, 10.5f, DbPriority::ResolutionDate, err, nullptr, nullptr, &removed);
      CHECK(cok &&
            removed == 0);
      CHECK(db.cleanSources({ida}, 10.5f, 30.f, DbPriority::ResolutionDate, err, nullptr, nullptr, &removed) &&
            removed > 0);
    }
    {  // copies des surfaces dans les .pdb : un .pdb copié seul garde ses surfaces
      int adopted = -1, copied = -1;
      {
        BathyDb db;
        CHECK(db.open(folder, err));
        CHECK(db.syncDatum(err, &adopted, &copied) && adopted == 0 && copied > 0);
        CHECK(db.syncDatum(err, &adopted, &copied) && adopted == 0 && copied == 0);  // déjà à jour
      }
      const std::string solo = Tmp("pd_testdatum_solo");
      std::filesystem::remove_all(solo);
      std::filesystem::create_directories(solo);
      for (const auto& e : std::filesystem::directory_iterator(folder))
        if (e.path().extension() == ".pdb") std::filesystem::copy_file(e.path(), std::filesystem::path(solo) / e.path().filename());
      BathyDb d2;
      CHECK(d2.open(solo, err));
      CHECK(!std::filesystem::exists(std::filesystem::path(solo) / DatumStore::FileName()));
      CHECK(d2.syncDatum(err, &adopted, &copied) && adopted > 0);
      // source G (au niveau moyen) : affichée au ZH avec les surfaces reprises du .pdb
      CHECK(d2.buildView(-4.452, -4.448, 48.308, 48.312, 16, 20000000, v, dl, dt, used, err, nullptr,
                         nullptr, nullptr, false, Datum::Cd));
      const double z0g = nmF(-4.45, 48.31) - zhF(-4.45, 48.31) - 0.2;  // ZH relevé de 20 cm
      std::printf("Surfaces copiées dans les .pdb : %d reprises par la base copiée, vue ZH %.3f (attendu %.3f)\n",
                  adopted, v.zMin, 30 - z0g);
      CHECK(std::fabs(v.zMin - (30 - z0g)) < 0.02);
    }
    {  // zéro des cartes officiel d'un autre pays : mllw.gtx NOAA VDatum (MLLW − LMSL)
      const std::string nd = Tmp("pd_national");
      std::filesystem::remove_all(nd);
      std::filesystem::create_directories(std::filesystem::path(nd) / "vdatum" / "MAcoast01_8301");
      {
        FILE* f = std::fopen((std::filesystem::path(nd) / "vdatum" / "MAcoast01_8301" / "MAcoast01_8301_mllw.gtx").string().c_str(), "wb");
        auto be64 = [&](double x) { uint64_t u; std::memcpy(&u, &x, 8); for (int k = 7; k >= 0; --k) std::fputc((int)(u >> (8 * k)) & 255, f); };
        auto be32 = [&](uint32_t u) { for (int k = 3; k >= 0; --k) std::fputc((int)(u >> (8 * k)) & 255, f); };
        be64(41.0); be64(289.0); be64(0.05); be64(0.05); be32(21); be32(21);  // 41–42° N, 71–70° W (0–360)
        for (int r = 0; r < 21; ++r)
          for (int c = 0; c < 21; ++c) {
            float v = c >= 18 ? -88.8888f : (float)(-1.0 - 0.01 * c);  // MLLW sous le NM ; bord est à terre
            uint32_t u; std::memcpy(&u, &v, 4); be32(u);
          }
        std::fclose(f);
      }
      DatumConfig nc;
      nc.national = ScanNationalDatums(nd);
      CHECK(nc.national.size() == 1 && nc.national[0].name == "MLLW NOAA VDatum" &&
            std::fabs(nc.national[0].lonW + 71.0) < 1e-9);
      DatumProvider np(nc);
      Grid2D sg;
      std::vector<uint8_t> so;
      CHECK(np.compute(Datum::Cd, -70.9, -70.5, 41.4, 41.6, sg, so, err));
      const double sv = sg.sample(-70.75, 41.5);  // c = 5 : MLLW = −1,05 m -> S_cd = 1,05
      int nNat = 0, nNone = 0;
      for (uint8_t o : so) { nNat += o == kOrigNational; nNone += o == kOrigNone; }
      std::printf("Zéro national (VDatum) : S_cd %.3f m, nœuds national %d, sans référence %d (%s)\n", sv, nNat,
                  nNone, DatumOriginText(Datum::Cd, kOrigNational, "", NationalNames(nc.national)).c_str());
      CHECK(std::fabs(sv - 1.05) < 0.002 && nNat == (int)so.size());
      CHECK(np.compute(Datum::Cd, -60.5, -60.4, 41.4, 41.6, sg, so, err) && so[0] == kOrigNone && sg.v[0] == 0.f);
    }
    std::printf("Référence verticale (niveau moyen, migration, mise à jour) : OK\n");
  }
  {  // Masque terre : shapefile de polygones (format OSMSHP d'OpenCPN), île carrée
    const std::string shp = Tmp("pd_land.shp"), shx = Tmp("pd_land.shx"), dbf = Tmp("pd_land.dbf");
    // île : carré lon -4.505..-4.495, lat 48.305..48.315 (anneau extérieur horaire)
    const double ring[5][2] = {{-4.505, 48.305}, {-4.505, 48.315}, {-4.495, 48.315}, {-4.495, 48.305}, {-4.505, 48.305}};
    auto be32 = [](std::vector<uint8_t>& v, uint32_t x) { for (int k = 3; k >= 0; --k) v.push_back((x >> (8 * k)) & 255); };
    auto le = [](std::vector<uint8_t>& v, const void* p, size_t n) { const uint8_t* b = (const uint8_t*)p; v.insert(v.end(), b, b + n); };
    std::vector<uint8_t> rec;  // enregistrement : type 5, emprise, 1 partie, 5 points
    const int32_t t5 = 5, np = 1, npt = 5, part0 = 0;
    const double box[4] = {-4.505, 48.305, -4.495, 48.315};
    le(rec, &t5, 4); le(rec, box, 32); le(rec, &np, 4); le(rec, &npt, 4); le(rec, &part0, 4);
    for (auto& q : ring) le(rec, q, 16);
    auto header = [&](std::vector<uint8_t>& h, uint32_t lenWords) {
      be32(h, 9994); for (int k = 0; k < 5; ++k) be32(h, 0); be32(h, lenWords);
      const int32_t ver = 1000; le(h, &ver, 4); le(h, &t5, 4); le(h, box, 32);
      const double zm[4] = {0, 0, 0, 0}; le(h, zm, 32);
    };
    std::vector<uint8_t> S, X;
    header(S, (uint32_t)(100 + 8 + rec.size()) / 2);
    be32(S, 1); be32(S, (uint32_t)rec.size() / 2); S.insert(S.end(), rec.begin(), rec.end());
    header(X, (100 + 8) / 2);
    be32(X, 50); be32(X, (uint32_t)rec.size() / 2);
    std::vector<uint8_t> D(32, 0);  // .dbf : xmin xmax ymin ymax (N 24)
    D[0] = 3; const uint32_t one = 1; std::memcpy(&D[4], &one, 4);
    const uint16_t hl = 32 + 4 * 32 + 1, rl = 1 + 4 * 24;
    std::memcpy(&D[8], &hl, 2); std::memcpy(&D[10], &rl, 2);
    const char* names[4] = {"xmin", "xmax", "ymin", "ymax"};
    for (auto n : names) { std::vector<uint8_t> f(32, 0); std::memcpy(f.data(), n, 4); f[11] = 'N'; f[16] = 24; f[17] = 15; D.insert(D.end(), f.begin(), f.end()); }
    D.push_back(0x0D);
    char r[200];
    std::snprintf(r, sizeof r, " %24.15f%24.15f%24.15f%24.15f", box[0], box[2], box[1], box[3]);
    D.insert(D.end(), r, r + rl);
    D.push_back(0x1A);
    for (auto& fv : {std::make_pair(shp, &S), std::make_pair(shx, &X), std::make_pair(dbf, &D)}) {
      FILE* f = std::fopen(fv.first.c_str(), "wb");
      CHECK(f);
      std::fwrite(fv.second->data(), 1, fv.second->size(), f);
      std::fclose(f);
    }
    LandMask lm;
    CHECK(lm.open(shp, err));
    CHECK(lm.landAt(-4.50, 48.31) && !lm.landAt(-4.51, 48.31) && !lm.landAt(-4.50, 48.32));
    // levé en lignes autour de l'île (sondes jusque sur l'île : interpolation à travers)
    PointCloud L;
    for (double la = 48.295; la <= 48.325; la += 0.0009)
      for (double lo = -4.515; lo <= -4.485; lo += 0.00005) L.add(lo, la, 12.f);
    L.computeBounds();
    const std::string folder = Tmp("pd_testland");
    std::filesystem::remove_all(folder);
    BathyDb db;
    CHECK(db.open(folder, err));
    DbSource m;
    m.name = "L"; m.res = 5; m.interpD = 150; m.interpMethod = 0;
    // sans masque : île couverte ; puis masque appliqué à la source
    int64_t id = 0;
    CHECK(db.importCloud(L, m, false, DbPriority::ResolutionDate, err, nullptr, nullptr, &id));
    PointCloud v;
    double dl, dt;
    int used;
    auto at = [&](double lo, double la) {
      return db.buildView(lo - 0.0003, lo + 0.0003, la - 0.0003, la + 0.0003, 17, 100000, v, dl, dt, used, err)
                 ? v.size() : (size_t)0;
    };
    const size_t before = at(-4.50, 48.3105);  // entre deux lignes, sur l'île
    db.setLandMask(&lm);
    long long removed = 0;
    CHECK(db.applyLandMask({id}, DbPriority::ResolutionDate, err, nullptr, nullptr, &removed));
    const size_t after = at(-4.50, 48.3105);
    std::printf("Masque terre : %zu cellules sur l'île avant, %zu après, %lld pixels retirés\n", before, after, removed);
    CHECK(before > 0 && removed > 0 && after < before && at(-4.51, 48.3105) > 0);
    // import avec le masque : île vide entre les lignes dès l'import
    int64_t id2 = 0;
    CHECK(db.removeSources({id}, DbPriority::ResolutionDate, err));
    CHECK(db.importCloud(L, m, false, DbPriority::ResolutionDate, err, nullptr, nullptr, &id2));
    CHECK(at(-4.50, 48.3105) == after);
    // lac absent du trait de côte (sondes toutes sur la « terre ») : masque non appliqué
    CHECK(db.removeSources({id2}, DbPriority::ResolutionDate, err));
    PointCloud K;
    for (double la = 48.3062; la <= 48.3138; la += 0.0009)
      for (double lo = -4.5038; lo <= -4.4962; lo += 0.00005) K.add(lo, la, 6.f);
    K.computeBounds();
    DbSource mk = m;
    mk.name = "K"; mk.datum = (int)Datum::Free;
    int64_t id3 = 0;
    CHECK(db.importCloud(K, mk, false, DbPriority::ResolutionDate, err, nullptr, nullptr, &id3));
    const size_t lake = at(-4.50, 48.3105);
    CHECK(db.applyLandMask({id3}, DbPriority::ResolutionDate, err, nullptr, nullptr, &removed));
    std::printf("Masque terre, lac inconnu : %zu cellules, %lld pixels retirés\n", lake, removed);
    CHECK(lake > 0 && removed == 0 && at(-4.50, 48.3105) == lake);
    std::printf("Masque terre (OSMSHP) : OK\n");
    // Trait de côte personnel (KML) : lac rectangulaire, sondes à 10 m en lignes
    // jusqu'à ~150 m du bord ; rivage = isobathe 0 pour l'interpolation
    const std::string kml = Tmp("pd_lac.kml");
    {
      FILE* f = std::fopen(kml.c_str(), "wb");
      std::fputs("<?xml version=\"1.0\"?><kml xmlns=\"http://www.opengis.net/kml/2.2\"><Document><Placemark>"
                 "<Polygon><outerBoundaryIs><LinearRing><coordinates>-4.60,48.40,0 -4.58,48.40,0 "
                 "-4.58,48.41,0 -4.60,48.41,0 -4.60,48.40,0</coordinates></LinearRing></outerBoundaryIs>"
                 "</Polygon></Placemark></Document></kml>", f);
      std::fclose(f);
    }
    std::vector<ShoreFeature> sf;
    CHECK(ReadShoreFile(kml, sf, err) && sf.size() == 1 && sf[0].closed && sf[0].x.size() == 4);
    LandMask lk;
    CHECK(lk.addCustom(kml, err) && lk.isOpen() && !lk.hasBase() && lk.hasCustomPolygons());
    PointCloud W;
    for (double la = 48.4015; la <= 48.4086; la += 0.0009)
      for (double lo = -4.598; lo <= -4.582; lo += 0.00005) W.add(lo, la, 10.f);
    W.computeBounds();
    DbSource mw;
    mw.name = "W"; mw.res = 5; mw.interpD = 300; mw.interpMethod = 0; mw.datum = (int)Datum::Free;
    auto depthAt = [&](BathyDb& d, double lo, double la) -> float {
      if (!d.buildView(lo - 0.00005, lo + 0.00005, la - 0.00005, la + 0.00005, 17, 100000, v, dl, dt, used, err) ||
          v.empty())
        return NAN;
      return v.z[v.size() / 2];
    };
    for (int pass = 0; pass < 2; ++pass) {
      const std::string fw = Tmp(pass ? "pd_testshore1" : "pd_testshore0");
      std::filesystem::remove_all(fw);
      BathyDb dw;
      CHECK(dw.open(fw, err));
      dw.setLandMask(&lk);
      dw.setShoreConstraint(pass == 1);
      CHECK(dw.importCloud(W, mw, false, DbPriority::ResolutionDate, err));
      const float edge = depthAt(dw, -4.599, 48.4051), out = depthAt(dw, -4.6015, 48.4051);
      const float mid = depthAt(dw, -4.59, 48.4051);
      std::printf("Trait de côte KML, isobathe 0 %s : bord %.2f m, hors du lac %.2f, centre %.2f\n",
                  pass ? "oui" : "non", edge, out, mid);
      CHECK(std::isnan(out) && std::fabs(mid - 10.f) < 0.01f);
      if (pass) CHECK(edge > 3.f && edge < 7.f);
      else CHECK(std::isnan(edge));
    }
    // même lac en shapefile (lu à la demande, .shx) : polygone (5), puis
    // polyligne refermée (3, contour exporté en ligne par QGIS) prise comme anneau
    for (int shpType : {5, 3}) {
      const std::string shp = Tmp((std::string("pd_lac") + std::to_string(shpType) + ".shp").c_str()), shx = Tmp((std::string("pd_lac") + std::to_string(shpType) + ".shx").c_str());
      const double X[5] = {-4.60, -4.60, -4.58, -4.58, -4.60}, Y[5] = {48.40, 48.41, 48.41, 48.40, 48.40};
      std::vector<uint8_t> rec;
      auto le32 = [&](std::vector<uint8_t>& v, int32_t x) { for (int k = 0; k < 4; ++k) v.push_back((uint8_t)(x >> (8 * k))); };
      auto le64 = [&](std::vector<uint8_t>& v, double d) { uint64_t u; std::memcpy(&u, &d, 8); for (int k = 0; k < 8; ++k) v.push_back((uint8_t)(u >> (8 * k))); };
      auto be32 = [&](std::vector<uint8_t>& v, int32_t x) { for (int k = 3; k >= 0; --k) v.push_back((uint8_t)(x >> (8 * k))); };
      le32(rec, shpType); le64(rec, -4.60); le64(rec, 48.40); le64(rec, -4.58); le64(rec, 48.41);
      le32(rec, 1); le32(rec, 5); le32(rec, 0);
      for (int k = 0; k < 5; ++k) { le64(rec, X[k]); le64(rec, Y[k]); }
      auto header = [&](int32_t lenWords) {
        std::vector<uint8_t> h;
        be32(h, 9994);
        for (int k = 0; k < 5; ++k) be32(h, 0);
        be32(h, lenWords); le32(h, 1000); le32(h, shpType);
        le64(h, -4.60); le64(h, 48.40); le64(h, -4.58); le64(h, 48.41);
        for (int k = 0; k < 4; ++k) le64(h, 0);
        return h;
      };
      std::vector<uint8_t> fshp = header((int32_t)((100 + 8 + rec.size()) / 2));
      be32(fshp, 1); be32(fshp, (int32_t)(rec.size() / 2));
      fshp.insert(fshp.end(), rec.begin(), rec.end());
      std::vector<uint8_t> fshx = header((100 + 8) / 2);
      be32(fshx, 50); be32(fshx, (int32_t)(rec.size() / 2));
      FILE* f = std::fopen(shp.c_str(), "wb"); std::fwrite(fshp.data(), 1, fshp.size(), f); std::fclose(f);
      f = std::fopen(shx.c_str(), "wb"); std::fwrite(fshx.data(), 1, fshx.size(), f); std::fclose(f);
      LandMask ls;
      CHECK(ls.addCustom(shp, err) && ls.isOpen() && ls.hasCustomPolygons());
      const std::string fw = Tmp((std::string("pd_testshore_shp") + std::to_string(shpType)).c_str());
      std::filesystem::remove_all(fw);
      BathyDb dw;
      CHECK(dw.open(fw, err));
      dw.setLandMask(&ls);
      dw.setShoreConstraint(true);
      CHECK(dw.importCloud(W, mw, false, DbPriority::ResolutionDate, err));
      const float edge = depthAt(dw, -4.599, 48.4051), out = depthAt(dw, -4.6015, 48.4051);
      std::printf("Trait de côte shapefile type %d (à la demande) : bord %.2f m, hors du lac %.2f\n", shpType, edge, out);
      CHECK(std::isnan(out) && edge > 3.f && edge < 7.f);
    }
    std::printf("Trait de côte personnel (KML, lac) : OK\n");
  }
  {  // Traces de sondeur : GPX Garmin et journal sonar SL2 synthétique
    const std::string gpx = Tmp("pd_trace.gpx");
    {
      FILE* f = std::fopen(gpx.c_str(), "wb");
      std::fputs("<?xml version=\"1.0\"?><gpx xmlns:gpxx=\"x\"><wpt lat=\"52.24\" lon=\"104.35\"><name>1</name>"
                 "<extensions><gpxx:WaypointExtension><gpxx:Depth>2.5</gpxx:Depth></gpxx:WaypointExtension>"
                 "</extensions></wpt><trk><trkseg>"
                 "<trkpt lat='52.2020' lon='104.3711'><time>2022-08-07T01:44:02Z</time><extensions>"
                 "<gpxx:TrackPointExtension><gpxx:Temperature>15</gpxx:Temperature><gpxx:Depth>0.56</gpxx:Depth>"
                 "</gpxx:TrackPointExtension></extensions></trkpt>"
                 "<trkpt lat=\"52.2021\" lon=\"104.3712\"><time>2022-08-07T01:44:28Z</time></trkpt>"
                 "<trkpt lat=\"52.2022\" lon=\"104.3713\"/></trkseg></trk></gpx>", f);
      std::fclose(f);
    }
    std::vector<TrackSample> ts;
    TrackInfo ti;
    CHECK(ReadGpxSamples(gpx, ts, ti, err) && ti.records == 4 && ti.noDepth == 2 && ts.size() == 2);
    CHECK(std::fabs(ts[0].depth - 2.5f) < 1e-6 && !ts[0].hasT);
    CHECK(ts[1].hasT && ts[1].t == 1659836642LL && std::fabs(ts[1].lon - 104.3711) < 1e-9);
    LoadOptions lo;
    lo.zOffset = 0.4f;  // enfoncement du transducteur
    PointCloud pc;
    CHECK(LoadTrack(gpx, lo, pc, err) && pc.size() == 2 && std::fabs(pc.z[1] - 0.96f) < 1e-5 &&
          pc.tMin == 1659836642LL);
    // SL2 : 2 positions, 3 pings primaires sur la première (médiane), 1 secondaire
    const std::string sl2 = Tmp("pd_sonar.sl2");
    {
      std::vector<uint8_t> F(8, 0);
      const uint16_t hdr[4] = {2, 2, 3200, 1};
      std::memcpy(F.data(), hdr, 8);
      const double R = 6356752.3142;
      auto frame = [&](int ch, float ft, double lon, double lat, int ms, uint32_t date) {
        std::vector<uint8_t> h(144 + 16, 0);
        const uint32_t pos = (uint32_t)F.size();
        const uint16_t size = (uint16_t)h.size(), c = (uint16_t)ch;
        const int32_t e = (int32_t)std::lround(lon * M_PI / 180 * R);
        const int32_t n = (int32_t)std::lround(R * std::log(std::tan(M_PI / 4 + lat * M_PI / 360)));
        std::memcpy(&h[0], &pos, 4); std::memcpy(&h[28], &size, 2); std::memcpy(&h[32], &c, 2);
        std::memcpy(&h[60], &date, 4); std::memcpy(&h[64], &ft, 4);
        std::memcpy(&h[108], &e, 4); std::memcpy(&h[112], &n, 4); std::memcpy(&h[140], &ms, 4);
        F.insert(F.end(), h.begin(), h.end());
      };
      frame(0, 10.f, 31.7, 61.37, 0, 1620821817u);
      frame(1, 99.f, 31.7, 61.37, 0, 0);
      frame(0, 30.f, 31.7, 61.37, 100, 100);
      frame(0, 20.f, 31.7, 61.37, 200, 200);
      frame(0, 0.f, 31.701, 61.371, 1500, 1500);  // fond non trouvé
      frame(0, 40.f, 31.701, 61.371, 1600, 1600);
      FILE* f = std::fopen(sl2.c_str(), "wb");
      std::fwrite(F.data(), 1, F.size(), f);
      std::fclose(f);
    }
    ts.clear();
    CHECK(ReadSonarLogSamples(sl2, ts, ti, err) && ti.kind == "SL2" && ti.records == 6 && ti.channel == 0 &&
          ti.pings == 5 && ti.noDepth == 1 && ts.size() == 2);
    CHECK(std::fabs(ts[0].depth - 20 * 0.3048f) < 1e-4 && std::fabs(ts[1].depth - 40 * 0.3048f) < 1e-4);
    CHECK(std::fabs(ts[0].lon - 31.7) < 2e-5 && std::fabs(ts[0].lat - 61.37) < 2e-5 && ts[0].t == 1620821817LL &&
          ts[1].t == 1620821819LL);
    std::printf("Traces GPX et SL2 : OK\n");
  }
  {  // Écran tourné (route / cap en haut) : l'échelle ne dépend pas de l'angle
    const double clon = -4.8, clat = 48.37, ppd = 20000.0, W = 1900, H = 980;
    double prev = -1;
    for (int deg = 0; deg < 360; deg += 15) {
      const double r = deg * M_PI / 180, cs = std::cos(r), sn = std::sin(r);
      auto ll = [&](double px, double py, double& lo, double& la) {  // pixel -> lon/lat
        const double dx = px - W / 2, dy = py - H / 2;
        const double ex = cs * dx + sn * dy, ny = -(-sn * dx + cs * dy);  // repère nord en haut
        lo = clon + ex / ppd;
        la = 2 * std::atan(std::exp((ScreenMapper::mercY(clat) + ny / ppd) * M_PI / 180)) * 180 / M_PI - 90;
      };
      double lon[3], lat[3], x[3] = {0, W, 0}, y[3] = {H, H, 0};
      for (int k = 0; k < 3; ++k) ll(x[k], y[k], lon[k], lat[k]);
      ScreenMapper sm;
      CHECK(sm.fit(lon, lat, x, y));
      CHECK(std::fabs(sm.pixelsPerDegLon() - ppd) < 1e-3 * ppd);
      float px, py;
      sm.map(clon, clat, px, py);
      CHECK(std::fabs(px - W / 2) < 0.5 && std::fabs(py - H / 2) < 0.5);
      prev = sm.pixelsPerDegLon();
    }
    std::printf("Écran tourné : échelle constante sur 360° (%.1f px/°) : OK\n", prev);
  }
  {  // Données externes : JSON, SHA-256, archives .7z / .zip, catalogue, REFMAR
    std::string err;
    Json j;
    CHECK(Json::Parse("{\"a\":[1,2.5,-3e2],\"b\":{\"c\":\"\\u00e9\\n\",\"d\":true,\"e\":null},\"n\":\"42\"}", j, err));
    CHECK(j["a"].size() == 3 && j["a"][1].num() == 2.5 && j["a"][2].num() == -300);
    CHECK(j["b"]["c"].str() == "\xc3\xa9\n" && j["b"]["d"].boolean() && j["b"]["e"].isNull());
    CHECK(j["n"].integer() == 42 && j["absent"]["x"].str("def") == "def");
    CHECK(!Json::Parse("{\"a\":}", j, err) && !Json::Parse("[1,2", j, err) && !Json::Parse("{} x", j, err));
    CHECK(Sha256::OfString("abc") == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    CHECK(Sha256::OfString("") == "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
    CHECK(Sha256::OfString(std::string(1000, 'a')) == "41edece42d63e8d9bf515a9ba6932e1c20cbc9f5a5d134645adb5db1b9737ea3");
    // archives : même contenu en LZMA (comme BathyElli du Shom), LZMA2, deflate, stocké
    const std::string glhiSha = "1eb2174e9bc5fcbc681034e530413c0e85378e3e6f1e380697acae7eda672430";
    const std::string xmlSha = "7ab706c813b39468eec3eabf3abe5bfa7bd44886495bb29198f6355a802b6d49";
    struct A { const char* name; const unsigned char* d; size_t n; ArchiveKind k; };
    const A arcs[] = {{"lzma.7z", kArc_lzma_7z, sizeof kArc_lzma_7z, ArchiveKind::SevenZip},
                      {"lzma2.7z", kArc_lzma2_7z, sizeof kArc_lzma2_7z, ArchiveKind::SevenZip},
                      {"defl.zip", kArc_defl_zip, sizeof kArc_defl_zip, ArchiveKind::Zip},
                      {"store.zip", kArc_store_zip, sizeof kArc_store_zip, ArchiveKind::Zip}};
    for (const auto& a : arcs) {
      const std::string f = Tmp(std::string("pd_test_") + a.name);
      { std::ofstream o(f, std::ios::binary); o.write((const char*)a.d, (std::streamsize)a.n); }
      CHECK(DetectArchive(f) == a.k);
      std::vector<ArchiveEntry> ls;
      CHECK(ListArchive(f, ls, err));
      bool sawGlhi = false;
      for (const auto& e : ls)
        if (e.name == "BATHYELLI_ZH_ell_V2_1/DATA/BathyElliv2.1_ATL_ZH.glhi") sawGlhi = e.size == 3600 && !e.dir;
      CHECK(sawGlhi);
      const std::string dest = Tmp(std::string("pd_test_x_") + a.name);
      std::filesystem::remove_all(dest);
      std::vector<std::string> w;
      CHECK(ExtractArchive(f, dest, nullptr, false, w, err));
      CHECK(w.size() == 2);
      CHECK(Sha256::OfFile(dest + "/BATHYELLI_ZH_ell_V2_1/DATA/BathyElliv2.1_ATL_ZH.glhi") == glhiSha);
      CHECK(Sha256::OfFile(dest + "/BATHYELLI_ZH_ell_V2_1/notice.xml") == xmlSha);
      // fichiers choisis, sans sous-dossiers
      std::filesystem::remove_all(dest);
      CHECK(ExtractArchive(f, dest, [](const std::string& n) { return catalog::WildMatch("*.glhi", n); }, true, w, err));
      CHECK(w.size() == 1 && Sha256::OfFile(dest + "/BathyElliv2.1_ATL_ZH.glhi") == glhiSha);
      CHECK(catalog::FindFile(dest, "*zh*.GLHI") == w[0] && catalog::FindFile(dest, "*NM*.glhi").empty());
      std::filesystem::remove_all(dest);
    }
    {  // noms dangereux : jamais écrits hors du dossier
      const std::string f = Tmp("pd_test_evil.zip");
      { std::ofstream o(f, std::ios::binary); o.write((const char*)kArc_evil_zip, sizeof kArc_evil_zip); }
      const std::string dest = Tmp("pd_test_x_evil");
      std::filesystem::remove_all(dest);
      std::vector<std::string> w;
      CHECK(ExtractArchive(f, dest, nullptr, false, w, err));
      CHECK(w.size() == 1 && w[0].find("ok") != std::string::npos);
      CHECK(!std::filesystem::exists(Tmp("evil.txt")));
      CHECK(SafeEntryName("../x") == "" && SafeEntryName("C:/x") == "" && SafeEntryName("./a//b\\c") == "a/b/c");
      std::filesystem::remove_all(dest);
      // fichier ordinaire : pas une archive
      CHECK(DetectArchive(Tmp("pd_test_lzma.7z")) == ArchiveKind::SevenZip);
    }
    {  // catalogue intégré : adresses https, empreintes, motifs
      Catalog c;
      CHECK(catalog::Parse(catalog::BuiltinJson(), c, err));
      CHECK(c.format == 1 && c.version >= 1 && c.items.size() >= 6);
      const CatalogItem* zh = c.find("bathyelli-zh");
      CHECK(zh && zh->downloadable() && zh->extract && zh->sha256.size() == 64 && zh->check == "*ZH*.glhi");
      CHECK(zh->title("ru") != zh->title("fr") && !zh->description("xx").empty());
      const CatalogItem* fes = c.find("fes2022b");
      CHECK(fes && fes->manual && !fes->downloadable() && fes->use == "tide");
      const CatalogItem* raf = c.find("raf20");
      CHECK(raf && raf->check == "RAF20.tac" && raf->size == 1685906);
      // adresse non https ou nom avec dossier : refusés
      Catalog bad;
      CHECK(catalog::Parse("{\"format\":1,\"items\":[{\"id\":\"x\",\"download\":{\"url\":\"http://a/b\",\"file\":\"b\"}},"
                           "{\"id\":\"y\",\"download\":{\"url\":\"https://a/b\",\"file\":\"../b\"}}]}", bad, err));
      CHECK(!bad.items[0].downloadable() && !bad.items[1].downloadable());
      CHECK(!catalog::Parse("{\"format\":2,\"items\":[{\"id\":\"x\"}]}", bad, err));
      // cache plus récent pris, plus ancien ignoré
      const std::string cache = Tmp("pd_test_catalog.json");
      { std::ofstream o(cache); o << "{\"format\":1,\"version\":999,\"items\":[{\"id\":\"z\",\"manual\":true}]}"; }
      CHECK(catalog::Load(cache).find("z") != nullptr);
      { std::ofstream o(cache); o << "{\"format\":1,\"version\":0,\"items\":[{\"id\":\"z\",\"manual\":true}]}"; }
      CHECK(catalog::Load(cache).find("bathyelli-zh") != nullptr);
      CHECK(catalog::WildMatch("*mllw*.gtx", "AKglacier00_8301_MLLW.gtx") && !catalog::WildMatch("RAF20.tac", "RAF20.tac.part"));
      CHECK(catalog::WildMatch("RAC2?.mnt", "rac23.mnt") && !catalog::WildMatch("a*b", "ac"));
    }
    {  // REFMAR : liste des marégraphes, tranches, lecture (meilleure source du jour)
      std::vector<GaugeStation> st;
      CHECK(refmar::ParseStations("[{\"shom_id\":\"152\",\"name\":\"LE_CONQUET\",\"longitude\":-4.78075,\"latitude\":48.359098,"
                                  "\"state\":\"OK\",\"reseau\":\"RONIM\"},{\"shom_id\":\"3\",\"name\":\"BREST\",\"longitude\":-4.495,"
                                  "\"latitude\":48.383},{\"shom_id\":\"x/../1\",\"name\":\"BAD\",\"longitude\":0,\"latitude\":0}]",
                                  st, err));
      CHECK(st.size() == 2 && st[0].name == "BREST" && st[1].label() == "Le Conquet" && st[1].network == "RONIM");
      CHECK(st[1].provider == "refmar" && st[1].folder() == "152_LE_CONQUET");
      const double d = gauge::DistanceKm(st[0].lon, st[0].lat, st[1].lon, st[1].lat);
      CHECK(d > 20 && d < 22);
      double km = 0;
      CHECK(gauge::Nearest(st, -4.77, 48.36, &km) == 1 && km < 2);
      const GaugeProvider* prov = FindGaugeProvider("refmar");
      CHECK(prov && prov == GaugeProviders().front() && prov->datum() == "ZH" && prov->maxChunkDays() == 31);
      long long t0, t1;
      CHECK(ParseDateTime("2026-01-01", "", 1, t0) && ParseDateTime("2026-03-15", "", 1, t1));
      const auto ch = prov->chunks("152", t0, t1, 99);
      CHECK(ch.size() == 3 && ch[0].file == "152_2026-01-01_31d.txt" && ch[1].file == "152_2026-02-01_31d.txt");
      CHECK(ch[2].file == "152_2026-03-04_11d.txt");
      const auto days = prov->chunks("152", t0 + 3600, t0 + 2 * 86400 - 60, 1);
      CHECK(days.size() == 2 && days[1].file == "152_2026-01-02_1d.txt" &&
            days[1].url.find("dtStart=2026-01-02T00:00:00Z&dtEnd=2026-01-02T23:59:59Z") != std::string::npos);
      long long c0 = 0;
      int nd = 0;
      CHECK(gauge::ParseChunkFile("152_2026-01-02_1d.txt", c0, nd) && c0 == t0 + 86400 && nd == 1);
      CHECK(gauge::ParseChunkFile("3_2026-02-01_31d.txt", c0, nd) && nd == 31);
      CHECK(!gauge::ParseChunkFile("152_2026-01-02.txt", c0, nd) && !gauge::ParseChunkFile("stations.json", c0, nd));
      CHECK(ch[0].url.find("txt/152?sources=1,2,3,4&dtStart=2026-01-01T00:00:00Z&dtEnd=2026-01-31T23:59:59Z") != std::string::npos);
      CHECK(ch.back().t1 == t1);
      const std::string txt =
          "# Station : LE_CONQUET\n# Fuseau horaire : UTC\n# Référence verticale : zero_hydrographique\n# Unité : m\n"
          "# Date;Valeur;Source\n"
          "01/01/2025 00:00:00;2.274;4\n01/01/2025 00:00:00;2.285;3\n01/01/2025 00:00:00;2.280;2\n"
          "01/01/2025 00:00:00;2.2801;1\n01/01/2025 00:01:00;2.2700;1\n01/01/2025 00:10:00;2.100;3\n"
          "01/01/2025 00:10:00;2.101;2\n01/01/2025 06:12:00;6.5;6\n"
          "02/01/2025 00:00:00;3.0;1\n02/01/2025 00:01:00;3.1;1\nligne illisible\n";
      CHECK(prov->recognises(txt) && refmar::IsRefmarText(txt) && refmar::HasData(txt) && !refmar::HasData("# Station : X\n# Date;Valeur;Source\n"));
      CHECK(refmar::HeaderValue(txt, "Référence verticale") == "zero_hydrographique");
      std::vector<long long> T;
      std::vector<double> H;
      int sk = 0;
      CHECK(refmar::Parse(txt, T, H, sk, err));
      // 1er janvier : source 3 seule (2 valeurs) ; 2 janvier : source 1 (2 valeurs)
      CHECK(T.size() == 4 && std::fabs(H[0] - 2.285) < 1e-9 && std::fabs(H[1] - 2.100) < 1e-9);
      CHECK(T[1] - T[0] == 600 && std::fabs(H[2] - 3.0) < 1e-9 && sk == 1);
      CHECK(FormatTime(T[0]) == "2025-01-01 00:00");
      // série de hauteurs d'eau : dossier de fichiers REFMAR lu directement, en m même si l'unité choisie est le pied
      const std::string dir = Tmp("pd_test_refmar");
      std::filesystem::remove_all(dir);
      std::filesystem::create_directories(dir);
      { std::ofstream o(dir + "/152_2025-01-01_1d.txt", std::ios::binary); o << txt; }
      WaterLevelSeries ws;
      TextFormat tf;
      CHECK(ws.load(dir, tf, 0.3048, err));
      float hh = 0;
      CHECK(ws.size() == 4 && ws.at(T[0] + 300, hh) && std::fabs(hh - 2.1925f) < 1e-4);
      // série du marégraphe (onglet Marée) : tranches du dossier, trous et bords
      GaugeSeries gs;
      CHECK(gs.load(*prov, dir, T[0], T[0] + 2 * 86400) && gs.size() == 4);
      double gh = 0;
      long long tob = 0;
      CHECK(gs.at(T[0] + 300, 1800, 600, gh, &tob) && std::fabs(gh - 2.1925) < 1e-9 && tob == T[0]);
      CHECK(!gs.at(T[1] + 3600, 1800, 600, gh));          // trou de près d'un jour : pas d'interpolation
      CHECK(gs.at(T[3] + 300, 1800, 600, gh) && gh == H[3]);  // après la dernière mesure : gardée 10 min
      CHECK(!gs.at(T[3] + 900, 1800, 600, gh));
      CHECK(!gs.load(*prov, dir, T[0] + 10 * 86400, T[0] + 11 * 86400));  // tranche hors période
      std::filesystem::remove_all(dir);
    }
    {  // NOAA CO-OPS : liste (stations à marée seulement), adresse, réponse CSV, entête enregistré
      const GaugeProvider* np = FindGaugeProvider("noaa");
      CHECK(np && GaugeProviders().size() == 2 && np->datum() == "MLLW");
      std::vector<GaugeStation> st;
      CHECK(np->parseStations("{\"count\":3,\"stations\":[{\"id\":\"8454000\",\"name\":\"Providence\",\"lat\":41.8071,"
                              "\"lng\":-71.4012,\"state\":\"RI\",\"tidal\":true,\"greatlakes\":false},"
                              "{\"id\":\"9063020\",\"name\":\"Buffalo\",\"lat\":42.87,\"lng\":-78.89,\"state\":\"NY\","
                              "\"tidal\":false,\"greatlakes\":true},{\"id\":\"1612340\",\"name\":\"Honolulu\",\"lat\":21.3,"
                              "\"lng\":-157.86,\"state\":\"HI\",\"tidal\":true}]}", st, err));
      CHECK(st.size() == 2 && st[0].name == "Honolulu" && st[1].provider == "noaa" && st[1].network == "RI");
      long long a, b;
      CHECK(ParseDateTime("2026-11-20", "", 1, a));
      b = a + 86400;
      const auto c = np->chunks("8454000", a, b, 1);
      CHECK(c.size() == 1 && c[0].file == "8454000_2026-11-20_1d.txt" &&
            c[0].url.find("begin_date=20261120&end_date=20261120&datum=MLLW&station=8454000&time_zone=gmt&units=metric") !=
                std::string::npos);
      const std::string csv =
          "Date Time, Water Level, Sigma, O or I (for verified), F, R, L, Quality \n"
          "2026-11-20 00:00,0.568,0.005,0,0,0,0,p\n2026-11-20 00:06,0.55,0.006,0,0,0,0,p\n2026-11-20 00:12,,,,,,,\n";
      CHECK(np->hasData(csv) && !np->hasData("\nThere is no MLLW for the station: 9999999\n"));
      CHECK(!np->recognises(csv));  // réponse brute : référence inconnue
      const std::string stored = np->store(csv);
      CHECK(np->recognises(stored) && np->store(stored) == stored && !refmar::Provider().recognises(stored));
      std::vector<long long> T;
      std::vector<double> H;
      int sk = 0;
      CHECK(np->parse(stored, T, H, sk, err) && T.size() == 2 && T[0] == a && T[1] == a + 360 && H[1] == 0.55);
      // import : un fichier NOAA enregistré par le plugin est lu tel quel
      const std::string dir = Tmp("pd_test_noaa");
      std::filesystem::remove_all(dir);
      std::filesystem::create_directories(dir);
      { std::ofstream o(dir + "/" + c[0].file, std::ios::binary); o << stored; }
      WaterLevelSeries ws;
      TextFormat tf;
      float hh = 0;
      CHECK(ws.load(dir, tf, 1.0, err) && ws.size() == 2 && ws.at(a + 180, hh) && std::fabs(hh - 0.559f) < 1e-4);
      GaugeSeries gs;
      CHECK(gs.load(*np, dir, a, b) && gs.size() == 2);
      std::filesystem::remove_all(dir);
    }
    {  // catalogue : empreintes des fichiers installés
      Catalog c;
      CHECK(catalog::Parse(catalog::BuiltinJson(), c, err) && c.version >= 2);
      const CatalogItem* zh = c.find("bathyelli-zh");
      CHECK(zh && zh->files.size() == 2 && zh->files[0].first == "BathyElliv2.1_ATL_ZH.glhi");
      const std::string dir = Tmp("pd_test_verify");
      std::filesystem::remove_all(dir);
      std::filesystem::create_directories(dir + "/sub");
      CatalogItem it;
      it.files = {{"a.txt", Sha256::OfString("abc")}, {"b.txt", Sha256::OfString("x")}, {"c.txt", Sha256::OfString("y")}};
      { std::ofstream o(dir + "/sub/a.txt", std::ios::binary); o << "abc"; }
      { std::ofstream o(dir + "/b.txt", std::ios::binary); o << "changed"; }
      auto v = catalog::Verify(it, dir);
      CHECK(v.size() == 3 && v[0].status == catalog::FileCheck::Ok && v[1].status == catalog::FileCheck::Different &&
            v[2].status == catalog::FileCheck::Missing);
      CatalogItem one;  // fichier téléchargé tel quel : son empreinte de téléchargement
      one.check = "a.txt";
      one.sha256 = Sha256::OfString("abc");
      v = catalog::Verify(one, dir);
      CHECK(v.size() == 1 && v[0].status == catalog::FileCheck::Ok);
      one.extract = true;  // archive : pas d'empreinte du contenu
      v = catalog::Verify(one, dir);
      CHECK(v.size() == 1 && v[0].status == catalog::FileCheck::NoFingerprint);
      std::filesystem::remove_all(dir);
    }
    std::printf("Données externes (JSON, SHA-256, .7z / .zip, catalogue, REFMAR, NOAA, vérification) : OK\n");
  }
  std::printf("\nTOUS LES TESTS PASSENT\n");
  return 0;
}

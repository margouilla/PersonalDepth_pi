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

#include "LandMask.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <filesystem>

#include "FileUtil.h"
#include "Lang.h"
#include "WebMerc.h"

namespace pd {
namespace fs = std::filesystem;

namespace {
bool Seek(FILE* f, long long pos) {
#ifdef _WIN32
  return _fseeki64(f, pos, SEEK_SET) == 0;
#else
  return fseeko(f, (off_t)pos, SEEK_SET) == 0;
#endif
}
uint32_t BE32(const uint8_t* p) { return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; }
template <class T> T LE(const uint8_t* p) {
  T v;
  std::memcpy(&v, p, sizeof v);  // petit-boutiste, comme x86
  return v;
}
std::string Lower(std::string s) {
  for (char& c : s) c = (char)std::tolower((unsigned char)c);
  return s;
}
}  // namespace

LandMask::~LandMask() {
  if (f_) std::fclose(f_);
}

bool LandMask::Find(const std::string& path, std::string& file, std::string& name) {
  file.clear();
  name.clear();
  if (path.empty()) return false;
  std::error_code ec;
  fs::path p = fs::u8path(path);
  if (fs::is_directory(p, ec)) {  // meilleure qualité présente (OSM d'abord)
    static const char* names[] = {"basemap_full.shp", "basemap_high.shp", "basemap_medium.shp",
                                  "basemap_low.shp",  "basemap_crude_10x10.shp", "poly-f-1.dat",
                                  "poly-h-1.dat",     "poly-i-1.dat",     "poly-l-1.dat",
                                  "poly-c-1.dat"};
    fs::path found;
    for (const char* n : names) {
      for (const fs::path& d : {p, p / "OSMSHP", p / "GSHHG"})
        if (fs::exists(d / n, ec)) { found = d / n; break; }
      if (!found.empty()) break;
    }
    if (found.empty()) return false;
    p = found;
  } else if (!fs::exists(p, ec)) {
    return false;
  }
  file = p.u8string();
  const std::string ext = Lower(p.extension().u8string()), st = p.stem().u8string();
  if (ext == ".shp") name = "OSM " + st;
  else if (ext == ".dat") name = "GSHHG " + (st.size() > 5 ? st.substr(5, 1) : st);
  else return false;
  return true;
}

bool LandMask::open(const std::string& path, std::string& err) {
  if (f_) { std::fclose(f_); f_ = nullptr; }
  kind_ = None;
  shpIndex_.clear(); shxOff_.clear(); cellPolys_.clear(); recPoly_.clear(); polys_.clear();
  std::string nm;
  if (!Find(path, file_, nm)) {
    err = pd::tr("Trait de côte introuvable (basemap_*.shp ou poly-*-1.dat) dans ") + path;
    return false;
  }
  const fs::path p = fs::u8path(file_);
  const std::string ext = Lower(p.extension().u8string());
  if (ext == ".shp") {
    // index : emprise de chaque enregistrement (.dbf : xmin xmax ymin ymax) et
    // position (.shx), par cellule de 1°
    fs::path dbf = p, shx = p;
    dbf.replace_extension(".dbf");
    shx.replace_extension(".shx");
    FILE* fs_ = OpenFile(shx.u8string(), "rb");
    if (!fs_) { err = pd::tr("Impossible d'ouvrir ") + shx.u8string(); return false; }
    std::fseek(fs_, 0, SEEK_END);
    const long sz = std::ftell(fs_);
    std::vector<uint8_t> b(sz > 100 ? (size_t)sz : 0);
    std::fseek(fs_, 0, SEEK_SET);
    const bool rd = !b.empty() && std::fread(b.data(), 1, b.size(), fs_) == b.size();
    std::fclose(fs_);
    if (!rd || LE<int32_t>(b.data() + 32) != 5) {
      err = pd::tr("Shapefile de polygones attendu : ") + file_;
      return false;
    }
    const size_t n = (b.size() - 100) / 8;
    shxOff_.resize(n);
    for (size_t k = 0; k < n; ++k) shxOff_[k] = BE32(&b[100 + 8 * k]);  // mots de 16 bits
    FILE* fd = OpenFile(dbf.u8string(), "rb");
    if (!fd) { err = pd::tr("Impossible d'ouvrir ") + dbf.u8string(); return false; }
    uint8_t h[32];
    if (std::fread(h, 1, 32, fd) != 32) { std::fclose(fd); err = pd::tr("Fichier .dbf illisible"); return false; }
    const uint32_t nrec = LE<uint32_t>(h + 4);
    const uint16_t hlen = LE<uint16_t>(h + 8), rlen = LE<uint16_t>(h + 10);
    std::vector<uint8_t> hdr(hlen);
    std::memcpy(hdr.data(), h, 32);
    if (hlen < 33 || std::fread(hdr.data() + 32, 1, hlen - 32, fd) != (size_t)hlen - 32) {
      std::fclose(fd); err = pd::tr("Fichier .dbf illisible"); return false;
    }
    int off[4] = {-1, -1, -1, -1}, len[4] = {0, 0, 0, 0};
    static const char* fn[4] = {"xmin", "xmax", "ymin", "ymax"};
    int pos = 1;  // octet d'effacement
    for (size_t q = 32; q + 32 <= hlen && hdr[q] != 0x0D; q += 32) {
      const std::string name = Lower(std::string((const char*)&hdr[q], strnlen((const char*)&hdr[q], 11)));
      for (int k = 0; k < 4; ++k)
        if (name == fn[k]) { off[k] = pos; len[k] = hdr[q + 16]; }
      pos += hdr[q + 16];
    }
    if (off[0] < 0 || off[1] < 0 || off[2] < 0 || off[3] < 0 || nrec != n) {
      std::fclose(fd);
      err = pd::tr("Shapefile sans emprise des polygones (xmin, xmax, ymin, ymax) : ") + file_;
      return false;
    }
    std::vector<uint8_t> blk((size_t)rlen * 4096);
    char tmp[64];
    for (uint32_t r0 = 0; r0 < nrec; r0 += 4096) {
      const uint32_t m = std::min<uint32_t>(4096, nrec - r0);
      if (std::fread(blk.data(), rlen, m, fd) != m) { std::fclose(fd); err = pd::tr("Fichier .dbf illisible"); return false; }
      for (uint32_t q = 0; q < m; ++q) {
        double v[4];
        for (int k = 0; k < 4; ++k) {
          const int L = std::min(len[k], 63);
          std::memcpy(tmp, &blk[(size_t)q * rlen + off[k]], L);
          tmp[L] = 0;
          v[k] = std::atof(tmp);
        }
        const int cx0 = (int)std::floor(v[0]), cx1 = (int)std::floor(v[1] - 1e-9);
        const int cy0 = (int)std::floor(v[2]), cy1 = (int)std::floor(v[3] - 1e-9);
        for (int cy = cy0; cy <= std::max(cy0, cy1); ++cy)
          for (int cx = cx0; cx <= std::max(cx0, cx1); ++cx) shpIndex_[{cx, cy}].push_back(r0 + q);
      }
    }
    std::fclose(fd);
    f_ = OpenFile(file_, "rb");
    if (!f_) { err = pd::tr("Impossible d'ouvrir ") + file_; return false; }
    kind_ = Shp;
    name_ = nm;
  } else {
    f_ = OpenFile(file_, "rb");
    if (!f_) { err = pd::tr("Impossible d'ouvrir ") + file_; return false; }
    int32_t h[12];
    if (std::fread(h, 4, 12, f_) != 12 || h[1] <= 0 || h[2] <= 0 || h[3] != 0 || h[4] != -90) {
      std::fclose(f_); f_ = nullptr;
      err = pd::tr("Fichier GSHHG d'OpenCPN invalide : ") + file_;
      return false;
    }
    pasx_ = h[1]; pasy_ = h[2];
    kind_ = Gshhg;
    name_ = nm;
  }
  return true;
}

bool LandMask::readShpRecord(uint32_t rec, Poly& p, std::string& err) {
  if (rec >= shxOff_.size() || !Seek(f_, 2LL * shxOff_[rec])) { err = pd::tr("Shapefile illisible"); return false; }
  uint8_t h[52];
  if (std::fread(h, 1, 52, f_) != 52) { err = pd::tr("Shapefile illisible"); return false; }
  const int32_t type = LE<int32_t>(h + 8);
  if (type == 0) return true;  // forme vide
  if (type != 5) { err = pd::tr("Shapefile de polygones attendu : ") + file_; return false; }
  p.x0 = LE<double>(h + 12); p.y0 = LE<double>(h + 20);
  p.x1 = LE<double>(h + 28); p.y1 = LE<double>(h + 36);
  const int32_t np = LE<int32_t>(h + 44), npt = LE<int32_t>(h + 48);
  if (np <= 0 || npt <= 0 || np > 10000000 || npt > 50000000) { err = pd::tr("Shapefile illisible"); return false; }
  std::vector<int32_t> parts(np);
  std::vector<double> pts((size_t)npt * 2);
  if (std::fread(parts.data(), 4, np, f_) != (size_t)np || std::fread(pts.data(), 16, npt, f_) != (size_t)npt) {
    err = pd::tr("Shapefile illisible");
    return false;
  }
  for (int32_t k = 0; k < np; ++k) {
    const int32_t a = parts[k], b = k + 1 < np ? parts[k + 1] : npt;
    if (a < 0 || b > npt || b - a < 3) continue;
    Ring r;
    r.x.reserve(b - a); r.y.reserve(b - a);
    for (int32_t q = a; q < b; ++q) { r.x.push_back(pts[2 * q]); r.y.push_back(pts[2 * q + 1]); }
    p.rings.push_back(std::move(r));
  }
  return true;
}

bool LandMask::loadCell(int cx, int cy, std::string& err) {
  const std::pair<int, int> key(cx, cy);
  if (cellPolys_.count(key)) return true;
  std::vector<int>& list = cellPolys_[key];
  if (kind_ == Shp) {
    auto it = shpIndex_.find(key);
    if (it == shpIndex_.end()) return true;
    for (uint32_t rec : it->second) {
      auto r = recPoly_.find(rec);
      if (r != recPoly_.end()) { list.push_back(r->second); continue; }  // polygone sur plusieurs cellules
      Poly p;
      if (!readShpRecord(rec, p, err)) { cellPolys_.erase(key); return false; }
      if (p.rings.empty()) continue;
      polys_.push_back(std::move(p));
      recPoly_[rec] = (int)polys_.size() - 1;
      list.push_back((int)polys_.size() - 1);
    }
    return true;
  }
  if (kind_ == Gshhg) {
    if (cy < -90 || cy >= 90) return true;
    const int x0 = ((cx % 360) + 360) % 360;  // longitudes 0..360 dans le fichier
    const long tab = (long)(x0 / pasx_) * (180 / pasy_) + (cy + 90) / pasy_;
    int32_t pos = 0;
    if (!Seek(f_, 48 + 4LL * tab) || std::fread(&pos, 4, 1, f_) != 1 || !Seek(f_, pos)) {
      err = pd::tr("Fichier GSHHG illisible");
      cellPolys_.erase(key);
      return false;
    }
    const double shift = x0 >= 180 ? -360.0 : 0.0;  // longitudes -180..180
    for (int lev = 0; lev < 5; ++lev) {  // terre, lac, île, étang, Antarctique
      int32_t nc = 0;
      if (std::fread(&nc, 4, 1, f_) != 1 || nc < 0 || nc > 1000000) { err = pd::tr("Fichier GSHHG illisible"); return false; }
      Poly p;
      p.water = lev == 1 || lev == 3;
      p.x0 = p.y0 = 1e9; p.x1 = p.y1 = -1e9;
      for (int32_t c = 0; c < nc; ++c) {
        int32_t hv[2];
        if (std::fread(hv, 4, 2, f_) != 2 || hv[1] < 0 || hv[1] > 50000000) { err = pd::tr("Fichier GSHHG illisible"); return false; }
        std::vector<double> xy((size_t)hv[1] * 2);
        if (hv[1] && std::fread(xy.data(), 16, hv[1], f_) != (size_t)hv[1]) { err = pd::tr("Fichier GSHHG illisible"); return false; }
        if (hv[1] < 3) continue;
        Ring r;
        for (int32_t q = 0; q < hv[1]; ++q) {
          const double lo = xy[2 * q] * 1e-6 + shift, la = xy[2 * q + 1] * 1e-6;
          r.x.push_back(lo); r.y.push_back(la);
          p.x0 = std::min(p.x0, lo); p.x1 = std::max(p.x1, lo);
          p.y0 = std::min(p.y0, la); p.y1 = std::max(p.y1, la);
        }
        p.rings.push_back(std::move(r));
      }
      if (p.rings.empty()) continue;
      polys_.push_back(std::move(p));
      list.push_back((int)polys_.size() - 1);
    }
  }
  return true;
}

bool LandMask::tile(int z, int x, int y, std::vector<uint8_t>& land, std::string& err) {
  land.assign(256 * 256, 0);
  if (!isOpen()) return true;
  double lonW, lonE, latS, latN;
  TileBounds(z, x, y, lonW, lonE, latS, latN);
  // polygones des cellules de 1° touchées, dans l'ordre (GSHHG : niveaux)
  std::vector<int> ids;
  for (int cy = (int)std::floor(latS); cy <= (int)std::floor(latN); ++cy)
    for (int cx = (int)std::floor(lonW); cx <= (int)std::floor(lonE - 1e-12); ++cx) {
      if (!loadCell(cx, cy, err)) return false;
      for (int id : cellPolys_[{cx, cy}])
        if (std::find(ids.begin(), ids.end(), id) == ids.end()) ids.push_back(id);
    }
  if (ids.empty()) return true;
  double rowLat[256];
  for (int j = 0; j < 256; ++j) {
    double lo;
    PixelToLonLat(z, x * 256.0 + 0.5, y * 256.0 + j + 0.5, lo, rowLat[j]);
  }
  const double dl = (lonE - lonW) / 256.0;
  std::vector<uint8_t> in(256 * 256);
  struct E { double x0, y0, x1, y1; };
  std::vector<E> edges;
  std::vector<double> xs;
  for (int id : ids) {
    const Poly& p = polys_[id];
    if (p.x1 < lonW || p.x0 > lonE || p.y1 < latS || p.y0 > latN) continue;
    // arêtes qui traversent la bande de latitudes de la tuile
    edges.clear();
    for (const Ring& r : p.rings) {
      const size_t n = r.x.size();
      for (size_t a = 0, b = n - 1; a < n; b = a++) {
        const double ya = r.y[a], yb = r.y[b];
        if (std::max(ya, yb) < latS || std::min(ya, yb) > latN) continue;
        edges.push_back({r.x[b], yb, r.x[a], ya});
      }
    }
    if (edges.empty()) continue;
    std::fill(in.begin(), in.end(), 0);
    bool any = false;
    for (int j = 0; j < 256; ++j) {
      const double la = rowLat[j];
      xs.clear();
      for (const E& e : edges)
        if ((e.y0 <= la) != (e.y1 <= la)) xs.push_back(e.x0 + (la - e.y0) * (e.x1 - e.x0) / (e.y1 - e.y0));
      if (xs.size() < 2) continue;
      std::sort(xs.begin(), xs.end());
      // pair-impair : pixel dedans si un nombre impair d'intersections à sa gauche
      size_t q = 0;
      for (int i = 0; i < 256; ++i) {
        const double lo = lonW + (i + 0.5) * dl;
        while (q < xs.size() && xs[q] < lo) ++q;
        if (q & 1) { in[j * 256 + i] = 1; any = true; }
      }
    }
    if (!any) continue;
    for (int k = 0; k < 256 * 256; ++k)
      if (in[k]) land[k] = p.water ? 0 : 1;
  }
  return true;
}

bool LandMask::landAt(double lon, double lat) {
  double gx, gy;
  const int z = 20;
  LonLatToPixel(z, lon, lat, gx, gy);
  const int x = (int)std::floor(gx / 256), y = (int)std::floor(gy / 256);
  std::vector<uint8_t> m;
  std::string e;
  if (!tile(z, x, y, m, e)) return false;
  const int i = (int)std::floor(gx) - x * 256, j = (int)std::floor(gy) - y * 256;
  return m[j * 256 + i] != 0;
}

}  // namespace pd

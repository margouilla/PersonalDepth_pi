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

#include "Imagery.h"

#include <algorithm>
#include <cstdio>
#include <map>

#include "GeoTiff.h"
#include "Lang.h"
#include "PointCloud.h"
#include "Projection.h"

namespace pd {

namespace {

bool IsNoData(double v, const std::vector<double>& nd) {
  for (double x : nd)
    if (std::fabs(v - x) <= 1e-6 * std::max(1.0, std::fabs(x))) return true;
  return false;
}

}  // namespace

bool AnalyzeImagery(const std::string& path, const ImageryOptions& o, ImageryInfo& info, std::string& err,
                    const std::atomic<bool>* cancel) {
  info = ImageryInfo();
  GeoTiff t;
  if (!t.open(path, err)) return false;
  const GeoTiffInfo& G = t.info();
  if (!G.georef) { err = pd::tr("TIFF sans géoréférencement (ni GeoTIFF ni fichier .tfw)"); return false; }
  std::shared_ptr<const Crs> crs;
  if (!GeoTiffCrs(G, o.crs, crs, info.crsUsed, err)) return false;
  info.width = G.width;
  info.height = G.height;
  // latitude moyenne et pas en mètres
  {
    const double yc = G.y0 - 0.5 * G.height * G.dy;
    double lon = 0, lat = yc;
    if (crs && !crs->toWGS84(G.x0 + 0.5 * G.width * G.dx, yc, lon, lat)) lat = 0;
    info.midLat = lat;
  }
  if (!crs || crs->isGeographic()) {
    info.stepX = G.dx * 111320.0 * std::cos(info.midLat * M_PI / 180.0);
    info.stepY = G.dy * 110574.0;
  } else {
    info.stepX = G.dx * crs->unitToMeter();
    info.stepY = G.dy * crs->unitToMeter();
  }
  info.resM = std::max(std::min(info.stepX, info.stepY), 0.5 * std::max(info.stepX, info.stepY));
  // lignes échantillonnées (au plus 400) et bords de l'image
  const int nRows = std::min(G.height, 400);
  const int stride = std::max(1, (int)((double)G.width * nRows / 1500000.0));
  std::vector<float> row(G.width), vals, border;
  vals.reserve((size_t)G.width / stride * nRows);
  for (int k = 0; k < nRows; ++k) {
    if (cancel && *cancel) { err = "Annulé"; return false; }
    const int r = nRows == 1 ? 0 : (int)std::lround((double)k * (G.height - 1) / (nRows - 1));
    if (!t.readRow(r, row.data(), err)) return false;
    for (int c = 0; c < G.width; c += stride)
      if (!std::isnan(row[c]) && !IsNoData(row[c], o.nodata)) vals.push_back(row[c]);
    if (r == 0 || r == G.height - 1) {
      for (int c = 0; c < G.width; c += std::max(1, G.width / 2000)) border.push_back(row[c]);
    } else {
      border.push_back(row[0]);
      border.push_back(row[G.width - 1]);
    }
  }
  if (vals.empty()) { err = pd::tr("Image vide"); return false; }
  // fond : valeur extrême (plus petite ou plus grande) présente sur au moins
  // 25 % du bord (zone hors levé des mosaïques, souvent blanche ou noire)
  const auto mm = std::minmax_element(vals.begin(), vals.end());
  if (o.noBackground) {
    info.hasBackground = false;
  } else if (!std::isnan(o.background)) {
    info.hasBackground = true;
    info.background = o.background;
  } else {
    size_t nb = 0, nLo = 0, nHi = 0;
    for (float b : border) {
      if (std::isnan(b)) continue;
      ++nb;
      nLo += b == *mm.first;
      nHi += b == *mm.second;
    }
    if (nb && std::max(nLo, nHi) * 4 >= nb) {
      info.hasBackground = true;
      info.backgroundAuto = true;
      info.background = nHi >= nLo ? *mm.second : *mm.first;
    }
  }
  if (info.hasBackground)
    vals.erase(std::remove(vals.begin(), vals.end(), (float)info.background), vals.end());
  if (vals.empty()) { err = pd::tr("Image vide"); return false; }
  // étirement : 2 % et 98 % des valeurs
  std::sort(vals.begin(), vals.end());
  info.lo = vals[(size_t)(0.02 * (vals.size() - 1))];
  info.hi = vals[(size_t)(0.98 * (vals.size() - 1))];
  if (info.hi <= info.lo) { info.lo = vals.front(); info.hi = vals.back(); }
  if (info.hi <= info.lo) info.hi = info.lo + 1;
  // unité
  if (!G.palette.empty() || (G.bits == 8 && G.sampleFormat != 3)) info.unit = pd::tr("niveau");
  else if (G.sampleFormat == 3 && vals.front() >= -120 && vals.back() <= 30 && info.lo < 0) info.unit = "dB";
  return true;
}

bool ImportImagery(BathyDb& db, const std::string& path, const ImageryOptions& o, DbSource meta,
                   DbPriority prio, std::string& err, const DbProgress& progress,
                   const std::atomic<bool>* cancel, int64_t* newId, ImageryInfo* infoOut) {
  if (db.kind() != DbKind::Imagery) { err = "ImportImagery : base de bathymétrie"; return false; }
  ImageryInfo info;
  if (progress) progress(0.f, pd::tr("Analyse de l'image"));
  if (!AnalyzeImagery(path, o, info, err, cancel)) return false;
  GeoTiff t;
  if (!t.open(path, err)) return false;
  std::shared_ptr<const Crs> crs;
  std::string used;
  if (!GeoTiffCrs(t.info(), o.crs, crs, used, err)) return false;
  meta.res = info.resM;
  meta.gridStepX = info.stepX;
  meta.gridStepY = info.stepY;
  meta.vunit = info.unit;
  meta.vlo = info.lo;
  meta.vhi = info.hi;
  meta.crs = info.crsUsed;
  meta.datum = (int)Datum::Free;
  {
    char b[256];
    std::snprintf(b, sizeof b, pd::tr("image %d × %d, pixel %.2f × %.2f m, affichage %g – %g %s"), info.width,
                  info.height, info.stepX, info.stepY, info.lo, info.hi, info.unit.c_str());
    meta.info = b;
    if (info.hasBackground) {
      std::snprintf(b, sizeof b, pd::tr(" ; fond %g écarté"), info.background);
      meta.info += b;
    }
  }
  const int W = t.info().width, H = t.info().height;
  const size_t kBlock = (size_t)1 << 21;  // ~2 M pixels par bloc : mémoire bornée
  long long pixels = 0;
  BathyDb::CloudFeed feed = [&](const BathyDb::CloudSink& sink, std::string& ferr) {
    PointCloud block;
    block.reserve(kBlock + W);
    bool ok = true;
    if (!ForEachGeoTiffRow(t, crs, [&](int r, const float* v, const double* lon, const double* lat) {
          for (int c = 0; c < W; ++c) {
            const float x = v[c];
            if (std::isnan(x) || std::isnan(lon[c]) || IsNoData(x, o.nodata)) continue;
            if (info.hasBackground && x == (float)info.background) continue;
            block.add(lon[c], lat[c], x);
          }
          if (block.size() >= kBlock || r == H - 1) {
            if (!block.empty()) {
              block.computeBounds();
              pixels += (long long)block.size();
              if (!sink(block, (float)(r + 1) / H)) { ok = false; return false; }
            }
            block.clear();
          }
          return true;
        }, ferr, cancel))
      return false;
    return ok;
  };
  if (!db.importCloudStream(feed, info.midLat, meta, true, prio, err, progress, cancel, newId)) return false;
  info.pixels = pixels;
  if (infoOut) *infoOut = info;
  return true;
}

}  // namespace pd

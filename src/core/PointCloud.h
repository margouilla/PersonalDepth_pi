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

// PersonalDepth_pi — cœur de données bathymétriques (sans dépendance wxWidgets)
// Nuage de sondes quantifié : 12 octets/point (lon/lat int32 au 1e-7°, z float32)
#pragma once
#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace pd {

class Crs;
using ProgressFn = std::function<void(float /*0..1*/)>;

struct LoadOptions {
  enum class Order { LonLatZ, LatLonZ };
  enum class ZSign { Auto, DepthPositive, Elevation };
  Order order = Order::LonLatZ;
  ZSign zsign = ZSign::Auto;  // Auto : si >60 % des z sont négatifs -> altitudes
  float zOffset = 0.f;        // décalage vertical éventuel (ex. passage ZH/IGN)
  // Valeurs NODATA supplémentaires (Z brut du fichier, tous formats), en plus
  // de celle de l'entête : 9999, -9999, 99999… Les valeurs non finies ou de
  // module >= 1e20 sont toujours ignorées.
  std::vector<double> nodata;
  // Système de coordonnées du fichier (nullptr = WGS84 lon/lat). Les deux
  // premières colonnes sont alors Est/Nord (ou Nord/Est avec LatLonZ).
  std::shared_ptr<const Crs> crs;
};

// Pas régulier d'une suite de coordonnées entières (en unités), 0 sinon.
// Tolère des lignes manquantes et ±2 unités d'arrondi.
double LatticeStepUnits(const std::vector<int64_t>& q);

class PointCloud {
 public:
  static constexpr double kQ = 1e-7;  // ~1,1 cm à l'équateur

  std::vector<int32_t> qlon, qlat;
  std::vector<float> z;  // profondeur, positive vers le bas (m)

  double lonMin = 0, lonMax = 0, latMin = 0, latMax = 0;
  float zMin = 0, zMax = 0;
  // Pas de grille détecté dans les coordonnées projetées d'origine (m), 0 sinon
  double srcStepX = 0, srcStepY = 0;

  size_t size() const { return z.size(); }
  bool empty() const { return z.empty(); }
  double lon(size_t i) const { return qlon[i] * kQ; }
  double lat(size_t i) const { return qlat[i] * kQ; }

  void reserve(size_t n);
  void add(double lon, double lat, float depth);
  void clear();
  void computeBounds();
  // NODATA (utilisateur), conventions de signe + offset ; `tris` : indices de
  // triangles renumérotés si des points sont retirés
  void applySign(const LoadOptions& o, std::vector<uint32_t>* tris = nullptr);
  // Garde les points vérifiant keep(z) ; triangles renumérotés (ceux qui
  // touchent un point retiré disparaissent). Renvoie le nombre retiré.
  template <class F>
  size_t filter(F keep, std::vector<uint32_t>* tris = nullptr);
  // Bornes de profondeur (m, positives vers le bas)
  size_t keepDepths(float zMin, float zMax, std::vector<uint32_t>* tris = nullptr) {
    return filter([&](float v) { return v >= zMin && v <= zMax; }, tris);
  }
  size_t memoryBytes() const { return size() * 12; }

  // Cache binaire (relecture quasi instantanée des gros fichiers)
  bool saveCache(const std::string& path) const;
  bool loadCache(const std::string& path);
};

template <class F>
size_t PointCloud::filter(F keep, std::vector<uint32_t>* tris) {
  const size_t n = size();
  std::vector<uint32_t> remap;
  if (tris) remap.assign(n, UINT32_MAX);
  size_t w = 0;
  for (size_t i = 0; i < n; ++i) {
    if (!keep(z[i])) continue;
    if (tris) remap[i] = (uint32_t)w;
    if (w != i) { qlon[w] = qlon[i]; qlat[w] = qlat[i]; z[w] = z[i]; }
    ++w;
  }
  if (w == n) return 0;
  qlon.resize(w); qlat.resize(w); z.resize(w);
  if (tris) {
    size_t o = 0;
    for (size_t t = 0; t + 2 < tris->size(); t += 3) {
      const uint32_t a = remap[(*tris)[t]], b = remap[(*tris)[t + 1]], c = remap[(*tris)[t + 2]];
      if (a == UINT32_MAX || b == UINT32_MAX || c == UINT32_MAX) continue;
      (*tris)[o++] = a; (*tris)[o++] = b; (*tris)[o++] = c;
    }
    tris->resize(o);
  }
  computeBounds();
  return n - w;
}

// Lecteurs. Les sondes conservent leur valeur numérique d'origine (float32).
bool LoadXYZ(const std::string& path, const LoadOptions& opt, PointCloud& pc,
             std::string& err, const ProgressFn& progress = nullptr,
             const std::atomic<bool>* cancel = nullptr);

// Grille ESRI ASCII (.asc) : chaque cellule valide devient une sonde au
// centre de cellule. Renvoie le pas natif (degrés, ou unités du système).
bool LoadESRIAscii(const std::string& path, const LoadOptions& opt,
                   PointCloud& pc, double& cellLon, double& cellLat,
                   std::string& err,
                   const std::atomic<bool>* cancel = nullptr);

// TIN LandXML minimal : <P id="n">lat lon z</P> et <F>a b c</F>
bool LoadLandXMLTin(const std::string& path, const LoadOptions& opt,
                    PointCloud& pc, std::vector<uint32_t>& triangles,
                    std::string& err);

#ifdef PD_HAVE_GDAL
// Raster GDAL (GeoTIFF, BAG, NetCDF…) en EPSG:4326, bande 1
bool LoadGdalRaster(const std::string& path, const LoadOptions& opt,
                    PointCloud& pc, double& cellLon, double& cellLat,
                    std::string& err);
#endif

}  // namespace pd

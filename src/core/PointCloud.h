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

// Format personnalisé d'un fichier texte : lignes d'entête, séparateurs et
// rôle des colonnes (numérotées à partir de 1, 0 = absente)
struct TextFormat {
  bool custom = false;   // false : trois premiers nombres de la ligne (X Y Z / Y X Z)
  int skipLines = 0;     // lignes d'entête ignorées
  char sep = 0;          // 0 automatique, ' ' espaces/tabulations, ',', ';', '\t'
  char decimal = '.';    // '.' ou ','
  int colX = 1, colY = 2, colZ = 3;
  int colDate = 0;       // date, ou date et heure (2021-06-26T19:41:24Z)
  int colTime = 0;       // heure seule (08:10:23.000)
  int dateOrder = 0;     // 0 automatique, 1 A-M-J, 2 J/M/A, 3 M/J/A
  // Gabarits facultatifs ("" = reconnaissance automatique / nombre décimal) :
  // date et heure : y M d h m s z AP MMM (« yyyy/M/d h:m:s.s ») ;
  // coordonnées X et Y (chacune) : D M S X (« D M S.S X », « DDMM.MX »)
  std::string dateFmt, timeFmt, xFmt, yFmt;
  // Fuseau horaire des dates du fichier : décalage sur UTC en minutes (heure
  // fixe, sans changement d'heure) ; les instants lus sont ramenés en UTC
  int tzMinutes = 0;
  std::string serialize() const;            // "" si non personnalisé
  bool deserialize(const std::string& s);   // "" -> non personnalisé
  bool operator==(const TextFormat& o) const { return serialize() == o.serialize(); }
};

// Découpe d'une ligne en champs selon le format (espaces, virgule…)
std::vector<std::string> SplitFields(const std::string& line, const TextFormat& f);
// Nombre d'un champ (décimale '.' ou ',')
bool ParseFieldNumber(const std::string& s, char decimal, double& v);
// Date (et heure) d'un ou deux champs -> secondes UTC depuis 1970 (reconnaissance automatique)
bool ParseDateTime(const std::string& date, const std::string& time, int dateOrder, long long& t);
// Idem avec les gabarits du format (dateFmt / timeFmt), sinon automatique
bool ParseFieldDateTime(const std::string& date, const std::string& time, const TextFormat& f,
                        long long& t);
// Coordonnée X (isY false) ou Y : gabarit xFmt / yFmt (degrés, minutes, secondes, hémisphère), sinon nombre
bool ParseFieldCoord(const std::string& s, const TextFormat& f, bool isY, double& v);
// Gabarits seuls (aide, tests). Date : champs non lus = -1 ; renvoie false si le texte ne suit pas le gabarit
struct DateParts {
  int y = -1, mo = -1, d = -1, h = -1, mi = -1;
  double s = -1;
  int ampm = 0;  // 1 AM, 2 PM
};
bool ParseDatePattern(const std::string& text, const std::string& pattern, DateParts& p);
bool ParseCoordPattern(const std::string& text, const std::string& pattern, double& deg);
// Premières lignes d'un fichier texte (UTF-8, fins de ligne retirées)
bool ReadTextHead(const std::string& path, size_t maxLines, std::vector<std::string>& lines,
                  std::string& err);
// Format deviné d'après les premières lignes (entête, séparateur, colonnes)
TextFormat GuessTextFormat(const std::vector<std::string>& lines);
// Fichier de hauteurs d'eau : date (heure) et valeur (colonne Z) ; X et Y à 0
TextFormat GuessWaterLevelFormat(const std::vector<std::string>& lines);
// Décalages horaires proposés (minutes) et libellé « UTC+05:30 »
const std::vector<int>& TimeZoneOffsets();
std::string TimeZoneLabel(int minutes);

class WaterLevelSeries;

struct LoadOptions {
  enum class Order { LonLatZ, LatLonZ };
  enum class ZSign { Auto, DepthPositive, Elevation };
  Order order = Order::LonLatZ;
  ZSign zsign = ZSign::Auto;  // Auto : si >60 % des z sont négatifs -> altitudes
  float zOffset = 0.f;        // décalage vertical éventuel (m), ajouté après conversion
  // Unité verticale du fichier : facteur vers le mètre (1 = m, 0.3048 = pieds).
  // Les bases et l'affichage interne restent toujours en mètres.
  double zToMeters = 1.0;
  // Valeurs NODATA supplémentaires (Z brut du fichier, tous formats), en plus
  // de celle de l'entête : 9999, -9999, 99999… Les valeurs non finies ou de
  // module >= 1e20 sont toujours ignorées.
  std::vector<double> nodata;
  // Système de coordonnées du fichier (nullptr = WGS84 lon/lat). Les deux
  // premières colonnes sont alors Est/Nord (ou Nord/Est avec LatLonZ).
  std::shared_ptr<const Crs> crs;
  // Fichiers texte : format personnalisé (colonnes, entête, séparateurs)
  TextFormat text;
  // Hauteurs d'eau observées (m, format personnalisé avec date) : chaque
  // sonde est corrigée de la hauteur à son heure.
  // waterSubtract : profondeur ZH = sonde - hauteur (hauteur au-dessus du zéro)
  std::shared_ptr<const WaterLevelSeries> water;
  bool waterSubtract = true;
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
  // Dates des sondes (colonne date/heure du fichier), secondes UTC ; 0 = inconnues
  long long tMin = 0, tMax = 0;
  // Dernière lecture avec hauteurs d'eau (non gardé dans le cache) : sondes
  // écartées (sans date, hors de la série) et plage des corrections (unité de Z)
  long long waterNoTime = 0, waterMiss = 0;
  float waterCorrMin = 0, waterCorrMax = 0;
  bool waterApplied = false;

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
// Lecture par blocs (très gros fichiers) : si `sink` est donné, chaque bloc
// d'environ `blockN` sondes converties (signe, unité, hauteurs d'eau) est remis
// à `sink` puis vidé ; `sink` renvoie false pour arrêter la lecture (sans
// erreur). pc.tMin / tMax et les compteurs d'eau sont cumulés au fil des blocs.
using XYZBlockSink = std::function<bool(PointCloud&)>;
bool LoadXYZ(const std::string& path, const LoadOptions& opt, PointCloud& pc,
             std::string& err, const ProgressFn& progress = nullptr,
             const std::atomic<bool>* cancel = nullptr, const XYZBlockSink* sink = nullptr,
             size_t blockN = 0);

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

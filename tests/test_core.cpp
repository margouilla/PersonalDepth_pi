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
#include "../src/core/GeoGrid.h"
#include "../src/core/LandMask.h"
#include <cstring>
#include "../src/core/Projection.h"

using namespace pd;
using clk = std::chrono::steady_clock;
static std::string Tmp(const char* n) {
  return (std::filesystem::temp_directory_path() / n).string();
}
static double ms(clk::time_point a) {
  return std::chrono::duration<double, std::milli>(clk::now() - a).count();
}
#define CHECK(c) do { if (!(c)) { std::printf("ÉCHEC %s:%d %s\n", __FILE__, __LINE__, #c); return 1; } } while (0)

int main(int argc, char** argv) {
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
    std::printf("Masque terre (OSMSHP) : OK\n");
  }
  std::printf("\nTOUS LES TESTS PASSENT\n");
  return 0;
}

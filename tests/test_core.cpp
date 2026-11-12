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
  const size_t N = argc > 1 ? std::strtoul(argv[1], nullptr, 10) : 3000000;
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
  std::printf("\nTOUS LES TESTS PASSENT\n");
  return 0;
}

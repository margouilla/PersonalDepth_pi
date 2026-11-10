// PersonalDepth_pi - personal bathymetry plugin for OpenCPN
// Copyright (C) 2026 Marcaurelio Franzetti <marcaurelio.franzetti@gmail.com>
// SPDX-License-Identifier: GPL-2.0-or-later
//
// pd_probe - diagnostic des jeux de test « grille / nuage / interpolation »
// Charge un fichier comme le plugin (LoadXYZ / LoadESRIAscii), affiche la
// décision actuelle (grille détectée, résolution estimée) et des statistiques
// d'espacement pour concevoir les nouvelles règles.
//   pd_probe <fichier> [crs=EPSG:2154] [fmt=<TextFormat sérialisé>|guess] [z=depth|elev|auto] [zft]
//            [hint=0..5] [force=1|2] [db=<dossier> [view=<fichier>] [zoom=lon0,lon1,lat0,lat1]
//            [stream=<sondes par bloc>]]
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <random>
#include <string>
#include <unordered_map>
#include <vector>

#include "../../src/core/BathyDb.h"
#include "../../src/core/PointCloud.h"
#include "../../src/core/Projection.h"
#include "../../src/core/SurveyShape.h"
#include "../../src/core/Triangulate.h"

using namespace pd;

static double Pct(std::vector<double>& v, double p) {
  if (v.empty()) return 0;
  size_t k = std::min(v.size() - 1, (size_t)(p * (v.size() - 1)));
  std::nth_element(v.begin(), v.begin() + k, v.end());
  return v[k];
}

int main(int argc, char** argv) {
  if (argc < 2) { std::fprintf(stderr, "usage\n"); return 2; }
  std::string path = argv[1];
  LoadOptions o;
  bool guess = false;
  int hint = 0, force = 0;
  std::string dbDir;
  bool onlyImport = false;
  std::string viewOut;
  double forceRes = 0;  // résolution imposée (m)
  int method = -1;      // -1 auto (voisin naturel si lignes), 0 TIN, 1 voisin naturel, 2 IDW
  size_t streamN = 0;  // import par blocs (sondes par bloc)
  double zb[4] = {0, 0, 0, 0};
  for (int i = 2; i < argc; ++i) {
    std::string a = argv[i];
    if (a.rfind("crs=", 0) == 0) {
      auto c = std::make_shared<Crs>();
      std::string e;
      if (!Crs::Parse(a.substr(4), *c, e)) { std::fprintf(stderr, "CRS: %s\n", e.c_str()); return 2; }
      o.crs = c;
    } else if (a == "fmt=guess") guess = true;
    else if (a.rfind("fmt=", 0) == 0) o.text.deserialize(a.substr(4));
    else if (a == "z=depth") o.zsign = LoadOptions::ZSign::DepthPositive;
    else if (a == "z=elev") o.zsign = LoadOptions::ZSign::Elevation;
    else if (a == "zft") o.zToMeters = 0.3048;
    else if (a.rfind("hint=", 0) == 0) hint = std::atoi(a.c_str() + 5);
    else if (a.rfind("force=", 0) == 0) force = std::atoi(a.c_str() + 6);
    else if (a.rfind("db=", 0) == 0) { dbDir = a.substr(3); onlyImport = true; }
    else if (a.rfind("view=", 0) == 0) viewOut = a.substr(5);
    else if (a.rfind("res=", 0) == 0) forceRes = std::atof(a.c_str() + 4);
    else if (a.rfind("method=", 0) == 0) method = std::atoi(a.c_str() + 7);
    else if (a.rfind("stream=", 0) == 0) streamN = (size_t)std::atoll(a.c_str() + 7);
    else if (a.rfind("zoom=", 0) == 0) std::sscanf(a.c_str() + 5, "%lf,%lf,%lf,%lf", &zb[0], &zb[1], &zb[2], &zb[3]);
  }
  if (guess) {
    std::vector<std::string> head;
    std::string e;
    ReadTextHead(path, 200, head, e);
    o.text = GuessTextFormat(head);
    std::printf("format deviné : %s\n", o.text.serialize().c_str());
  }
  if (streamN && onlyImport) {  // import par blocs, comme le plugin pour les très gros fichiers
    auto t0 = std::chrono::steady_clock::now();
    PointCloud first, buf;
    std::string e;
    XYZBlockSink take = [&](PointCloud& b) { first = b; return false; };
    if (!LoadXYZ(path, o, buf, e, nullptr, nullptr, &take, streamN)) { std::printf("ÉCHEC : %s\n", e.c_str()); return 1; }
    SurveyShape sh = AnalyzeSurvey(first, (Instrument)hint, force);
    const double midLat = 0.5 * (first.latMin + first.latMax);
    std::printf("PREMIER BLOC  : %zu sondes ; %s (%.1f s)\n", first.size(), sh.describe().c_str(),
                std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count());
    first = PointCloud();
    std::filesystem::remove_all(dbDir);
    BathyDb db;
    if (!db.open(dbDir, e)) { std::printf("base : %s\n", e.c_str()); return 1; }
    DbSource m;
    m.name = path.substr(path.find_last_of("/\\") + 1);
    m.date = "2020-01-01";
    m.interpD = sh.isGrid() ? 0 : sh.maxEdge;
    m.res = m.interpD > 0 ? sh.interpRes : sh.storeRes;
    if (forceRes > 0) m.res = forceRes;
    m.interpMethod = method >= 0 ? method : (sh.lines ? 1 : 0);
    size_t blocks = 0, pts = 0;
    BathyDb::CloudFeed feed = [&](const BathyDb::CloudSink& sink, std::string& fe) {
      XYZBlockSink each = [&](PointCloud& b) { ++blocks; pts += b.size(); return sink(b, 0.5f); };
      return LoadXYZ(path, o, buf, fe, nullptr, nullptr, &each, streamN);
    };
    int64_t id = 0;
    auto t2 = std::chrono::steady_clock::now();
    bool okI = db.importCloudStream(feed, midLat, m, sh.isGrid(), DbPriority::ResolutionDate, e, nullptr, nullptr, &id);
    double ti = std::chrono::duration<double>(std::chrono::steady_clock::now() - t2).count();
    long long bytes = 0;
    for (auto& f : std::filesystem::directory_iterator(dbDir)) bytes += (long long)f.file_size();
    const DbSource* d = db.source(id);
    std::printf("IMPORT BLOCS  : %s en %.1f s ; %zu blocs, %zu sondes ; niveau %d, %lld pixels mesurés, %lld interpolés ; base %.1f Mo\n",
                okI ? "OK" : e.c_str(), ti, blocks, pts, d ? d->level : 0, d ? d->count : 0LL,
                d ? d->interpCount : 0LL, bytes / 1048576.0);
    if (d) std::printf("EMPRISE       : lon %.6f..%.6f lat %.6f..%.6f ; z %.2f..%.2f\n", d->lonMin, d->lonMax, d->latMin, d->latMax, d->zmin, d->zmax);
    return okI ? 0 : 1;
  }
  PointCloud pc;
  std::string err;
  auto t0 = std::chrono::steady_clock::now();
  std::string ext = path.substr(path.find_last_of('.') + 1);
  for (auto& ch : ext) ch = (char)tolower(ch);
  double cl = 0, cb = 0;
  bool ok = (ext == "asc") ? LoadESRIAscii(path, o, pc, cl, cb, err, nullptr) : LoadXYZ(path, o, pc, err, nullptr, nullptr);
  double tl = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
  if (!ok) { std::printf("ÉCHEC : %s\n", err.c_str()); return 1; }
  const double midLat = 0.5 * (pc.latMin + pc.latMax), cosL = std::cos(midLat * M_PI / 180);
  const double W = (pc.lonMax - pc.lonMin) * 111320 * cosL, H = (pc.latMax - pc.latMin) * 110540;
  std::printf("points        : %zu   (lecture %.1f s)\n", pc.size(), tl);
  std::printf("emprise       : lon %.6f..%.6f  lat %.6f..%.6f  (%.0f x %.0f m)\n", pc.lonMin, pc.lonMax,
              pc.latMin, pc.latMax, W, H);
  std::printf("profondeurs   : %.2f .. %.2f m\n", pc.zMin, pc.zMax);
  std::printf("pas source    : X %.4f  Y %.4f m\n", pc.srcStepX, pc.srcStepY);
  bool gridded = false;
  t0 = std::chrono::steady_clock::now();
  double res = EstimateResolution(pc, gridded);
  std::printf("DÉCISION PLUGIN : %s, résolution %.3f m  (byArea %.3f m)\n", gridded ? "GRILLE" : "NUAGE", res,
              std::sqrt(std::max(1.0, W * H) / pc.size()));
  {
    auto t1 = std::chrono::steady_clock::now();
    SurveyShape sh = AnalyzeSurvey(pc, (Instrument)hint, force);
    std::printf("ANALYSE       : %s  [stockage %.3f m, D %.2f m, nn50 %.2f, arêtes %.2f/%.2f/%.2f (éclairci %.1f m), %zu sondes, %.1f s]\n",
                sh.describe().c_str(), sh.storeRes, sh.maxEdge, sh.nn50, sh.edge50, sh.edge90, sh.edge95, sh.thinCell, sh.analysed,
                std::chrono::duration<double>(std::chrono::steady_clock::now() - t1).count());
  }

  if (onlyImport) {  // import dans une base de test, avec surface interpolée
    SurveyShape sh = AnalyzeSurvey(pc, (Instrument)hint, force);
    std::filesystem::remove_all(dbDir);
    BathyDb db;
    std::string e;
    if (!db.open(dbDir, e)) { std::printf("base : %s\n", e.c_str()); return 1; }
    DbSource m;
    m.name = path.substr(path.find_last_of("/\\") + 1);
    m.date = "2020-01-01";
    m.interpD = sh.isGrid() ? 0 : sh.maxEdge;
    m.res = m.interpD > 0 ? sh.interpRes : sh.storeRes;
    if (forceRes > 0) m.res = forceRes;
    m.interpMethod = method >= 0 ? method : (sh.lines ? 1 : 0);
    int64_t id = 0;
    auto t2 = std::chrono::steady_clock::now();
    // durée de chaque étape (changement de message de progression)
    std::string lastStep;
    auto tStep = std::chrono::steady_clock::now();
    DbProgress steps = [&](float, const std::string& msg) {
      if (msg == lastStep) return;
      const auto now = std::chrono::steady_clock::now();
      if (!lastStep.empty())
        std::printf("  étape %-34s %6.2f s\n", lastStep.c_str(), std::chrono::duration<double>(now - tStep).count());
      lastStep = msg;
      tStep = now;
    };
    bool okI = db.importCloud(pc, m, sh.isGrid(), DbPriority::ResolutionDate, e, steps, nullptr, &id);
    double ti = std::chrono::duration<double>(std::chrono::steady_clock::now() - t2).count();
    long long bytes = 0;
    for (auto& f : std::filesystem::directory_iterator(dbDir)) bytes += (long long)f.file_size();
    const DbSource* d = db.source(id);
    std::printf("IMPORT        : %s en %.1f s ; niveau %d, %lld pixels mesurés, %lld interpolés ; base %.1f Mo ; %s\n",
                okI ? "OK" : e.c_str(), ti, d ? d->level : 0, d ? d->count : 0LL, d ? d->interpCount : 0LL,
                bytes / 1048576.0,
                sh.describe().c_str());
    if (okI && !viewOut.empty()) {  // vue de la base (lon lat z interpolé) pour contrôle visuel
      PointCloud v;
      double dl, dt;
      int used;
      std::vector<int64_t> src;
      std::vector<uint8_t> ip;
      const bool zm = zb[1] > zb[0];
      if (db.buildView(zm ? zb[0] : pc.lonMin, zm ? zb[1] : pc.lonMax, zm ? zb[2] : pc.latMin, zm ? zb[3] : pc.latMax, 22, 3000000, v, dl, dt, used, e, nullptr,
                       &src, &ip)) {
        FILE* f = std::fopen(viewOut.c_str(), "w");
        for (size_t k = 0; k < v.size(); ++k) std::fprintf(f, "%.7f %.7f %.2f %d\n", v.lon(k), v.lat(k), v.z[k], (int)ip[k]);
        std::fclose(f);
        std::printf("VUE           : %zu cellules (niveau %d) -> %s\n", v.size(), used, viewOut.c_str());
      }
    }
    return okI ? 0 : 1;
  }
  // ---- statistiques d'espacement sur une fenêtre d'au plus ~1,5 M points
  const size_t N = pc.size();
  const double kx = PointCloud::kQ * 111320 * cosL, ky = PointCloud::kQ * 110540;
  std::vector<double> X, Y;
  {
    double frac = std::min(1.0, 1.5e6 / (double)N);
    // fenêtre centrée sur un point « médian » de la donnée (pas le centre de l'emprise, souvent vide)
    std::vector<int32_t> a(pc.qlon.begin(), pc.qlon.end());
    std::nth_element(a.begin(), a.begin() + N / 2, a.end());
    std::vector<int32_t> b(pc.qlat.begin(), pc.qlat.end());
    std::nth_element(b.begin(), b.begin() + N / 2, b.end());
    const double cx = a[N / 2], cy = b[N / 2];
    double s = std::sqrt(frac);
    double hw = 0.5 * s * (pc.lonMax - pc.lonMin) / PointCloud::kQ, hh = 0.5 * s * (pc.latMax - pc.latMin) / PointCloud::kQ;
    for (int it = 0; it < 6; ++it) {
      X.clear(); Y.clear();
      for (size_t i = 0; i < N; ++i)
        if (std::fabs(pc.qlon[i] - cx) <= hw && std::fabs(pc.qlat[i] - cy) <= hh) {
          X.push_back((pc.qlon[i] - cx) * kx);
          Y.push_back((pc.qlat[i] - cy) * ky);
        }
      if (X.size() > 2.5e6) { hw *= 0.6; hh *= 0.6; continue; }
      if (X.size() < 2e5 && X.size() < N) { hw *= 1.6; hh *= 1.6; continue; }
      break;
    }
  }
  const size_t n = X.size();
  std::printf("fenêtre       : %zu points\n", n);
  // espacement entre sondes consécutives (ordre du fichier, fenêtre)
  {
    std::vector<double> d;
    for (size_t i = 0; i + 1 < n; ++i) {
      double v = std::hypot(X[i + 1] - X[i], Y[i + 1] - Y[i]);
      if (v > 0.01) d.push_back(v);
    }
    std::printf("consécutives  : P10 %.2f  P50 %.2f  P90 %.2f  P99 %.2f m\n", Pct(d, .1), Pct(d, .5), Pct(d, .9), Pct(d, .99));
  }
  // plus proche voisin (hachage)
  if (n > 10) {
    double x0 = *std::min_element(X.begin(), X.end()), y0 = *std::min_element(Y.begin(), Y.end());
    double x1 = *std::max_element(X.begin(), X.end()), y1 = *std::max_element(Y.begin(), Y.end());
    double c = std::max(0.05, std::sqrt(std::max(1.0, (x1 - x0) * (y1 - y0)) / n) * 1.5);
    std::unordered_map<uint64_t, std::vector<uint32_t>> g;
    g.reserve(n);
    auto key = [&](long ix, long iy) { return ((uint64_t)(uint32_t)ix << 32) | (uint32_t)iy; };
    for (size_t i = 0; i < n; ++i) g[key((long)((X[i] - x0) / c), (long)((Y[i] - y0) / c))].push_back((uint32_t)i);
    std::mt19937 rng(1);
    std::vector<double> nn;
    for (int k = 0; k < 40000; ++k) {
      size_t i = rng() % n;
      long ix = (long)((X[i] - x0) / c), iy = (long)((Y[i] - y0) / c);
      double best = 1e30;
      for (int r = 0; r < 4000; ++r) {
        for (long a = ix - r; a <= ix + r; ++a)
          for (long b = iy - r; b <= iy + r; ++b) {
            if (std::max(std::labs(a - ix), std::labs(b - iy)) != r) continue;
            auto it = g.find(key(a, b));
            if (it == g.end()) continue;
            for (uint32_t j : it->second) {
              if (j == i) continue;
              double v = std::hypot(X[j] - X[i], Y[j] - Y[i]);
              if (v > 0.005 && v < best) best = v;
            }
          }
        if (best < r * c) break;
      }
      if (best < 1e29) nn.push_back(best);
    }
    std::printf("plus proche   : P10 %.2f  P50 %.2f  P90 %.2f  P99 %.2f m\n", Pct(nn, .1), Pct(nn, .5), Pct(nn, .9), Pct(nn, .99));
  }
  // triangulation de Delaunay sur au plus 400 000 points de la fenêtre
  {
    std::vector<double> xy;
    size_t step = std::max<size_t>(1, n / 400000);
    // éviter les doublons exacts (delaunator les tolère mal)
    std::unordered_map<uint64_t, char> seen;
    for (size_t i = 0; i < n; i += step) {
      uint64_t k = ((uint64_t)(uint32_t)std::lround(X[i] * 100) << 32) | (uint32_t)std::lround(Y[i] * 100);
      if (seen.emplace(k, 1).second) { xy.push_back(X[i]); xy.push_back(Y[i]); }
    }
    t0 = std::chrono::steady_clock::now();
    auto tri = Triangulate(xy);
    double tt = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    std::vector<double> e;
    for (size_t t = 0; t + 2 < tri.size(); t += 3)
      for (int k = 0; k < 3; ++k) {
        size_t a = tri[t + k], b = tri[t + (k + 1) % 3];
        if (a < b) e.push_back(std::hypot(xy[2 * a] - xy[2 * b], xy[2 * a + 1] - xy[2 * b + 1]));
      }
    std::printf("delaunay      : %zu sommets (pas %zu), %zu triangles, %.1f s\n", xy.size() / 2, step, tri.size() / 3, tt);
    std::printf("arêtes        : P50 %.2f  P75 %.2f  P90 %.2f  P95 %.2f  P99 %.2f m\n", Pct(e, .5), Pct(e, .75),
                Pct(e, .9), Pct(e, .95), Pct(e, .99));
    // histogramme logarithmique des arêtes (bimodalité des levés en lignes)
    int hist[24] = {0};
    for (double v : e) { int b = (int)std::floor(std::log2(std::max(0.01, v)) * 1) + 7; hist[std::max(0, std::min(23, b))]++; }
    std::printf("histogramme   :");
    for (int b = 0; b < 24; ++b)
      if (hist[b]) std::printf(" %g-%g:%.1f%%", std::pow(2.0, b - 7), std::pow(2.0, b - 6), 100.0 * hist[b] / e.size());
    std::printf("\n");
  }
  return 0;
}

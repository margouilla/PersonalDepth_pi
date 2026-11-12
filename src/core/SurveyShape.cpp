// PersonalDepth_pi - nature d'un levé : grille ou nuage, résolution, distance D
// Copyright (C) 2026 Marcaurelio Franzetti
// SPDX-License-Identifier: GPL-2.0-or-later
#include "SurveyShape.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <random>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "Lang.h"
#include "PointCloud.h"
#include "Projection.h"
#include "Triangulate.h"

namespace pd {

namespace {
constexpr double kPi = 3.14159265358979323846;
constexpr size_t kSample = 300000;  // sondes utilisées pour chaque test

double Pct(std::vector<double> v, double p) {
  if (v.empty()) return 0;
  const size_t k = std::min(v.size() - 1, (size_t)(p * (double)(v.size() - 1)));
  std::nth_element(v.begin(), v.begin() + (long)k, v.end());
  return v[k];
}

// Réseau régulier sur des coordonnées entières (unités) ; occupation des nœuds
bool Lattice(const std::vector<int64_t>& x, const std::vector<int64_t>& y, double minStep,
             double& sx, double& sy, double& occ) {
  sx = LatticeStepUnits(x);
  sy = sx > 0 ? LatticeStepUnits(y) : 0;
  if (sx < minStep || sy < minStep) return false;
  const auto [x0, x1] = std::minmax_element(x.begin(), x.end());
  const auto [y0, y1] = std::minmax_element(y.begin(), y.end());
  const double nodes = ((double)(*x1 - *x0) / sx + 1) * ((double)(*y1 - *y0) / sy + 1);
  occ = nodes > 0 ? std::min(1.0, (double)x.size() / nodes) : 0;
  // un semis aux coordonnées arrondies ressemble à une grille au pas de
  // l'arrondi, mais n'en occupe qu'une infime partie des nœuds
  return occ >= 0.02;
}

// Réseau bruité sur un axe (mètres) : pas le plus fréquent entre valeurs
// distinctes (au cm), affiné par moindres carrés sur le rang des nœuds ;
// renvoie la part des points à moins de tol d'un nœud. Supporte quelques
// points hors réseau (sondes d'origine mêlées à la grille) et le bruit des
// coordonnées arrondies (DMS à 0,001").
double NoisyAxis(const std::vector<double>& v, double minStep, double& step) {
  step = 0;
  if (v.size() < 64) return 0;
  std::vector<int64_t> u(v.size());
  for (size_t i = 0; i < v.size(); ++i) u[i] = (int64_t)std::llround(v[i] * 100);
  std::sort(u.begin(), u.end());
  u.erase(std::unique(u.begin(), u.end()), u.end());
  std::vector<int64_t> d;
  for (size_t i = 0; i + 1 < u.size(); ++i)
    if (u[i + 1] - u[i] >= (int64_t)(minStep * 100)) d.push_back(u[i + 1] - u[i]);
  if (d.size() < 16) return 0;
  std::sort(d.begin(), d.end());
  // mode : fenêtre glissante de ±1 %
  size_t best = 0, bi = 0;
  for (size_t a = 0, b = 0; a < d.size(); ++a) {
    while (b < d.size() && d[b] <= d[a] + std::max<int64_t>(2, d[a] / 100)) ++b;
    if (b - a > best) { best = b - a; bi = a; }
  }
  double s0 = d[bi + best / 2] / 100.0;
  if (s0 < minStep) return 0;
  // affinage : x = a + s·k, sur une étendue croissante autour d'un point
  // d'origine (une erreur de pas se cumule avec le rang k du nœud)
  const double o = v[v.size() / 2];
  for (double span : {20.0, 100.0, 500.0, 3000.0, 1e9}) {
    double sk = 0, sx = 0, skk = 0, skx = 0;
    size_t n = 0;
    for (double x : v) {
      if (std::fabs(x - o) > span * s0) continue;
      const double k = std::round((x - o) / s0);
      if (std::fabs(x - o - k * s0) > 0.25 * s0) continue;
      sk += k; sx += x; skk += k * k; skx += k * x; ++n;
    }
    const double den = (double)n * skk - sk * sk;
    if (n < 16 || std::fabs(den) < 1e-9) break;
    s0 = ((double)n * skx - sk * sx) / den;
  }
  // phase (moyenne circulaire) puis part des points sur le réseau
  double cs = 0, sn = 0;
  for (double x : v) { const double a = 2 * kPi * std::fmod(x - o, s0) / s0; cs += std::cos(a); sn += std::sin(a); }
  const double ph = std::atan2(sn, cs) / (2 * kPi) * s0;
  const double tol = std::max(0.03, 0.03 * s0);
  size_t on = 0;
  for (double x : v) {
    const double r = x - o - ph;
    if (std::fabs(r - s0 * std::round(r / s0)) <= tol) ++on;
  }
  step = s0;
  return (double)on / (double)v.size();
}
}  // namespace

std::string SurveyShape::describe() const {
  char b[300];
  if (isGrid()) {
    std::string where = frame == "lonlat" ? pd::tr("en lon/lat")
                        : frame.rfind("utm", 0) == 0 ? pd::tr("en UTM ") + frame.substr(3)
                                                     : pd::tr("dans le système du fichier");
    std::snprintf(b, sizeof b, pd::tr("grille %.2f × %.2f m (%s), %.0f %% des nœuds occupés"), dx, dy,
                  where.c_str(), occupancy * 100);
  } else if (lines) {
    std::snprintf(b, sizeof b, pd::tr("nuage en lignes : sondes tous les %.2f m, lignes ~%.0f m ; D = %.0f m"),
                  nn50, edge90, maxEdge);
  } else {
    std::snprintf(b, sizeof b, pd::tr("nuage : espacement ~%.2f m ; D = %.2f m"), edge50, maxEdge);
  }
  std::string s = b;
  if (forced) s += pd::tr(" (imposé)");
  return s;
}

// P95 des grandes arêtes (plus de 5 fois l'arête médiane) : celles qui relient
// deux lignes d'un levé en lignes (0 s'il n'y en a pas)
static double BigEdgeP95(const std::vector<double>& e, double p50) {
  std::vector<double> big;
  for (double v : e)
    if (v > 5 * p50) big.push_back(v);
  return big.size() < 20 ? 0 : Pct(big, 0.95);
}

SurveyShape AnalyzeSurvey(const PointCloud& pc, Instrument hint, int force) {
  SurveyShape r;
  const size_t N = pc.size();
  if (N == 0) return r;
  const double midLat = 0.5 * (pc.latMin + pc.latMax);
  const double cosL = std::max(0.01, std::cos(midLat * kPi / 180.0));
  const double kx = PointCloud::kQ * 111320.0 * cosL, ky = PointCloud::kQ * 110540.0;
  const size_t ns = std::min(N, kSample);

  // ---- 1. grille ?
  if (force != 2) {
    if (pc.srcStepX > 0 && pc.srcStepY > 0) {  // système projeté du fichier
      r.kind = SurveyShape::Kind::Grid;
      r.dx = pc.srcStepX;
      r.dy = pc.srcStepY;
      r.frame = "source";
      r.occupancy = 1;  // déjà vérifiée à la lecture (>= 2 %)
    }
    if (!r.isGrid()) {  // lon/lat (degrés) : pas d'au moins 20 unités de 1e-7° (~2 m… 0,2 m)
      std::vector<int64_t> a(pc.qlon.begin(), pc.qlon.begin() + (long)ns);
      std::vector<int64_t> b(pc.qlat.begin(), pc.qlat.begin() + (long)ns);
      double sx, sy, occ;
      if (Lattice(a, b, 20, sx, sy, occ)) {
        r.kind = SurveyShape::Kind::Grid;
        r.dx = sx * kx;
        r.dy = sy * ky;
        r.frame = "lonlat";
        r.occupancy = occ;
      }
    }
    if (!r.isGrid() && pc.lonMax - pc.lonMin < 6) {
      // grille projetée diffusée en lon/lat : réseau dans la zone UTM du centre
      const double lon0 = 0.5 * (pc.lonMin + pc.lonMax);
      const int zone = std::max(1, std::min(60, (int)std::floor((lon0 + 180.0) / 6.0) + 1));
      Crs utm;
      std::string err;
      if (Crs::Parse("EPSG:" + std::to_string((midLat >= 0 ? 32600 : 32700) + zone), utm, err)) {
        std::vector<double> X(ns), Y(ns);
        bool ok = true;
        for (size_t i = 0; i < ns && ok; ++i)
          ok = utm.fromWGS84(pc.qlon[i] * PointCloud::kQ, pc.qlat[i] * PointCloud::kQ, X[i], Y[i]);
        double sx = 0, sy = 0;
        const double fx = ok ? NoisyAxis(X, 0.5, sx) : 0;
        const double fy = fx >= 0.9 ? NoisyAxis(Y, 0.5, sy) : 0;
        if (fx >= 0.9 && fy >= 0.9) {
          const auto [x0, x1] = std::minmax_element(X.begin(), X.end());
          const auto [y0, y1] = std::minmax_element(Y.begin(), Y.end());
          const double nodes = ((*x1 - *x0) / sx + 1) * ((*y1 - *y0) / sy + 1);
          const double occ = nodes > 0 ? std::min(1.0, (double)ns / nodes) : 0;
          if (occ >= 0.02) {
            r.kind = SurveyShape::Kind::Grid;
            r.dx = sx;
            r.dy = sy;
            r.frame = "utm" + std::to_string(zone) + (midLat >= 0 ? "N" : "S");
            r.occupancy = occ;
          }
        }
      }
    }
  }
  if (force == 1 && !r.isGrid()) r.forced = true;  // grille imposée : pas d'après l'espacement
  if (force == 2 && pc.srcStepX > 0) r.forced = true;

  // ---- 2. espacement : fenêtre autour d'une sonde « médiane » (dans la donnée)
  std::vector<double> X, Y;
  {
    std::vector<int32_t> a(pc.qlon.begin(), pc.qlon.end()), b(pc.qlat.begin(), pc.qlat.end());
    std::nth_element(a.begin(), a.begin() + (long)(N / 2), a.end());
    std::nth_element(b.begin(), b.begin() + (long)(N / 2), b.end());
    const double cx = a[N / 2], cy = b[N / 2];
    double s = std::sqrt(std::min(1.0, (double)kSample / (double)N));
    double hw = 0.5 * s * (pc.lonMax - pc.lonMin) / PointCloud::kQ + 1;
    double hh = 0.5 * s * (pc.latMax - pc.latMin) / PointCloud::kQ + 1;
    for (int it = 0; it < 8; ++it) {
      X.clear();
      Y.clear();
      for (size_t i = 0; i < N; ++i)
        if (std::fabs(pc.qlon[i] - cx) <= hw && std::fabs(pc.qlat[i] - cy) <= hh) {
          X.push_back((pc.qlon[i] - cx) * kx);
          Y.push_back((pc.qlat[i] - cy) * ky);
        }
      if (X.size() > 2 * kSample) { hw *= 0.6; hh *= 0.6; continue; }
      if (X.size() < kSample / 3 && X.size() < N) { hw *= 1.6; hh *= 1.6; continue; }
      break;
    }
  }
  // doublons exacts (au cm) retirés : bateau arrêté, recouvrement de fauchées
  {
    std::unordered_set<uint64_t> seen;
    seen.reserve(X.size() * 2);
    size_t k = 0;
    const size_t step = std::max<size_t>(1, X.size() / kSample);
    for (size_t i = 0; i < X.size(); i += step) {
      const uint64_t key = ((uint64_t)(uint32_t)std::llround(X[i] * 100) << 32) | (uint32_t)std::llround(Y[i] * 100);
      if (seen.insert(key).second) { X[k] = X[i]; Y[k] = Y[i]; ++k; }
    }
    X.resize(k);
    Y.resize(k);
  }
  const size_t n = X.size();
  r.analysed = n;
  if (n >= 3) {
    // plus proche voisin (hachage) sur 20 000 sondes tirées
    const double x0 = *std::min_element(X.begin(), X.end()), y0 = *std::min_element(Y.begin(), Y.end());
    const double x1 = *std::max_element(X.begin(), X.end()), y1 = *std::max_element(Y.begin(), Y.end());
    const double c = std::max(0.02, std::sqrt(std::max(1e-4, (x1 - x0) * (y1 - y0)) / (double)n) * 1.5);
    std::unordered_map<uint64_t, std::vector<uint32_t>> g;
    g.reserve(n);
    auto key = [](long ix, long iy) { return ((uint64_t)(uint32_t)ix << 32) | (uint32_t)iy; };
    for (size_t i = 0; i < n; ++i) g[key((long)((X[i] - x0) / c), (long)((Y[i] - y0) / c))].push_back((uint32_t)i);
    std::mt19937 rng(12345);
    std::vector<double> nn;
    const int ntry = (int)std::min<size_t>(n, 20000);
    for (int t = 0; t < ntry; ++t) {
      const size_t i = n <= 20000 ? (size_t)t : rng() % n;
      const long ix = (long)((X[i] - x0) / c), iy = (long)((Y[i] - y0) / c);
      double best = 1e30;
      for (long rr = 0; rr < 2000; ++rr) {
        for (long a = ix - rr; a <= ix + rr; ++a)
          for (long b = iy - rr; b <= iy + rr; ++b) {
            if (std::max(std::labs(a - ix), std::labs(b - iy)) != rr) continue;
            auto it = g.find(key(a, b));
            if (it == g.end()) continue;
            for (uint32_t j : it->second) {
              if (j == i) continue;
              const double v = std::hypot(X[j] - X[i], Y[j] - Y[i]);
              if (v < best) best = v;
            }
          }
        if (best < (double)rr * c) break;
      }
      if (best < 1e29) nn.push_back(best);
    }
    r.nn50 = Pct(nn, 0.5);
    // arêtes de la triangulation de Delaunay
    std::vector<double> xy(2 * n);
    for (size_t i = 0; i < n; ++i) { xy[2 * i] = X[i]; xy[2 * i + 1] = Y[i]; }
    const auto tri = Triangulate(xy);
    std::vector<double> e;
    e.reserve(tri.size());
    for (size_t t = 0; t + 2 < tri.size(); t += 3)
      for (int k = 0; k < 3; ++k) {
        const size_t a = tri[t + k], b = tri[t + (k + 1) % 3];
        if (a < b) e.push_back(std::hypot(xy[2 * a] - xy[2 * b], xy[2 * a + 1] - xy[2 * b + 1]));
      }
    r.edge50 = Pct(e, 0.5);
    r.edge90 = Pct(e, 0.9);
    r.edge95 = Pct(e, 0.95);
    r.gap95 = BigEdgeP95(e, r.edge50);
    // Levé en lignes très dense (sondeur à cadence rapide, sondes à quelques
    // décimètres le long de la route, un peu dispersées en travers) : les
    // milliers de petites arêtes à l'intérieur de chaque route masquent l'écart
    // entre les lignes. On refait les arêtes sur un échantillon éclairci (une
    // sonde par cellule de h = 4, 16, 64 × nn50) : chaque route devient une
    // chaîne de sondes espacées de ~h et l'écart entre lignes ressort (P90).
    if (r.nn50 > 0 && !(r.edge90 > 5 * r.edge50)) {
      const double ext = std::max(x1 - x0, y1 - y0);
      for (double f : {4.0, 16.0, 64.0}) {
        const double h = f * r.nn50;
        if (h > ext / 20) break;  // trop peu de cellules pour conclure
        std::unordered_set<uint64_t> cell;
        cell.reserve(n);
        std::vector<double> txy;
        for (size_t i = 0; i < n; ++i)
          if (cell.insert(key((long)((X[i] - x0) / h), (long)((Y[i] - y0) / h))).second) {
            txy.push_back(X[i]);
            txy.push_back(Y[i]);
          }
        if (txy.size() < 2 * 200) break;
        const auto tt = Triangulate(txy);
        std::vector<double> te;
        te.reserve(tt.size());
        for (size_t t = 0; t + 2 < tt.size(); t += 3)
          for (int k = 0; k < 3; ++k) {
            const size_t a = tt[t + k], b = tt[t + (k + 1) % 3];
            if (a < b) te.push_back(std::hypot(txy[2 * a] - txy[2 * b], txy[2 * a + 1] - txy[2 * b + 1]));
          }
        const double t50 = Pct(te, 0.5), t90 = Pct(te, 0.9);
        // routes (arêtes courantes ~ cellule) bien séparées (grandes arêtes)
        if (t50 > 0 && t50 < 2.5 * h && t90 > 5 * t50) {
          r.thinCell = h;
          r.edge50 = t50;
          r.edge90 = t90;
          r.edge95 = Pct(te, 0.95);
          r.gap95 = BigEdgeP95(te, t50);
          break;
        }
      }
    }
  }

  // ---- 3. décisions
  if (force == 1 && !r.isGrid()) {  // grille imposée sans réseau trouvé : pas = espacement
    r.kind = SurveyShape::Kind::Grid;
    r.dx = r.dy = std::max(0.05, r.edge50 > 0 ? std::min(r.edge50, 2 * r.nn50) : r.nn50);
    r.frame = "source";
  }
  if (force == 2 && r.isGrid()) { r.kind = SurveyShape::Kind::Cloud; r.forced = true; }
  if (r.isGrid()) {
    r.storeRes = r.interpRes = std::max(r.dx, r.dy);
    r.maxEdge = 0;
    return r;
  }
  r.lines = (hint == Instrument::SingleBeam) ||
            (hint == Instrument::Auto && n >= 50 && r.edge50 > 0 && r.edge90 > 5 * r.edge50);
  if (r.lines) {
    // un peu plus que les grands écarts entre lignes (95 % des arêtes qui
    // relient deux lignes), pour combler aussi les passages plus espacés
    r.maxEdge = 1.25 * std::max({r.gap95, r.edge90, r.edge50});
    // espacement le long de la route, mais pas plus fin que 1/100 de l'écart
    // entre lignes (sondeur à cadence rapide : la tache au fond fait bien plus)
    r.storeRes = std::max({0.05, r.nn50, r.maxEdge / 125.0});
  } else {
    r.storeRes = std::max(0.05, r.edge50 > 0 ? r.edge50 : r.nn50);
    double f = 2.0, cap = 0;
    switch (hint) {
      case Instrument::MultiBeam: f = 3.0; break;
      case Instrument::Lidar: f = 3.0; break;
      case Instrument::ChartSoundings: cap = 1000; break;
      case Instrument::LeadLine: cap = 500; break;
      default: break;
    }
    r.maxEdge = f * r.storeRes;
    // sondes de carte, plomb : semis irrégulier (routes, sondes choisies) ->
    // aussi les grands écarts courants (arête P90), dans la limite du plafond
    if (cap > 0) r.maxEdge = std::max(r.maxEdge, r.edge90);
    if (cap > 0) r.maxEdge = std::min(r.maxEdge, cap);
  }
  if (n < 3) r.storeRes = std::max(r.storeRes, 1.0);
  r.interpRes = r.lines ? r.storeRes : std::min(r.storeRes, std::max(2.0, r.maxEdge / 8.0));
  return r;
}

}  // namespace pd

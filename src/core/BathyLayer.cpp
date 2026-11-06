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

#include "BathyLayer.h"
#include "Lang.h"
#include "FileUtil.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <sstream>

#include "Projection.h"

namespace pd {
namespace fs = std::filesystem;

BathyLayer::~BathyLayer() {
  cancel_ = true;
  if (th_.joinable()) th_.join();
}

void BathyLayer::startLoad() {
  if (th_.joinable()) th_.join();
  cancel_ = false;
  progress_ = 0.f;
  state_.store(Loading, std::memory_order_release);
  th_ = std::thread([this] { run(); });
}

static std::string lowerExt(const std::string& p) {
  std::string e = U8Path(p).extension().u8string();
  std::transform(e.begin(), e.end(), e.begin(), ::tolower);
  return e;
}

void BathyLayer::run() {
  std::string err;
  bool ok = false;
  std::vector<uint32_t> tris;
  GridParams gp = params.grid;
  const std::string ext = lowerExt(params.path);
  LoadOptions lo = params.load;
  try {
    // Système de coordonnées : .prj voisin, sinon choix de l'utilisateur
    std::string def = params.crsDef, label = params.crsName;
    if (params.readPrj) {
      fs::path base = U8Path(params.path);
      for (fs::path prj : {U8Path(params.path + ".prj"), base.replace_extension(".prj")}) {
        std::error_code ec;
        if (!fs::exists(prj, ec)) continue;
        std::ifstream f(prj);
        std::stringstream ss; ss << f.rdbuf();
        int code = EpsgFromWkt(ss.str());
        if (code) { def = "EPSG:" + std::to_string(code); label = def + " (.prj)"; }
        break;
      }
    }
    if (!def.empty()) {
      auto crs = std::make_shared<Crs>();
      std::string cerr;
      if (!Crs::Parse(def, *crs, cerr)) throw std::runtime_error(cerr);
      if (!crs->isWGS84LonLat()) lo.crs = crs;
    }
    crsUsed_ = lo.crs ? (label.empty() ? def : label) : std::string();
    const bool projected = lo.crs && !lo.crs->isGeographic();

    if (params.custom) {
      double dl = 0, dt = 0;
      ok = params.custom(cloud, dl, dt, pointSource, err);
      gp.forceDLon = dl; gp.forceDLat = dt;
      gp.mode = GridMode::Shoalest;
    } else if (ext == ".asc") {
      double cl, ca;
      ok = LoadESRIAscii(params.path, lo, cloud, cl, ca, err, stopFlag());
      if (projected) {
        if (gp.cellMeters <= 0) gp.cellMeters = std::max(cloud.srcStepX, cloud.srcStepY);
      } else if (!lo.crs) {
        gp.forceDLon = cl; gp.forceDLat = ca;
      }
    } else if (ext == ".xml" || ext == ".landxml") {
      ok = LoadLandXMLTin(params.path, lo, cloud, tris, err);
    } else if (ext == ".tif" || ext == ".tiff" || ext == ".bag" || ext == ".nc" ||
               ext == ".grd") {
#ifdef PD_HAVE_GDAL
      double cl, ca;
      ok = LoadGdalRaster(params.path, lo, cloud, cl, ca, err);
      gp.forceDLon = cl; gp.forceDLat = ca;
#else
      err = pd::tr("Format raster ") + ext + pd::tr(" : plugin compilé sans GDAL (utiliser .asc ou .xyz)");
#endif
    } else {  // XYZ texte, avec cache binaire
      char tag[128], rng[48] = "";
      const size_t crsHash = lo.crs ? std::hash<std::string>()(lo.crs->definition()) & 0xFFFFFF : 0;
      if (!params.load.nodata.empty()) {  // NODATA de l'utilisateur : autre cache
        std::string nd;
        for (double v : params.load.nodata) nd += std::to_string(v) + ";";
        std::snprintf(rng, sizeof rng, "_n%06zx", std::hash<std::string>()(nd) & 0xFFFFFF);
      }
      if (params.load.text.custom)  // format personnalisé : autre cache
        std::snprintf(rng + std::strlen(rng), sizeof rng - std::strlen(rng), "_f%06zx",
                      std::hash<std::string>()(params.load.text.serialize()) & 0xFFFFFF);
      if (params.load.zToMeters != 1.0)  // fichier en pieds : autre cache
        std::snprintf(rng + std::strlen(rng), sizeof rng - std::strlen(rng), "_u%g",
                      params.load.zToMeters);
      std::snprintf(tag, sizeof tag, ".%d%d_%g%s_%06zx.v3.pdpc", (int)params.load.order,
                    (int)params.load.zsign, params.load.zOffset, rng, crsHash);
      const std::string cache = params.path + tag;
      std::error_code ec;
      bool fresh = params.useCache && fs::exists(U8Path(cache), ec) &&
                   fs::last_write_time(U8Path(cache), ec) >= fs::last_write_time(U8Path(params.path), ec);
      if (fresh && cloud.loadCache(cache)) {
        ok = true;
        progress_ = 0.6f;
      } else {
        ok = LoadXYZ(params.path, lo, cloud, err,
                     [this](float f) { progress_ = 0.6f * f; }, stopFlag());
        if (ok && params.useCache) cloud.saveCache(cache);  // échec silencieux (lecture seule)
      }
    }
    // Référence verticale : profondeurs ramenées au zéro hydrographique
    vrefInfo_.clear();
    if (ok && !stopped() && params.vref.active() && !params.custom) {
      progress_ = 0.62f;
      VerticalCorrection vc;
      std::string verr;
      if (!vc.build(params.vref, cloud.lonMin, cloud.lonMax, cloud.latMin, cloud.latMax, verr))
        throw std::runtime_error(verr);
      float cmin, cmax;
      const size_t miss = vc.apply(cloud, cmin, cmax);
      if (miss == cloud.size()) throw std::runtime_error(pd::tr("Aucune sonde dans la couverture de la référence verticale"));
      char b[200];
      std::snprintf(b, sizeof b, "%s → ZH (%+.2f…%+.2f m)", params.vref.label.empty() ? pd::tr("Réf. terrestre") : params.vref.label.c_str(), cmin, cmax);
      vrefInfo_ = b;
      if (miss) {
        std::snprintf(b, sizeof b, pd::tr(" — %zu sondes hors couverture NON converties"), miss);
        vrefInfo_ += b;
      }
    }
    // Bornes d'import sur la profondeur finale
    if (ok && !stopped() && params.depthBounds && !params.custom) {
      cloud.keepDepths(params.depthMin, params.depthMax, tris.empty() ? nullptr : &tris);
      if (cloud.empty()) {
        char b[160];
        std::snprintf(b, sizeof b, pd::tr("Aucune sonde entre %.1f et %.1f m (bornes d'import)"),
                      params.depthMin, params.depthMax);
        throw std::runtime_error(b);
      }
    }
    // Levé maillé en projection : cellule = pas d'origine, et grille
    // d'affichage interpolée depuis les nœuds d'origine
    LatticeSource lat;
    if (ok && params.cloudOnly) { progress_ = 1.f; state_.store(stopped() ? Failed : Ready, std::memory_order_release); return; }
    if (ok && projected && !params.custom && cloud.srcStepX > 0 && cloud.srcStepY > 0) {
      if (gp.cellMeters <= 0) gp.cellMeters = std::max(cloud.srcStepX, cloud.srcStepY);
      if (gp.mode != GridMode::Tin && buildLattice(lo, lat)) gp.lattice = &lat;
    }
    if (ok && !stopped()) {
      progress_ = 0.65f;
      ok = grid.build(cloud, gp, tris.empty() ? nullptr : &tris, err);
      progress_ = 0.8f;
      if (ok) {
        grid.shadeSmooth = params.shadeSmooth;
        grid.computeHillshade(params.exaggeration);
      }
      progress_ = 0.9f;
      if (ok) index.build(cloud);
    }
  } catch (const std::bad_alloc&) {
    ok = false; err = pd::tr("Mémoire insuffisante : augmenter la taille de cellule");
  } catch (const std::exception& e) {
    ok = false; err = e.what();
  }
  if (stopped()) { ok = false; err = "Annulé"; }
  error_ = err;
  progress_ = 1.f;
  state_.store(ok ? Ready : Failed, std::memory_order_release);
}

// Reconstitue la grille projetée d'origine à partir des sondes (projetées en
// retour) ; les nœuds en double gardent la plus faible profondeur.
bool BathyLayer::buildLattice(const LoadOptions& lo, LatticeSource& L) {
  const auto crs = lo.crs;
  if (!crs || cloud.empty()) return false;
  const double u = crs->unitToMeter();
  L.dx = cloud.srcStepX / u;
  L.dy = cloud.srcStepY / u;
  std::vector<double> E(cloud.size()), N(cloud.size());
  double emin = 1e300, nmin = 1e300, emax = -1e300, nmax = -1e300;
  for (size_t k = 0; k < cloud.size(); ++k) {
    if (!crs->fromWGS84(cloud.lon(k), cloud.lat(k), E[k], N[k])) return false;
    emin = std::min(emin, E[k]); emax = std::max(emax, E[k]);
    nmin = std::min(nmin, N[k]); nmax = std::max(nmax, N[k]);
  }
  L.e0 = emin; L.n0 = nmin;
  L.nx = (int)std::lround((emax - emin) / L.dx) + 1;
  L.ny = (int)std::lround((nmax - nmin) / L.dy) + 1;
  if ((double)L.nx * L.ny > 200e6) return false;  // trop grand : méthode générale
  L.z.assign((size_t)L.nx * L.ny, NAN);
  size_t off = 0;
  for (size_t k = 0; k < cloud.size(); ++k) {
    const double fi = (E[k] - L.e0) / L.dx, fj = (N[k] - L.n0) / L.dy;
    const long i = std::lround(fi), j = std::lround(fj);
    if (std::fabs(fi - i) > 0.1 || std::fabs(fj - j) > 0.1) ++off;  // hors maillage
    float& c = L.z[(size_t)j * L.nx + i];
    if (std::isnan(c) || cloud.z[k] < c) c = cloud.z[k];
  }
  if (off > cloud.size() / 20) return false;  // pas une vraie grille régulière
  L.toSrc = [crs](double lon, double lat, double& e, double& n) {
    return crs->fromWGS84(lon, lat, e, n);
  };
  return true;
}

std::string BathyLayer::summary() const {
  std::string name = params.custom ? params.label : fs::u8path(params.path).filename().u8string();
  char b[512];
  switch (state()) {
    case Loading:
      std::snprintf(b, sizeof b, pd::tr("%s — chargement %d %%"), name.c_str(), (int)(progress() * 100));
      break;
    case Failed:
      std::snprintf(b, sizeof b, pd::tr("%s — ERREUR : %s"), name.c_str(), error_.c_str());
      break;
    case Ready: {
      const GridLevel& g = grid.levels[0];
      double cellM = g.dlat * 110540.0;
      const bool srcGrid = cloud.srcStepX > 0 && cloud.srcStepY > 0;
      std::snprintf(b, sizeof b, pd::tr("%s%s — %.2f M sondes, %.2f…%.2f m, cellule %.1f m%s, %d×%d%s%s"),
                    visible ? "" : pd::tr("(masqué) "), name.c_str(), cloud.size() / 1e6,
                    cloud.zMin, cloud.zMax, cellM,
                    grid.nativeLattice ? pd::tr(" (grille native)") : srcGrid ? pd::tr(" (pas d'origine)") : "",
                    g.nx, g.ny, crsUsed_.empty() ? "" : " — ", crsUsed_.c_str());
      if (!vrefInfo_.empty()) {
        std::string r = std::string(b) + " — " + vrefInfo_;
        return r;
      }
      break;
    }
    default:
      std::snprintf(b, sizeof b, "%s", name.c_str());
  }
  return b;
}

bool BathyLayer::probe(double lon, double lat, double radiusDeg, float& gz,
                       float& sz, double& sdist, int64_t* sourceId) const {
  if (sourceId) *sourceId = 0;
  if (state() != Ready || grid.empty()) return false;
  const GridLevel& g = grid.levels[0];
  int i, j;
  gz = g.cellOf(lon, lat, i, j) ? g.at(i, j) : NAN;
  sz = NAN; sdist = -1;
  uint32_t k;
  if (index.nearest(cloud, lon, lat, radiusDeg, k)) {
    sz = cloud.z[k];
    if (sourceId && k < pointSource.size()) *sourceId = pointSource[k];
    double dx = (cloud.lon(k) - lon) * 111320.0 * std::cos(lat * M_PI / 180.0);
    double dy = (cloud.lat(k) - lat) * 110540.0;
    sdist = std::hypot(dx, dy);
  }
  return !std::isnan(gz) || !std::isnan(sz);
}

}  // namespace pd

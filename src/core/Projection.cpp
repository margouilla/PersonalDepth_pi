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

#include "Projection.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <regex>
#include <sstream>

namespace pd {

namespace {
const double D2R = M_PI / 180.0, R2D = 180.0 / M_PI;
const double SEC2RAD = M_PI / (180.0 * 3600.0);
const double kWgsA = 6378137.0, kWgsF = 1 / 298.257223563;

struct Ellps { const char* name; double a, rf; };  // rf = 1/f (0 : utiliser b)
const Ellps kEllps[] = {
    {"GRS80", 6378137.0, 298.257222101},  {"WGS84", 6378137.0, 298.257223563},
    {"intl", 6378388.0, 297.0},           {"bessel", 6377397.155, 299.1528128},
    {"airy", 6377563.396, 299.3249646},   {"clrk66", 6378206.4, 294.9786982138982},
    {"clrk80ign", 6378249.2, 293.4660212936269}, {"clrk80", 6378249.145, 293.465},
    {"krass", 6378245.0, 298.3},          {"mod_airy", 6377340.189, 299.3249646},
    {"WGS72", 6378135.0, 298.26},         {"aust_SA", 6378160.0, 298.25},
    {"helmert", 6378200.0, 298.3},        {"sphere", 6370997.0, 0}};

double tfn(double phi, double e) {  // t(φ) des projections coniques/stéréographiques
  const double s = e * std::sin(phi);
  return std::tan(M_PI / 4 - phi / 2) / std::pow((1 - s) / (1 + s), e / 2);
}
double mfn(double phi, double e2) {
  return std::cos(phi) / std::sqrt(1 - e2 * std::sin(phi) * std::sin(phi));
}
double phiFromT(double t, double e) {  // inverse de tfn par itération
  double phi = M_PI / 2 - 2 * std::atan(t);
  for (int i = 0; i < 30; ++i) {
    const double s = e * std::sin(phi);
    const double np = M_PI / 2 - 2 * std::atan(t * std::pow((1 - s) / (1 + s), e / 2));
    if (std::fabs(np - phi) < 1e-14) return np;
    phi = np;
  }
  return phi;
}
double normLon(double l) {
  while (l > M_PI) l -= 2 * M_PI;
  while (l < -M_PI) l += 2 * M_PI;
  return l;
}
void geodToEcef(double lam, double phi, double a, double e2, double& X, double& Y, double& Z) {
  const double s = std::sin(phi), c = std::cos(phi);
  const double N = a / std::sqrt(1 - e2 * s * s);
  X = N * c * std::cos(lam); Y = N * c * std::sin(lam); Z = N * (1 - e2) * s;
}
void ecefToGeod(double X, double Y, double Z, double a, double e2, double& lam, double& phi) {
  lam = std::atan2(Y, X);
  const double p = std::hypot(X, Y);
  phi = std::atan2(Z, p * (1 - e2));
  for (int i = 0; i < 10; ++i) {
    const double s = std::sin(phi);
    const double N = a / std::sqrt(1 - e2 * s * s);
    const double h = p / std::cos(phi) - N;
    const double np = std::atan2(Z, p * (1 - e2 * N / (N + h)));
    if (std::fabs(np - phi) < 1e-13) { phi = np; break; }
    phi = np;
  }
}
}  // namespace

// ----------------------------------------------------------------- parseur
bool Crs::Parse(const std::string& defIn, Crs& o, std::string& err) {
  std::string def = defIn;
  // Code EPSG seul
  {
    std::string t = def;
    t.erase(std::remove_if(t.begin(), t.end(), ::isspace), t.end());
    std::string u = t;
    std::transform(u.begin(), u.end(), u.begin(), ::toupper);
    if (u.rfind("EPSG:", 0) == 0) t = t.substr(5);
    if (!t.empty() && std::all_of(t.begin(), t.end(), ::isdigit)) {
      def = EpsgToProj(std::atoi(t.c_str()));
      if (def.empty()) { err = "Code EPSG:" + t + " non pris en charge (utiliser une chaîne PROJ)"; return false; }
    }
  }
  std::map<std::string, std::string> kv;
  std::istringstream in(def);
  std::string tok;
  while (in >> tok) {
    if (tok[0] != '+') continue;
    tok = tok.substr(1);
    size_t eq = tok.find('=');
    if (eq == std::string::npos) kv[tok] = "";
    else kv[tok.substr(0, eq)] = tok.substr(eq + 1);
  }
  if (!kv.count("proj")) { err = "Définition sans +proj"; return false; }
  Crs c;
  c.def_ = def;
  auto num = [&](const char* k, double dflt) {
    auto it = kv.find(k);
    return it == kv.end() || it->second.empty() ? dflt : std::atof(it->second.c_str());
  };
  // ellipsoïde / datum
  double a = kWgsA, f = kWgsF;
  std::string ell = kv.count("ellps") ? kv["ellps"] : "";
  std::string datum = kv.count("datum") ? kv["datum"] : "";
  std::string tw = kv.count("towgs84") ? kv["towgs84"] : "";
  if (datum == "WGS84") ell = "WGS84";
  else if (datum == "NAD83") ell = "GRS80";
  else if (datum == "NAD27") { ell = "clrk66"; if (tw.empty()) tw = "-8,160,176"; }
  else if (datum == "OSGB36") { ell = "airy"; if (tw.empty()) tw = "446.448,-125.157,542.06,0.15,0.247,0.842,-20.489"; }
  else if (datum == "potsdam") { ell = "bessel"; if (tw.empty()) tw = "598.1,73.7,418.2,0.202,0.045,-2.455,6.7"; }
  else if (!datum.empty()) { err = "Datum +datum=" + datum + " non reconnu (utiliser +towgs84)"; return false; }
  if (!ell.empty()) {
    bool found = false;
    for (const Ellps& E : kEllps)
      if (ell == E.name) { a = E.a; f = E.rf > 0 ? 1 / E.rf : 0; found = true; }
    if (!found) { err = "Ellipsoïde +ellps=" + ell + " inconnu"; return false; }
  }
  if (kv.count("a")) {
    a = num("a", a);
    if (kv.count("rf")) f = 1 / num("rf", 298.257223563);
    else if (kv.count("b")) f = (a - num("b", a)) / a;
    else if (kv.count("f")) f = num("f", 0);
    else f = 0;
  } else if (kv.count("R")) { a = num("R", a); f = 0; }
  c.a_ = a; c.f_ = f;
  if (!tw.empty()) {
    double v[7] = {0, 0, 0, 0, 0, 0, 0};
    int n = 0;
    std::istringstream ts(tw);
    std::string p;
    while (n < 7 && std::getline(ts, p, ',')) v[n++] = std::atof(p.c_str());
    if (n != 3 && n != 7) { err = "+towgs84 doit avoir 3 ou 7 valeurs"; return false; }
    bool nz = false;
    for (double x : v) nz |= (x != 0);
    c.hasHelmert_ = nz;
    for (int i = 0; i < 3; ++i) c.tw_[i] = v[i];
    for (int i = 3; i < 6; ++i) c.tw_[i] = v[i] * SEC2RAD;
    c.tw_[6] = v[6] * 1e-6;
  }
  // méridien d'origine, unités
  if (kv.count("pm")) {
    const std::string& pm = kv["pm"];
    if (pm == "paris") c.pm_ = 2.337229166666667 * D2R;
    else if (pm == "greenwich") c.pm_ = 0;
    else if (pm == "ferro") c.pm_ = -17.666666666666667 * D2R;
    else c.pm_ = std::atof(pm.c_str()) * D2R;
  }
  if (kv.count("to_meter")) c.unit_ = num("to_meter", 1);
  else if (kv.count("units")) {
    const std::string& u = kv["units"];
    if (u == "m") c.unit_ = 1;
    else if (u == "ft") c.unit_ = 0.3048;
    else if (u == "us-ft") c.unit_ = 1200.0 / 3937.0;
    else if (u == "km") c.unit_ = 1000;
    else { err = "Unité +units=" + u + " inconnue"; return false; }
  }
  // projection
  const std::string& pj = kv["proj"];
  c.lat0_ = num("lat_0", 0) * D2R;
  c.lon0_ = num("lon_0", 0) * D2R;
  c.k0_ = kv.count("k_0") ? num("k_0", 1) : num("k", 1);
  c.x0_ = num("x_0", 0);
  c.y0_ = num("y_0", 0);
  if (pj == "longlat" || pj == "latlong" || pj == "lonlat" || pj == "latlon") {
    c.kind_ = Kind::LongLat;
  } else if (pj == "utm") {
    int zone = (int)num("zone", 0);
    if (zone < 1 || zone > 60) { err = "+zone UTM invalide"; return false; }
    c.kind_ = Kind::TMerc;
    c.lon0_ = (-183.0 + 6.0 * zone) * D2R;
    c.lat0_ = 0; c.k0_ = 0.9996; c.x0_ = 500000;
    c.y0_ = kv.count("south") ? 10000000 : 0;
  } else if (pj == "tmerc" || pj == "etmerc") {
    c.kind_ = Kind::TMerc;
  } else if (pj == "lcc") {
    c.kind_ = Kind::Lcc;
    c.lat1_ = kv.count("lat_1") ? num("lat_1", 0) * D2R : c.lat0_;
    c.lat2_ = kv.count("lat_2") ? num("lat_2", 0) * D2R : c.lat1_;
  } else if (pj == "stere" || pj == "ups") {
    if (pj == "ups") {
      c.lat0_ = kv.count("south") ? -M_PI / 2 : M_PI / 2;
      c.k0_ = 0.994; c.x0_ = 2000000; c.y0_ = 2000000; c.lon0_ = 0;
    }
    if (std::fabs(std::fabs(c.lat0_) - M_PI / 2) > 1e-9) {
      err = "Stéréographique non polaire : utiliser +proj=sterea"; return false;
    }
    c.south_ = c.lat0_ < 0;
    c.latts_ = kv.count("lat_ts") ? num("lat_ts", 90) * D2R : c.lat0_;
    c.kind_ = std::fabs(std::fabs(c.latts_) - M_PI / 2) < 1e-9 ? Kind::StereA : Kind::StereB;
  } else if (pj == "sterea") {
    c.kind_ = Kind::Sterea;
  } else if (pj == "somerc") {
    c.kind_ = Kind::Somerc;
  } else if (pj == "merc") {
    c.kind_ = Kind::Merc;
    c.latts_ = num("lat_ts", 0) * D2R;
    c.lat1_ = kv.count("lat_ts") ? 1 : 0;  // drapeau : k0 issu de lat_ts
  } else {
    err = "Projection +proj=" + pj + " non prise en charge";
    return false;
  }
  c.init();
  o = c;
  return true;
}

// ---------------------------------------------------------- constantes
void Crs::init() {
  e2_ = f_ * (2 - f_);
  e_ = std::sqrt(e2_);
  const double e = e_, e2 = e2_;
  switch (kind_) {
    case Kind::TMerc: {
      const double n = f_ / (2 - f_), n2 = n * n, n3 = n2 * n, n4 = n3 * n, n5 = n4 * n, n6 = n5 * n;
      c_[0] = a_ / (1 + n) * (1 + n2 / 4 + n4 / 64 + n6 / 256);  // A
      alpha_[1] = n / 2 - 2 * n2 / 3 + 5 * n3 / 16 + 41 * n4 / 180 - 127 * n5 / 288 + 7891 * n6 / 37800;
      alpha_[2] = 13 * n2 / 48 - 3 * n3 / 5 + 557 * n4 / 1440 + 281 * n5 / 630 - 1983433 * n6 / 1935360;
      alpha_[3] = 61 * n3 / 240 - 103 * n4 / 140 + 15061 * n5 / 26880 + 167603 * n6 / 181440;
      alpha_[4] = 49561 * n4 / 161280 - 179 * n5 / 168 + 6601661 * n6 / 7257600;
      alpha_[5] = 34729 * n5 / 80640 - 3418889 * n6 / 1995840;
      alpha_[6] = 212378941 * n6 / 319334400;
      beta_[1] = n / 2 - 2 * n2 / 3 + 37 * n3 / 96 - n4 / 360 - 81 * n5 / 512 + 96199 * n6 / 604800;
      beta_[2] = n2 / 48 + n3 / 15 - 437 * n4 / 1440 + 46 * n5 / 105 - 1118711 * n6 / 3870720;
      beta_[3] = 17 * n3 / 480 - 37 * n4 / 840 - 209 * n5 / 4480 + 5569 * n6 / 90720;
      beta_[4] = 4397 * n4 / 161280 - 11 * n5 / 504 - 830251 * n6 / 7257600;
      beta_[5] = 4583 * n5 / 161280 - 108847 * n6 / 3991680;
      beta_[6] = 20648693 * n6 / 638668800;
      // ξ de la latitude d'origine (sur le méridien central)
      double x, y;
      c_[1] = 0;
      const double sx0 = x0_, sy0 = y0_, sk = k0_;
      x0_ = 0; y0_ = 0; k0_ = 1;
      projForward(lon0_, lat0_, x, y);  // sur le méridien central
      c_[1] = y / c_[0];
      x0_ = sx0; y0_ = sy0; k0_ = sk;
      break;
    }
    case Kind::Lcc: {
      const double m1 = mfn(lat1_, e2), m2 = mfn(lat2_, e2);
      const double t1 = tfn(lat1_, e), t2 = tfn(lat2_, e), t0 = tfn(lat0_, e);
      const double n = std::fabs(lat1_ - lat2_) > 1e-12
                           ? (std::log(m1) - std::log(m2)) / (std::log(t1) - std::log(t2))
                           : std::sin(lat1_);
      const double F = m1 / (n * std::pow(t1, n));
      c_[0] = n; c_[1] = F;
      c_[2] = a_ * k0_ * F * std::pow(t0, n);  // r0
      break;
    }
    case Kind::StereA:
      c_[0] = std::sqrt(std::pow(1 + e, 1 + e) * std::pow(1 - e, 1 - e));
      break;
    case Kind::StereB: {
      const double pc = std::fabs(latts_);
      c_[0] = tfn(pc, e);   // tc
      c_[1] = mfn(pc, e2);  // mc
      break;
    }
    case Kind::Sterea: {  // EPSG 9809 (sphère conforme de Gauss)
      const double s0 = std::sin(lat0_);
      const double rho0 = a_ * (1 - e2) / std::pow(1 - e2 * s0 * s0, 1.5);
      const double nu0 = a_ / std::sqrt(1 - e2 * s0 * s0);
      const double R = std::sqrt(rho0 * nu0);
      const double n = std::sqrt(1 + e2 * std::pow(std::cos(lat0_), 4) / (1 - e2));
      const double S1 = (1 + s0) / (1 - s0), S2 = (1 - e * s0) / (1 + e * s0);
      const double w1 = std::pow(S1 * std::pow(S2, e), n);
      const double sinchi00 = (w1 - 1) / (w1 + 1);
      const double cc = (n + s0) * (1 - sinchi00) / ((n - s0) * (1 + sinchi00));
      const double w2 = cc * w1;
      const double chi0 = std::asin((w2 - 1) / (w2 + 1));
      c_[0] = R; c_[1] = n; c_[2] = cc; c_[3] = chi0;
      break;
    }
    case Kind::Somerc: {  // oblique conforme suisse (formules PROJ somerc)
      const double one_es = 1 - e2;
      double cp = std::cos(lat0_);
      cp *= cp;
      const double c = std::sqrt(1 + e2 * cp * cp / one_es);
      double sp = std::sin(lat0_);
      const double sinp0 = sp / c;
      const double phip0 = std::asin(sinp0);
      const double cosp0 = std::cos(phip0);
      sp *= e;
      const double K = std::log(std::tan(M_PI / 4 + 0.5 * phip0)) -
                       c * (std::log(std::tan(M_PI / 4 + 0.5 * lat0_)) -
                            0.5 * e * std::log((1 + sp) / (1 - sp)));
      const double kR = k0_ * std::sqrt(one_es) / (1 - sp * sp);
      c_[0] = c; c_[1] = sinp0; c_[2] = cosp0; c_[3] = K; c_[4] = kR;
      break;
    }
    case Kind::Merc:
      c_[0] = lat1_ != 0 ? mfn(latts_, e2) : k0_;  // facteur d'échelle effectif
      break;
    default: break;
  }
}

// ------------------------------------------------------- projection directe
bool Crs::projForward(double lam, double phi, double& x, double& y) const {
  const double e = e_;
  lam = normLon(lam - lon0_);
  switch (kind_) {
    case Kind::LongLat:
      x = lam + lon0_; y = phi;
      return true;
    case Kind::TMerc: {
      const double lim = M_PI / 2 - 1e-10;
      phi = std::max(-lim, std::min(lim, phi));
      const double tau = std::tan(phi);
      const double sig = std::sinh(e * std::atanh(e * tau / std::hypot(1.0, tau)));
      const double taup = tau * std::hypot(1.0, sig) - sig * std::hypot(1.0, tau);
      const double xip = std::atan2(taup, std::cos(lam));
      const double etap = std::asinh(std::sin(lam) / std::hypot(taup, std::cos(lam)));
      double xi = xip, eta = etap;
      for (int j = 1; j <= 6; ++j) {
        xi += alpha_[j] * std::sin(2 * j * xip) * std::cosh(2 * j * etap);
        eta += alpha_[j] * std::cos(2 * j * xip) * std::sinh(2 * j * etap);
      }
      x = x0_ + k0_ * c_[0] * eta;
      y = y0_ + k0_ * c_[0] * (xi - c_[1]);
      return true;
    }
    case Kind::Lcc: {
      const double n = c_[0], F = c_[1], r0 = c_[2];
      const double r = a_ * k0_ * F * std::pow(tfn(phi, e), n);
      const double th = n * lam;
      x = x0_ + r * std::sin(th);
      y = y0_ + r0 - r * std::cos(th);
      return true;
    }
    case Kind::StereA:
    case Kind::StereB: {
      const double s = south_ ? -1 : 1;
      const double t = tfn(s * phi, e);
      const double rho = kind_ == Kind::StereA ? 2 * a_ * k0_ * t / c_[0] : a_ * c_[1] * t / c_[0];
      x = x0_ + rho * std::sin(lam);
      y = y0_ - s * rho * std::cos(lam);
      return true;
    }
    case Kind::Sterea: {
      const double R = c_[0], n = c_[1], cc = c_[2], chi0 = c_[3];
      const double sp = std::sin(phi);
      const double Sa = (1 + sp) / (1 - sp), Sb = (1 - e * sp) / (1 + e * sp);
      const double w = cc * std::pow(Sa * std::pow(Sb, e), n);
      const double chi = std::asin((w - 1) / (w + 1));
      const double L = n * lam;
      const double B = 1 + std::sin(chi) * std::sin(chi0) + std::cos(chi) * std::cos(chi0) * std::cos(L);
      x = x0_ + 2 * R * k0_ * std::cos(chi) * std::sin(L) / B;
      y = y0_ + 2 * R * k0_ * (std::sin(chi) * std::cos(chi0) - std::cos(chi) * std::sin(chi0) * std::cos(L)) / B;
      return true;
    }
    case Kind::Somerc: {
      const double c = c_[0], sinp0 = c_[1], cosp0 = c_[2], K = c_[3], kR = c_[4];
      const double sp = e * std::sin(phi);
      const double phip = 2 * std::atan(std::exp(c * (std::log(std::tan(M_PI / 4 + 0.5 * phi)) -
                                                      0.5 * e * std::log((1 + sp) / (1 - sp))) + K)) - M_PI / 2;
      const double lamp = c * lam;
      const double cp = std::cos(phip);
      const double phipp = std::asin(cosp0 * std::sin(phip) - sinp0 * cp * std::cos(lamp));
      const double lampp = std::asin(cp * std::sin(lamp) / std::cos(phipp));
      x = x0_ + a_ * kR * lampp;
      y = y0_ + a_ * kR * std::log(std::tan(M_PI / 4 + 0.5 * phipp));
      return true;
    }
    case Kind::Merc: {
      const double k = c_[0];
      if (std::fabs(phi) > M_PI / 2 - 1e-9) return false;
      x = x0_ + a_ * k * lam;
      y = y0_ - a_ * k * std::log(tfn(phi, e));
      return true;
    }
  }
  return false;
}

// ------------------------------------------------------- projection inverse
bool Crs::projInverse(double x, double y, double& lam, double& phi) const {
  const double e = e_, e2 = e2_;
  switch (kind_) {
    case Kind::LongLat:
      lam = x; phi = y;
      return true;
    case Kind::TMerc: {
      const double xi = (y - y0_) / (k0_ * c_[0]) + c_[1];
      const double eta = (x - x0_) / (k0_ * c_[0]);
      double xip = xi, etap = eta;
      for (int j = 1; j <= 6; ++j) {
        xip -= beta_[j] * std::sin(2 * j * xi) * std::cosh(2 * j * eta);
        etap -= beta_[j] * std::cos(2 * j * xi) * std::sinh(2 * j * eta);
      }
      const double taup = std::sin(xip) / std::hypot(std::sinh(etap), std::cos(xip));
      lam = std::atan2(std::sinh(etap), std::cos(xip));
      double tau = taup;
      for (int i = 0; i < 10; ++i) {
        const double sig = std::sinh(e * std::atanh(e * tau / std::hypot(1.0, tau)));
        const double taui = tau * std::hypot(1.0, sig) - sig * std::hypot(1.0, tau);
        const double dt = (taup - taui) / std::hypot(1.0, taui) * (1 + (1 - e2) * tau * tau) /
                          ((1 - e2) * std::hypot(1.0, tau));
        tau += dt;
        if (std::fabs(dt) < 1e-15) break;
      }
      phi = std::atan(tau);
      break;
    }
    case Kind::Lcc: {
      const double n = c_[0], F = c_[1], r0 = c_[2];
      const double dx = x - x0_, dy = r0 - (y - y0_);
      const double sn = n < 0 ? -1 : 1;
      const double rp = sn * std::hypot(dx, dy);
      const double th = std::atan2(sn * dx, sn * dy);
      lam = th / n;
      if (rp == 0) { phi = sn * M_PI / 2; break; }
      phi = phiFromT(std::pow(rp / (a_ * k0_ * F), 1 / n), e);
      break;
    }
    case Kind::StereA:
    case Kind::StereB: {
      const double s = south_ ? -1 : 1;
      const double dx = x - x0_, dy = y - y0_;
      const double rho = std::hypot(dx, dy);
      const double t = kind_ == Kind::StereA ? rho * c_[0] / (2 * a_ * k0_) : rho * c_[0] / (a_ * c_[1]);
      phi = s * phiFromT(t, e);
      lam = std::atan2(dx, -s * dy);
      break;
    }
    case Kind::Sterea: {
      const double R = c_[0], n = c_[1], cc = c_[2], chi0 = c_[3];
      const double E = x - x0_, N = y - y0_;
      const double g = 2 * R * k0_ * std::tan(M_PI / 4 - chi0 / 2);
      const double h = 4 * R * k0_ * std::tan(chi0) + g;
      const double i = std::atan(E / (h + N));
      const double j = std::atan(E / (g - N)) - i;
      const double chi = chi0 + 2 * std::atan((N - E * std::tan(j / 2)) / (2 * R * k0_));
      const double L = j + 2 * i;
      lam = L / n;
      const double psi = 0.5 * std::log((1 + std::sin(chi)) / (cc * (1 - std::sin(chi)))) / n;
      phi = 2 * std::atan(std::exp(psi)) - M_PI / 2;
      for (int it = 0; it < 30; ++it) {
        const double sp = std::sin(phi);
        const double psii = std::log(std::tan(phi / 2 + M_PI / 4) * std::pow((1 - e * sp) / (1 + e * sp), e / 2));
        const double np = phi - (psii - psi) * std::cos(phi) * (1 - e2 * sp * sp) / (1 - e2);
        if (std::fabs(np - phi) < 1e-14) { phi = np; break; }
        phi = np;
      }
      break;
    }
    case Kind::Somerc: {
      const double c = c_[0], sinp0 = c_[1], cosp0 = c_[2], K = c_[3], kR = c_[4];
      const double X = (x - x0_) / a_, Y = (y - y0_) / a_;
      const double phipp = 2 * (std::atan(std::exp(Y / kR)) - M_PI / 4);
      const double lampp = X / kR;
      const double cp = std::cos(phipp);
      const double phip = std::asin(cosp0 * std::sin(phipp) + sinp0 * cp * std::cos(lampp));
      const double lamp = std::asin(cp * std::sin(lampp) / std::cos(phip));
      const double con = (K - std::log(std::tan(M_PI / 4 + 0.5 * phip))) / c;
      phi = phip;
      for (int i = 0; i < 30; ++i) {
        const double esp = e * std::sin(phi);
        const double delp = (con + std::log(std::tan(M_PI / 4 + 0.5 * phi)) -
                             0.5 * e * std::log((1 + esp) / (1 - esp))) *
                            (1 - esp * esp) * std::cos(phi) / (1 - e2);
        phi -= delp;
        if (std::fabs(delp) < 1e-14) break;
      }
      lam = lamp / c;
      break;
    }
    case Kind::Merc: {
      const double k = c_[0];
      lam = (x - x0_) / (a_ * k);
      phi = phiFromT(std::exp(-(y - y0_) / (a_ * k)), e);
      break;
    }
  }
  lam = normLon(lam + lon0_);
  return std::isfinite(lam) && std::isfinite(phi);
}

// ---------------------------------------------------------------- datum
void Crs::datumToWGS84(double& lam, double& phi) const {
  if (!hasHelmert_) return;
  double X, Y, Z;
  geodToEcef(lam, phi, a_, e2_, X, Y, Z);
  const double s = 1 + tw_[6], rx = tw_[3], ry = tw_[4], rz = tw_[5];
  const double Xo = tw_[0] + s * (X - rz * Y + ry * Z);
  const double Yo = tw_[1] + s * (rz * X + Y - rx * Z);
  const double Zo = tw_[2] + s * (-ry * X + rx * Y + Z);
  ecefToGeod(Xo, Yo, Zo, kWgsA, kWgsF * (2 - kWgsF), lam, phi);
}

void Crs::datumFromWGS84(double& lam, double& phi) const {
  if (!hasHelmert_) return;
  double X, Y, Z;
  geodToEcef(lam, phi, kWgsA, kWgsF * (2 - kWgsF), X, Y, Z);
  const double s = 1 + tw_[6], rx = tw_[3], ry = tw_[4], rz = tw_[5];
  X = (X - tw_[0]) / s; Y = (Y - tw_[1]) / s; Z = (Z - tw_[2]) / s;
  // inverse de la rotation (petits angles : transposée)
  const double Xo = X + rz * Y - ry * Z;
  const double Yo = -rz * X + Y + rx * Z;
  const double Zo = ry * X - rx * Y + Z;
  ecefToGeod(Xo, Yo, Zo, a_, e2_, lam, phi);
}

bool Crs::toWGS84(double x, double y, double& lon, double& lat) const {
  double lam, phi;
  if (isGeographic()) { lam = x * D2R; phi = y * D2R; }
  else if (!projInverse(x * unit_, y * unit_, lam, phi)) return false;
  lam += pm_;
  datumToWGS84(lam, phi);
  lon = normLon(lam) * R2D;
  lat = phi * R2D;
  return std::isfinite(lon) && std::isfinite(lat) && std::fabs(lat) <= 90;
}

bool Crs::fromWGS84(double lon, double lat, double& x, double& y) const {
  double lam = lon * D2R, phi = lat * D2R;
  datumFromWGS84(lam, phi);
  lam -= pm_;
  if (isGeographic()) { x = normLon(lam) * R2D; y = phi * R2D; return true; }
  if (!projForward(lam, phi, x, y)) return false;
  x /= unit_; y /= unit_;
  return std::isfinite(x) && std::isfinite(y);
}

// ------------------------------------------------------------- catalogue
namespace {
const char* kNTF = " +a=6378249.2 +b=6356515 +towgs84=-168,-60,320,0,0,0,0 +pm=paris +units=m";
const char* kED50 = " +ellps=intl +towgs84=-87,-98,-121,0,0,0,0";

std::string Fmt(const char* f, ...) {
  char b[512];
  va_list ap;
  va_start(ap, f);
  std::vsnprintf(b, sizeof b, f, ap);
  va_end(ap);
  return b;
}

const std::map<int, std::string>& EpsgTable() {
  static std::map<int, std::string> t;
  if (!t.empty()) return t;
  t[4326] = "+proj=longlat +datum=WGS84";
  t[4258] = "+proj=longlat +ellps=GRS80";
  t[4171] = "+proj=longlat +ellps=GRS80";
  t[4269] = "+proj=longlat +ellps=GRS80";
  t[4230] = std::string("+proj=longlat") + kED50;
  t[3857] = "+proj=merc +a=6378137 +b=6378137 +lat_ts=0 +lon_0=0 +x_0=0 +y_0=0 +k=1 +units=m";
  t[3395] = "+proj=merc +lon_0=0 +k=1 +x_0=0 +y_0=0 +datum=WGS84 +units=m";
  t[32661] = "+proj=stere +lat_0=90 +lat_ts=90 +lon_0=0 +k=0.994 +x_0=2000000 +y_0=2000000 +datum=WGS84";
  t[32761] = "+proj=stere +lat_0=-90 +lat_ts=-90 +lon_0=0 +k=0.994 +x_0=2000000 +y_0=2000000 +datum=WGS84";
  t[3995] = "+proj=stere +lat_0=90 +lat_ts=71 +lon_0=0 +x_0=0 +y_0=0 +datum=WGS84";
  t[3413] = "+proj=stere +lat_0=90 +lat_ts=70 +lon_0=-45 +x_0=0 +y_0=0 +datum=WGS84";
  t[3031] = "+proj=stere +lat_0=-90 +lat_ts=-71 +lon_0=0 +x_0=0 +y_0=0 +datum=WGS84";
  // France métropolitaine
  t[2154] = "+proj=lcc +lat_0=46.5 +lon_0=3 +lat_1=49 +lat_2=44 +x_0=700000 +y_0=6600000 +ellps=GRS80 +units=m";
  for (int z = 42; z <= 50; ++z)
    t[3900 + z] = Fmt("+proj=lcc +lat_0=%d +lon_0=3 +lat_1=%.2f +lat_2=%.2f +x_0=1700000 +y_0=%d +ellps=GRS80 +units=m",
                      z, z - 0.75, z + 0.75, (z - 41) * 1000000 + 200000);
  t[27561] = std::string("+proj=lcc +lat_1=49.5 +lat_0=49.5 +lon_0=0 +k_0=0.999877341 +x_0=600000 +y_0=200000") + kNTF;
  t[27562] = std::string("+proj=lcc +lat_1=46.8 +lat_0=46.8 +lon_0=0 +k_0=0.99987742 +x_0=600000 +y_0=200000") + kNTF;
  t[27563] = std::string("+proj=lcc +lat_1=44.1 +lat_0=44.1 +lon_0=0 +k_0=0.999877499 +x_0=600000 +y_0=200000") + kNTF;
  t[27564] = std::string("+proj=lcc +lat_1=42.165 +lat_0=42.165 +lon_0=0 +k_0=0.99994471 +x_0=234.358 +y_0=185861.369") + kNTF;
  t[27571] = std::string("+proj=lcc +lat_1=49.5 +lat_0=49.5 +lon_0=0 +k_0=0.999877341 +x_0=600000 +y_0=1200000") + kNTF;
  t[27572] = std::string("+proj=lcc +lat_1=46.8 +lat_0=46.8 +lon_0=0 +k_0=0.99987742 +x_0=600000 +y_0=2200000") + kNTF;
  t[27573] = std::string("+proj=lcc +lat_1=44.1 +lat_0=44.1 +lon_0=0 +k_0=0.999877499 +x_0=600000 +y_0=3200000") + kNTF;
  t[27574] = std::string("+proj=lcc +lat_1=42.165 +lat_0=42.165 +lon_0=0 +k_0=0.99994471 +x_0=234.358 +y_0=4185861.369") + kNTF;
  // France outre-mer (repères ITRF, ellipsoïde GRS80)
  t[5490] = "+proj=utm +zone=20 +ellps=GRS80";
  t[2972] = "+proj=utm +zone=22 +ellps=GRS80";
  t[2975] = "+proj=utm +zone=40 +south +ellps=GRS80";
  t[4471] = "+proj=utm +zone=38 +south +ellps=GRS80";
  t[4467] = "+proj=utm +zone=21 +ellps=GRS80";
  t[3163] = "+proj=lcc +lat_0=-21.5 +lon_0=166 +lat_1=-20.6666666666667 +lat_2=-22.3333333333333 +x_0=400000 +y_0=300000 +ellps=GRS80";
  // Europe
  t[3034] = "+proj=lcc +lat_0=52 +lon_0=10 +lat_1=35 +lat_2=65 +x_0=4000000 +y_0=2800000 +ellps=GRS80";
  t[27700] = "+proj=tmerc +lat_0=49 +lon_0=-2 +k=0.9996012717 +x_0=400000 +y_0=-100000 +ellps=airy +towgs84=446.448,-125.157,542.06,0.15,0.247,0.842,-20.489";
  t[2157] = "+proj=tmerc +lat_0=53.5 +lon_0=-8 +k=0.99982 +x_0=600000 +y_0=750000 +ellps=GRS80";
  t[28992] = "+proj=sterea +lat_0=52.1561605555556 +lon_0=5.38763888888889 +k=0.9999079 +x_0=155000 +y_0=463000 +ellps=bessel +towgs84=565.4171,50.3319,465.5524,1.9342,-1.6677,9.1019,4.0725";
  t[31370] = "+proj=lcc +lat_0=90 +lon_0=4.36748666666667 +lat_1=51.1666672333333 +lat_2=49.8333339 +x_0=150000.013 +y_0=5400088.438 +ellps=intl +towgs84=-106.8686,52.2978,-103.7239,0.3366,-0.457,1.8422,-1.2747";
  t[3812] = "+proj=lcc +lat_0=50.797815 +lon_0=4.35921583333333 +lat_1=49.8333333333333 +lat_2=51.1666666666667 +x_0=649328 +y_0=665262 +ellps=GRS80";
  t[2169] = "+proj=tmerc +lat_0=49.8333333333333 +lon_0=6.16666666666667 +k=1 +x_0=80000 +y_0=100000 +ellps=intl +towgs84=-189.681,18.3463,-42.7695,-0.33746,-3.09264,2.53861,0.4598";
  t[2056] = "+proj=somerc +lat_0=46.9524055555556 +lon_0=7.43958333333333 +k_0=1 +x_0=2600000 +y_0=1200000 +ellps=bessel +towgs84=674.374,15.056,405.346,0,0,0,0";
  t[21781] = "+proj=somerc +lat_0=46.9524055555556 +lon_0=7.43958333333333 +k_0=1 +x_0=600000 +y_0=200000 +ellps=bessel +towgs84=674.374,15.056,405.346,0,0,0,0";
  for (int z = 2; z <= 5; ++z)
    t[31464 + z] = Fmt("+proj=tmerc +lat_0=0 +lon_0=%d +k=1 +x_0=%d +y_0=0 +ellps=bessel +towgs84=598.1,73.7,418.2,0.202,0.045,-2.455,6.7",
                       3 * z, z * 1000000 + 500000);
  t[3763] = "+proj=tmerc +lat_0=39.6682583333333 +lon_0=-8.13310833333333 +k=1 +x_0=0 +y_0=0 +ellps=GRS80";
  t[3003] = "+proj=tmerc +lat_0=0 +lon_0=9 +k=0.9996 +x_0=1500000 +y_0=0 +ellps=intl +towgs84=-104.1,-49.1,-9.9,0.971,-2.917,0.714,-11.68";
  t[3004] = "+proj=tmerc +lat_0=0 +lon_0=15 +k=0.9996 +x_0=2520000 +y_0=0 +ellps=intl +towgs84=-104.1,-49.1,-9.9,0.971,-2.917,0.714,-11.68";
  t[3006] = "+proj=utm +zone=33 +ellps=GRS80";
  t[3067] = "+proj=utm +zone=35 +ellps=GRS80";
  t[2180] = "+proj=tmerc +lat_0=0 +lon_0=19 +k=0.9993 +x_0=500000 +y_0=-5300000 +ellps=GRS80";
  t[3765] = "+proj=tmerc +lat_0=0 +lon_0=16.5 +k=0.9999 +x_0=500000 +y_0=0 +ellps=GRS80";
  t[2100] = "+proj=tmerc +lat_0=0 +lon_0=24 +k=0.9996 +x_0=500000 +y_0=0 +ellps=GRS80 +towgs84=-199.87,74.79,246.62,0,0,0,0";
  t[3301] = "+proj=lcc +lat_0=57.5175539305556 +lon_0=24 +lat_1=59.3333333333333 +lat_2=58 +x_0=500000 +y_0=6375000 +ellps=GRS80";
  t[3059] = "+proj=tmerc +lat_0=0 +lon_0=24 +k=0.9996 +x_0=500000 +y_0=-6000000 +ellps=GRS80";
  t[3346] = "+proj=tmerc +lat_0=0 +lon_0=24 +k=0.9998 +x_0=500000 +y_0=0 +ellps=GRS80";
  // Océanie
  t[2193] = "+proj=tmerc +lat_0=0 +lon_0=173 +k=0.9996 +x_0=1600000 +y_0=10000000 +ellps=GRS80";
  return t;
}
}  // namespace

std::string EpsgToProj(int code) {
  auto& t = EpsgTable();
  auto it = t.find(code);
  if (it != t.end()) return it->second;
  auto utm = [](int z, bool s, const char* rest) {
    return Fmt("+proj=utm +zone=%d%s%s", z, s ? " +south" : "", rest);
  };
  if (code >= 32601 && code <= 32660) return utm(code - 32600, false, " +datum=WGS84");
  if (code >= 32701 && code <= 32760) return utm(code - 32700, true, " +datum=WGS84");
  if (code >= 25828 && code <= 25838) return utm(code - 25800, false, " +ellps=GRS80");
  if (code >= 23028 && code <= 23038) return utm(code - 23000, false, kED50);
  if (code >= 26901 && code <= 26923) return utm(code - 26900, false, " +ellps=GRS80");
  if (code >= 26703 && code <= 26722) return utm(code - 26700, false, " +ellps=clrk66 +towgs84=-8,160,176,0,0,0,0");
  if (code >= 28348 && code <= 28358) return utm(code - 28300, true, " +ellps=GRS80");
  if (code >= 7846 && code <= 7859) return utm(code - 7800, true, " +ellps=GRS80");
  if (code >= 31965 && code <= 31976) return utm(code - 31954, false, " +ellps=GRS80");
  if (code >= 31977 && code <= 31985) return utm(code - 31960, true, " +ellps=GRS80");
  if (code >= 6707 && code <= 6709) return utm(code - 6675, false, " +ellps=GRS80");  // RDN2008 UTM 32-34
  return "";
}

const std::vector<CrsEntry>& CrsCatalog() {
  static std::vector<CrsEntry> c;
  if (!c.empty()) return c;
  auto E = [&](const char* code, const char* g, const char* n) { c.push_back({code, g, n, 0, 0, false}); };
  auto Z = [&](const char* code, const char* g, const char* n, int z0, int z1, bool s) {
    c.push_back({code, g, n, z0, z1, s});
  };
  E("EPSG:4326", "Monde", "WGS84 — longitude / latitude (GPS)");
  Z("UTM-WGS84", "Monde", "UTM WGS84 — fuseau et hémisphère", 1, 60, false);
  E("EPSG:3857", "Monde", "Web Mercator (Google, OSM)");
  E("EPSG:3395", "Monde", "Mercator WGS84 (World Mercator)");
  E("EPSG:32661", "Monde", "UPS Nord (stéréographique polaire)");
  E("EPSG:32761", "Monde", "UPS Sud (stéréographique polaire)");
  E("EPSG:3995", "Monde", "Arctique stéréographique polaire");
  E("EPSG:3413", "Monde", "NSIDC Arctique stéréographique polaire");
  E("EPSG:3031", "Monde", "Antarctique stéréographique polaire");
  E("EPSG:2154", "France", "RGF93 / Lambert-93");
  for (int z = 42; z <= 50; ++z)
    c.push_back({Fmt("EPSG:%d", 3900 + z), "France", Fmt("RGF93 / CC%d (conique conforme)", z), 0, 0, false});
  E("EPSG:4171", "France", "RGF93 — longitude / latitude");
  E("EPSG:27572", "France", "NTF / Lambert II étendu");
  E("EPSG:27571", "France", "NTF / Lambert I carto");
  E("EPSG:27573", "France", "NTF / Lambert III carto");
  E("EPSG:27574", "France", "NTF / Lambert IV carto (Corse)");
  E("EPSG:27561", "France", "NTF / Lambert Nord France");
  E("EPSG:27562", "France", "NTF / Lambert Centre France");
  E("EPSG:27563", "France", "NTF / Lambert Sud France");
  E("EPSG:27564", "France", "NTF / Lambert Corse");
  E("EPSG:5490", "France outre-mer", "Antilles — RGAF09 / UTM 20N");
  E("EPSG:2972", "France outre-mer", "Guyane — RGFG95 / UTM 22N");
  E("EPSG:2975", "France outre-mer", "La Réunion — RGR92 / UTM 40S");
  E("EPSG:4471", "France outre-mer", "Mayotte — RGM04 / UTM 38S");
  E("EPSG:4467", "France outre-mer", "Saint-Pierre-et-Miquelon — RGSPM06 / UTM 21N");
  E("EPSG:3163", "France outre-mer", "Nouvelle-Calédonie — RGNC91-93 / Lambert NC");
  Z("UTM-RGPF", "France outre-mer", "Polynésie — RGPF / UTM (fuseau 5 à 7 S)", 5, 7, true);
  Z("UTM-ETRS89", "Europe", "ETRS89 / UTM — fuseau (Espagne, Italie, Norvège…)", 28, 38, false);
  Z("UTM-ED50", "Europe", "ED50 / UTM — fuseau (anciennes cartes)", 28, 38, false);
  E("EPSG:4258", "Europe", "ETRS89 — longitude / latitude");
  E("EPSG:4230", "Europe", "ED50 — longitude / latitude");
  E("EPSG:3034", "Europe", "ETRS89 / Lambert conique Europe (LCC)");
  E("EPSG:27700", "Europe", "Royaume-Uni — OSGB36 / British National Grid");
  E("EPSG:2157", "Europe", "Irlande — IRENET95 / Irish Transverse Mercator");
  E("EPSG:28992", "Europe", "Pays-Bas — Amersfoort / RD New");
  E("EPSG:31370", "Europe", "Belgique — BD72 / Lambert belge 1972");
  E("EPSG:3812", "Europe", "Belgique — ETRS89 / Lambert belge 2008");
  E("EPSG:2169", "Europe", "Luxembourg — LUREF / Gauss");
  E("EPSG:2056", "Europe", "Suisse — CH1903+ / LV95");
  E("EPSG:21781", "Europe", "Suisse — CH1903 / LV03");
  E("EPSG:31466", "Europe", "Allemagne — DHDN / Gauss-Krüger zone 2");
  E("EPSG:31467", "Europe", "Allemagne — DHDN / Gauss-Krüger zone 3");
  E("EPSG:31468", "Europe", "Allemagne — DHDN / Gauss-Krüger zone 4");
  E("EPSG:31469", "Europe", "Allemagne — DHDN / Gauss-Krüger zone 5");
  E("EPSG:3763", "Europe", "Portugal — ETRS89 / PT-TM06");
  E("EPSG:3003", "Europe", "Italie — Monte Mario / Gauss-Boaga Ouest");
  E("EPSG:3004", "Europe", "Italie — Monte Mario / Gauss-Boaga Est");
  E("EPSG:3006", "Europe", "Suède — SWEREF99 TM");
  E("EPSG:3067", "Europe", "Finlande — ETRS-TM35FIN");
  E("EPSG:2180", "Europe", "Pologne — ETRS89 / CS92");
  E("EPSG:3765", "Europe", "Croatie — HTRS96 / TM");
  E("EPSG:2100", "Europe", "Grèce — GGRS87 / Greek Grid");
  E("EPSG:3301", "Europe", "Estonie — L-EST97");
  E("EPSG:3059", "Europe", "Lettonie — LKS92 / TM");
  E("EPSG:3346", "Europe", "Lituanie — LKS94 / TM");
  Z("UTM-NAD83", "Amériques", "NAD83 / UTM — fuseau (Amérique du Nord)", 1, 23, false);
  Z("UTM-NAD27", "Amériques", "NAD27 / UTM — fuseau (anciennes cartes)", 1, 22, false);
  Z("UTM-SIRGAS", "Amériques", "SIRGAS 2000 / UTM — fuseau (Amérique du Sud)", 11, 25, true);
  Z("UTM-GDA2020", "Océanie", "Australie — GDA2020 / MGA — fuseau", 46, 59, true);
  Z("UTM-GDA94", "Océanie", "Australie — GDA94 / MGA — fuseau", 48, 58, true);
  E("EPSG:2193", "Océanie", "Nouvelle-Zélande — NZGD2000 / NZTM");
  return c;
}

std::string CrsDefinition(const CrsEntry& e, int zone, bool south) {
  if (e.zoneMin == 0) return EpsgToProj(std::atoi(e.code.c_str() + 5));
  zone = std::max(e.zoneMin, std::min(e.zoneMax, zone));
  const char* rest = " +ellps=GRS80";
  if (e.code == "UTM-WGS84") rest = " +datum=WGS84";
  else if (e.code == "UTM-ED50") rest = kED50;
  else if (e.code == "UTM-NAD27") rest = " +ellps=clrk66 +towgs84=-8,160,176,0,0,0,0";
  return Fmt("+proj=utm +zone=%d%s%s", zone, south ? " +south" : "", rest);
}

int EpsgFromWkt(const std::string& wkt) {
  std::smatch m;
  // Autorité EPSG explicite (la dernière est celle du système complet)
  std::regex auth(R"re((?:AUTHORITY|ID)\s*\[\s*"EPSG"\s*,\s*"?(\d+)"?\s*\])re", std::regex::icase);
  int code = 0;
  for (auto it = std::sregex_iterator(wkt.begin(), wkt.end(), auth); it != std::sregex_iterator(); ++it)
    code = std::atoi((*it)[1].str().c_str());
  if (code && (!EpsgToProj(code).empty())) return code;
  // Nom du système (fichiers .prj ESRI sans autorité)
  std::string u;
  for (char ch : wkt) if (std::isalnum((unsigned char)ch)) u += (char)std::toupper((unsigned char)ch);
  const bool proj = u.find("PROJCS") != std::string::npos || u.find("PROJCRS") != std::string::npos;
  if (!proj) {
    if (u.find("WGS84") != std::string::npos || u.find("WGS1984") != std::string::npos) return 4326;
    if (u.find("ETRS") != std::string::npos || u.find("RGF93") != std::string::npos) return 4258;
    return 0;
  }
  if (u.find("LAMBERT93") != std::string::npos) return 2154;
  std::regex cc(R"(RGF93(?:V2B?)?CC(4[2-9]|50))");
  if (std::regex_search(u, m, cc)) return 3900 + std::atoi(m[1].str().c_str());
  if (u.find("LAMBERTIIETENDU") != std::string::npos || u.find("LAMBERTIIEXTENDED") != std::string::npos ||
      u.find("LAMBERT2ETENDU") != std::string::npos)
    return 27572;
  if (u.find("BRITISHNATIONALGRID") != std::string::npos) return 27700;
  if (u.find("RDNEW") != std::string::npos) return 28992;
  if (u.find("LAMBERT1972") != std::string::npos || u.find("LAMBERT72") != std::string::npos) return 31370;
  if (u.find("LAMBERT2008") != std::string::npos) return 3812;
  if (u.find("LV95") != std::string::npos) return 2056;
  if (u.find("LV03") != std::string::npos) return 21781;
  if (u.find("IRISHTRANSVERSEMERCATOR") != std::string::npos) return 2157;
  if (u.find("SWEREF99TM") != std::string::npos) return 3006;
  if (u.find("PSEUDOMERCATOR") != std::string::npos || u.find("WEBMERCATOR") != std::string::npos) return 3857;
  std::regex utm(R"(UTMZONE(\d{1,2})([NS]))");
  if (std::regex_search(u, m, utm)) {
    const int z = std::atoi(m[1].str().c_str());
    const bool s = m[2].str() == "S";
    if (u.find("ED50") != std::string::npos || u.find("EUROPEAN1950") != std::string::npos) return s ? 0 : 23000 + z;
    if (u.find("NAD27") != std::string::npos || u.find("NORTHAMERICAN1927") != std::string::npos) return s ? 0 : 26700 + z;
    if (u.find("NAD83") != std::string::npos || u.find("NORTHAMERICAN1983") != std::string::npos) return s ? 0 : 26900 + z;
    if (u.find("ETRS") != std::string::npos) return s ? 0 : 25800 + z;
    // WGS84 et repères ITRF (RGF93, RGAF09, SIRGAS, GDA2020…) : équivalents à mieux que 1 m
    return (s ? 32700 : 32600) + z;
  }
  return 0;
}

std::string GuessCrsHint(double x, double y) {
  char b[200];
  if (x > 50000 && x < 1300000 && y > 6000000 && y < 7200000)
    return "les coordonnées ressemblent à du Lambert-93 (EPSG:2154)";
  if (x > 50000 && x < 1250000 && y > 1600000 && y < 2800000)
    return "les coordonnées ressemblent à du Lambert II étendu (EPSG:27572)";
  if (x > 1000000 && x < 2400000 && y > 1000000 && y < 9400000) {
    int z = (int)std::lround((y - 200000) / 1000000.0) + 41;
    if (z >= 42 && z <= 50) {
      std::snprintf(b, sizeof b, "les coordonnées ressemblent à une zone conique conforme (CC%d, EPSG:%d)", z, 3900 + z);
      return b;
    }
  }
  if (x > 160000 && x < 840000 && y > 0 && y < 10000000)
    return "les coordonnées ressemblent à de l'UTM (choisir le fuseau : 30, 31 ou 32 N en France métropolitaine)";
  return "coordonnées projetées : choisir le système de coordonnées du fichier";
}

}  // namespace pd

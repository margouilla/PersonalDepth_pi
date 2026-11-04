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

#include "ColorRamp.h"
#include "TideField.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <sstream>

namespace pd {

void ColorRamp::sort() {
  std::stable_sort(stops.begin(), stops.end(),
                   [](const ColorStop& a, const ColorStop& b) { return a.depth < b.depth; });
}

static uint32_t lerp(uint32_t a, uint32_t b, float t) {
  auto L = [t](int x, int y) { return (int)std::lround(x + (y - x) * t); };
  return RGBA(L(R_(a), R_(b)), L(G_(a), G_(b)), L(B_(a), B_(b)), L(A_(a), A_(b)));
}

uint32_t ColorRamp::colorAt(float d) const {
  if (stops.empty() || std::isnan(d)) return 0;
  uint32_t c;
  if (d <= stops.front().depth) c = stops.front().rgba;
  else if (d >= stops.back().depth) c = stops.back().rgba;
  else {
    size_t k = 0;
    while (k + 1 < stops.size() && stops[k + 1].depth <= d) ++k;
    if (mode == Mode::Stepped) c = stops[k].rgba;  // [seuil_k, seuil_k+1[
    else {
      const ColorStop &a = stops[k], &b = stops[k + 1];
      c = lerp(a.rgba, b.rgba, (d - a.depth) / (b.depth - a.depth));
    }
  }
  if (safetyOn && d < safetyDepth) {
    float al = A_(safetyColor) / 255.f;
    uint32_t s = safetyColor | 0xFF000000u;
    uint32_t m = lerp(c, s, al);
    c = (m & 0x00FFFFFFu) | (c & 0xFF000000u);
  }
  return c;
}

static std::string hex(uint32_t c) {
  char b[16];
  std::snprintf(b, sizeof b, "#%02x%02x%02x%02x", R_(c), G_(c), B_(c), A_(c));
  return b;
}
static bool parseHex(const std::string& s, uint32_t& c) {
  unsigned r, g, b, a = 255;
  if (s.size() >= 9 && std::sscanf(s.c_str(), "#%2x%2x%2x%2x", &r, &g, &b, &a) == 4) {}
  else if (std::sscanf(s.c_str(), "#%2x%2x%2x", &r, &g, &b) != 3) return false;
  c = RGBA(r, g, b, a);
  return true;
}

std::string ColorRamp::serialize() const {
  std::ostringstream o;
  char z[32];
  o << "mode=" << (mode == Mode::Stepped ? 'S' : 'C');
  std::snprintf(z, sizeof z, "%.3f", safetyDepth);
  o << ";safe=" << (safetyOn ? 1 : 0) << ',' << z << ',' << hex(safetyColor) << ";stops=";
  for (size_t i = 0; i < stops.size(); ++i) {
    std::snprintf(z, sizeof z, "%.3f", stops[i].depth);
    o << (i ? "|" : "") << z << ':' << hex(stops[i].rgba);
  }
  return o.str();
}

bool ColorRamp::deserialize(const std::string& s) {
  ColorRamp r;
  r.stops.clear();
  std::istringstream in(s);
  std::string part;
  while (std::getline(in, part, ';')) {
    size_t eq = part.find('=');
    if (eq == std::string::npos) continue;
    std::string k = part.substr(0, eq), v = part.substr(eq + 1);
    if (k == "mode") r.mode = (v == "C") ? Mode::Continuous : Mode::Stepped;
    else if (k == "safe") {
      int on = 0; char col[16] = {0}; float d = 2;
      if (std::sscanf(v.c_str(), "%d,%f,%15s", &on, &d, col) >= 2) {
        r.safetyOn = on != 0; r.safetyDepth = d;
        uint32_t c; if (parseHex(col, c)) r.safetyColor = c;
      }
    } else if (k == "stops") {
      std::istringstream sv(v); std::string st;
      while (std::getline(sv, st, '|')) {
        size_t c = st.find(':'); uint32_t col;
        if (c == std::string::npos || !parseHex(st.substr(c + 1), col)) continue;
        r.stops.push_back({std::strtof(st.c_str(), nullptr), col});
      }
    }
  }
  if (r.stops.empty()) return false;
  r.sort();
  *this = r;
  return true;
}

// GMT .cpt : "z0 r g b z1 r g b" ou "z0 r/g/b z1 r/g/b"
bool ColorRamp::loadCPT(const std::string& path, std::string& err) {
  std::ifstream in(path);
  if (!in) { err = "Impossible d'ouvrir " + path; return false; }
  std::vector<ColorStop> st;
  bool allFlat = true;
  std::string line;
  while (std::getline(in, line)) {
    if (line.empty() || line[0] == '#' || line[0] == 'B' || line[0] == 'F' || line[0] == 'N')
      continue;
    for (char& ch : line) if (ch == '/') ch = ' ';
    std::istringstream l(line);
    float z0, z1; int r0, g0, b0, r1, g1, b1;
    if (!(l >> z0 >> r0 >> g0 >> b0 >> z1 >> r1 >> g1 >> b1)) continue;
    uint32_t c0 = RGBA(r0, g0, b0), c1 = RGBA(r1, g1, b1);
    if (c0 != c1) allFlat = false;
    if (!st.empty() && st.back().depth == z0) st.back().rgba = c0;  // début de segment
    else st.push_back({z0, c0});
    st.push_back({z1, c1});
  }
  if (st.size() < 2) { err = "Palette CPT vide"; return false; }
  mode = allFlat ? Mode::Stepped : Mode::Continuous;
  if (mode == Mode::Stepped) st.pop_back();  // la borne haute ne porte pas de couleur
  stops = st;
  sort();
  return true;
}

bool ColorRamp::saveCPT(const std::string& path) const {
  std::ofstream o(path);
  if (!o || stops.size() < 2) return false;
  o << "# PersonalDepth_pi - " << (mode == Mode::Stepped ? "paliers" : "continu") << "\n";
  for (size_t i = 0; i + 1 < stops.size(); ++i) {
    uint32_t a = stops[i].rgba, b = mode == Mode::Stepped ? a : stops[i + 1].rgba;
    o << stops[i].depth << ' ' << R_(a) << '/' << G_(a) << '/' << B_(a) << ' '
      << stops[i + 1].depth << ' ' << R_(b) << '/' << G_(b) << '/' << B_(b) << "\n";
  }
  if (mode == Mode::Stepped) {  // dernier palier ouvert vers le bas
    uint32_t a = stops.back().rgba;
    o << stops.back().depth << ' ' << R_(a) << '/' << G_(a) << '/' << B_(a) << ' '
      << stops.back().depth + 11000 << ' ' << R_(a) << '/' << G_(a) << '/' << B_(a) << "\n";
  }
  return (bool)o;
}

std::vector<std::string> ColorRamp::PresetNames() {
  return {"TimeZero PBG (bleus)", "Carte marine (paliers)", "Arc-en-ciel",
          "Sécurité tirant d'eau", "Niveaux de gris"};
}

ColorRamp ColorRamp::Preset(const std::string& n) {
  ColorRamp r;
  auto S = [&](float d, int R, int G, int B) { r.stops.push_back({d, RGBA(R, G, B)}); };
  if (n == "Carte marine (paliers)") {
    r.mode = Mode::Stepped;
    S(-10, 165, 190, 120); S(0, 110, 170, 230); S(2, 150, 200, 240);
    S(5, 195, 225, 250); S(10, 225, 240, 255); S(20, 250, 252, 255);
  } else if (n == "Arc-en-ciel") {
    r.mode = Mode::Continuous;
    S(0, 220, 30, 30); S(5, 250, 140, 30); S(10, 250, 230, 50); S(20, 60, 200, 80);
    S(40, 40, 200, 220); S(80, 40, 90, 220); S(200, 110, 40, 170);
  } else if (n == "Sécurité tirant d'eau") {
    r.mode = Mode::Stepped;
    S(-10, 180, 160, 110); S(0, 230, 60, 50); S(2, 250, 170, 60); S(3, 250, 230, 120);
    S(5, 170, 215, 245); S(10, 110, 170, 230); S(20, 60, 120, 200);
    r.safetyOn = true; r.safetyDepth = 2.5f;
  } else if (n == "Niveaux de gris") {
    r.mode = Mode::Continuous;
    S(0, 235, 235, 235); S(100, 40, 40, 40);
  } else {  // TimeZero PBG (bleus)
    r.mode = Mode::Continuous;
    S(-5, 170, 150, 90); S(0, 190, 235, 255); S(5, 120, 200, 240); S(10, 70, 160, 225);
    S(20, 40, 120, 200); S(50, 20, 80, 170); S(100, 10, 50, 130); S(500, 5, 20, 80);
  }
  return r;
}

std::vector<float> AutoThresholds(float zMin, float zMax, int n, bool nice) {
  n = std::max(2, std::min(64, n));
  if (!(zMax > zMin)) zMax = zMin + 1.f;
  std::vector<float> t;
  if (!nice) {
    for (int k = 0; k < n; ++k) t.push_back(zMin + (zMax - zMin) * k / (n - 1));
    return t;
  }
  // plus petit pas « rond » donnant au plus n seuils
  const double raw = (zMax - zMin) / (double)(n - 1);
  double step = 0;
  const double e = std::pow(10.0, std::floor(std::log10(raw)));
  for (double m : {1.0, 2.0, 2.5, 5.0, 10.0}) {
    step = m * e;
    const double first = std::floor(zMin / step) * step;
    if ((int)std::floor((zMax - first) / step + 1e-6) + 1 <= n) break;
  }
  const double first = std::floor(zMin / step + 1e-9) * step;
  for (double v = first; v <= zMax + 1e-6 * step && (int)t.size() < 64; v += step)
    t.push_back((float)(std::round(v / step) * step));
  if (t.size() < 2) t.push_back((float)(first + step));
  return t;
}

// Couleur à la fraction f (0..1) le long d'une suite de seuils (interpolée)
static uint32_t ColorAlong(const std::vector<ColorStop>& st, double f) {
  if (st.empty()) return RGBA(128, 128, 128);
  if (st.size() == 1) return st[0].rgba;
  const double pos = std::max(0.0, std::min(1.0, f)) * (st.size() - 1);
  const size_t i = std::min(st.size() - 2, (size_t)pos);
  const double w = pos - i;
  const uint32_t a = st[i].rgba, b = st[i + 1].rgba;
  auto mix = [w](int x, int y) { return (int)std::lround(x + (y - x) * w); };
  return RGBA(mix(R_(a), R_(b)), mix(G_(a), G_(b)), mix(B_(a), B_(b)), mix(A_(a), A_(b)));
}

ColorRamp AutoRamp(const ColorRamp& colors, const ColorRamp& keep, float zMin, float zMax,
                   int n, bool nice) {
  ColorRamp r = keep;
  r.stops.clear();
  std::vector<ColorStop> land, sea;
  for (const auto& c : colors.stops) (c.depth < 0 ? land : sea).push_back(c);
  if (sea.empty()) { sea = land; land.clear(); }
  const auto th = AutoThresholds(zMin, zMax, n, nice);
  std::vector<float> neg, pos;
  for (float t : th) (t < 0 && !land.empty() ? neg : pos).push_back(t);
  for (size_t k = 0; k < neg.size(); ++k)
    r.stops.push_back({neg[k], ColorAlong(land, neg.size() > 1 ? (double)k / (neg.size() - 1) : 0.0)});
  for (size_t k = 0; k < pos.size(); ++k)
    r.stops.push_back({pos[k], ColorAlong(sea, pos.size() > 1 ? (double)k / (pos.size() - 1) : 0.0)});
  r.sort();
  return r;
}

void ColorLUT::build(const ColorRamp& r, float zMin, float zMax, int n, float offset) {
  if (!(zMax > zMin)) zMax = zMin + 1.f;
  z0 = zMin;
  scale = n / (zMax - zMin);
  t.resize(n);
  for (int k = 0; k < n; ++k) t[k] = r.colorAt(zMin + (k + 0.5f) / scale + offset);
}

void Colorize(const GridLevel& g, int i0, int j0, int w, int h,
              const ColorLUT& lut, float k, uint32_t* out, int stride, const TideField* tide) {
  const bool tg = tide && tide->gridded();
  const float t0 = tide ? tide->h0 : 0.f;
  const bool sh = k > 0.f && g.shade.size() == g.z.size();
  for (int y = 0; y < h; ++y) {
    int j = j0 + y;
    uint32_t* o = out + (size_t)y * stride;
    if (j < 0 || j >= g.ny) { std::fill(o, o + w, 0u); continue; }
    const float* zr = &g.z[(size_t)j * g.nx];
    const uint8_t* sr = sh ? &g.shade[(size_t)j * g.nx] : nullptr;
    for (int x = 0; x < w; ++x) {
      int i = i0 + x;
      if (i < 0 || i >= g.nx || std::isnan(zr[i])) { o[x] = 0; continue; }
      const float hz = tg ? tide->at(g.lonW + (i + 0.5) * g.dlon, g.latN - (j + 0.5) * g.dlat) : t0;
      uint32_t c = lut(zr[i] + hz);
      if (sh) {
        float f = 1.f + k * (sr[i] * (1.f / 160.f) - 1.f);
        auto M = [f](int v) { int q = (int)(v * f); return q > 255 ? 255 : q; };
        c = RGBA(M(R_(c)), M(G_(c)), M(B_(c)), A_(c));
      }
      o[x] = c;
    }
  }
}

}  // namespace pd

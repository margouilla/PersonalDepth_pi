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

#include "BathyRenderer.h"

#include <chrono>

#include <unordered_map>
#include "core/Lang.h"

#include <wx/dcmemory.h>
#include <wx/image.h>

#include <algorithm>
#include <climits>
#include <cmath>
#include <cstdint>

#ifdef __WXOSX__
#include <OpenGL/gl.h>
#include <dlfcn.h>
#else
#ifdef _WIN32
#include <windows.h>
#endif
#include <GL/gl.h>
#ifndef _WIN32
#include <GL/glx.h>
#endif
#endif
#ifndef GL_CLAMP_TO_EDGE
#define GL_CLAMP_TO_EDGE 0x812F
#endif
#ifndef GL_CURRENT_PROGRAM
#define GL_CURRENT_PROGRAM 0x8B8D
#endif
#ifndef GL_ACTIVE_TEXTURE
#define GL_ACTIVE_TEXTURE 0x84E0
#endif
#ifndef GL_TEXTURE0
#define GL_TEXTURE0 0x84C0
#endif
#ifndef GL_ARRAY_BUFFER
#define GL_ARRAY_BUFFER 0x8892
#endif
#ifndef GL_ARRAY_BUFFER_BINDING
#define GL_ARRAY_BUFFER_BINDING 0x8894
#endif
#ifndef APIENTRY
#define APIENTRY
#endif
#ifndef GL_FRAGMENT_SHADER
#define GL_FRAGMENT_SHADER 0x8B30
#define GL_VERTEX_SHADER 0x8B31
#define GL_COMPILE_STATUS 0x8B81
#define GL_LINK_STATUS 0x8B82
#endif
#ifndef GL_TEXTURE1
#define GL_TEXTURE1 0x84C1
#endif
#ifndef GL_TEXTURE2
#define GL_TEXTURE2 0x84C2
#endif
#ifndef GL_LUMINANCE16
#define GL_LUMINANCE16 0x8042
#endif

#include "PersonalDepth_pi.h"

using namespace pd;

static bool IsSafety(const DisplaySettings& s, float level);
// Champ de marée courant (nul si aucun)
static const TideField& TideOf(const DisplaySettings& s) {
  static const TideField zero;
  return s.tide ? *s.tide : zero;
}
static bool IsMajor(const DisplaySettings& s, float level);

// --------------------------------------------------------------------------
// OpenCPN >= 5.8 dessine avec des shaders GLSL et peut laisser un programme,
// un VBO ou une autre unité de texture actifs. Le rendu du plugin (pipeline
// fixe) exige programme 0, unité 0, aucun VBO et une projection en pixels :
// on sauvegarde l'état de l'hôte, on l'impose, puis on restaure tout.
namespace {
typedef void(APIENTRY* PFN_UseProgram)(GLuint);
typedef void(APIENTRY* PFN_ActiveTexture)(GLenum);
typedef void(APIENTRY* PFN_BindBuffer)(GLenum, GLuint);

void* GLProc(const char* name) {
#ifdef _WIN32
  return (void*)wglGetProcAddress(name);
#elif defined(__WXOSX__)
  // macOS : fonctions OpenGL 2.x exportées directement par OpenGL.framework
  return dlsym(RTLD_DEFAULT, name);
#else
  return (void*)glXGetProcAddressARB((const GLubyte*)name);
#endif
}

struct GLHostState {
  PFN_UseProgram useProgram = nullptr;
  PFN_ActiveTexture activeTexture = nullptr;
  PFN_BindBuffer bindBuffer = nullptr;
  GLint program = 0, activeTex = 0, arrayBuffer = 0;

  explicit GLHostState(int w, int h) {
    static PFN_UseProgram up = (PFN_UseProgram)GLProc("glUseProgram");
    static PFN_ActiveTexture at = (PFN_ActiveTexture)GLProc("glActiveTexture");
    static PFN_BindBuffer bb = (PFN_BindBuffer)GLProc("glBindBuffer");
    useProgram = up; activeTexture = at; bindBuffer = bb;
    if (useProgram) { glGetIntegerv(GL_CURRENT_PROGRAM, &program); useProgram(0); }
    if (activeTexture) { glGetIntegerv(GL_ACTIVE_TEXTURE, &activeTex); activeTexture(GL_TEXTURE0); }
    if (bindBuffer) { glGetIntegerv(GL_ARRAY_BUFFER_BINDING, &arrayBuffer); bindBuffer(GL_ARRAY_BUFFER, 0); }
    glPushAttrib(GL_ENABLE_BIT | GL_COLOR_BUFFER_BIT | GL_TEXTURE_BIT |
                 GL_CURRENT_BIT | GL_LINE_BIT | GL_TRANSFORM_BIT);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_STENCIL_TEST);
    glDisable(GL_LIGHTING);
    glDisable(GL_CULL_FACE);
    glMatrixMode(GL_PROJECTION);
    glPushMatrix();
    glLoadIdentity();
    glOrtho(0, w, h, 0, -1, 1);  // pixels écran, origine en haut à gauche
    glMatrixMode(GL_MODELVIEW);
    glPushMatrix();
    glLoadIdentity();
    glMatrixMode(GL_TEXTURE);
    glPushMatrix();
    glLoadIdentity();
  }
  ~GLHostState() {
    glMatrixMode(GL_TEXTURE);
    glPopMatrix();
    glMatrixMode(GL_MODELVIEW);
    glPopMatrix();
    glMatrixMode(GL_PROJECTION);
    glPopMatrix();
    glPopAttrib();  // restaure aussi GL_MATRIX_MODE
    if (bindBuffer) bindBuffer(GL_ARRAY_BUFFER, (GLuint)arrayBuffer);
    if (activeTexture) activeTexture((GLenum)activeTex);
    if (useProgram) useProgram((GLuint)program);
  }
};
}  // namespace

static const int kTile = 512;   // cellules par tuile de texture
static const int kMesh = 8;     // subdivisions par tuile (suivi de la projection)
static const uint32_t kInk = RGBA(15, 25, 60), kHalo = RGBA(255, 255, 255);
// Zones interpolées « distinguées » : couleur mêlée de blanc dans cette proportion
static const float kInterpLighten = 0.45f;
// Bandeau de palette : fond sombre, badge « A » (automatique) / « M » (manuel)
static const uint32_t kBannerBg = RGBA(32, 38, 48), kBadgeAuto = RGBA(0, 132, 196),
                      kBadgeMan = RGBA(96, 104, 118), kBadgeSrc = RGBA(70, 84, 104);

struct BathyRenderer::Tile {
  GLuint tex = 0;    // couleurs précalculées (rendu sans lissage)
  GLuint dtex = 0;   // profondeur + ombrage 16 bits (rendu lissé)
  int w = 0, h = 0;
  unsigned stamp = 0;
  unsigned long used = 0;
};

// ---------------------------------------------------------------------------
// Rendu lissé : la texture de tuile contient la profondeur (normalisée sur la
// plage de la couche) et l'ombrage, filtrés linéairement par la carte
// graphique ; la palette est appliquée pixel par pixel via une texture 1D.
// Les paliers restent nets, le relief est continu, et changer la palette ne
// renvoie que la petite texture de couleurs.
namespace {
typedef GLuint(APIENTRY* PFN_CreateShader)(GLenum);
typedef void(APIENTRY* PFN_ShaderSource)(GLuint, GLsizei, const char* const*, const GLint*);
typedef void(APIENTRY* PFN_CompileShader)(GLuint);
typedef void(APIENTRY* PFN_GetShaderiv)(GLuint, GLenum, GLint*);
typedef GLuint(APIENTRY* PFN_CreateProgram)();
typedef void(APIENTRY* PFN_AttachShader)(GLuint, GLuint);
typedef void(APIENTRY* PFN_LinkProgram)(GLuint);
typedef void(APIENTRY* PFN_GetProgramiv)(GLuint, GLenum, GLint*);
typedef GLint(APIENTRY* PFN_GetUniformLocation)(GLuint, const char*);
typedef void(APIENTRY* PFN_Uniform1i)(GLint, GLint);
typedef void(APIENTRY* PFN_Uniform1f)(GLint, GLfloat);
typedef void(APIENTRY* PFN_Uniform4f)(GLint, GLfloat, GLfloat, GLfloat, GLfloat);
typedef void(APIENTRY* PFN_UseProgram2)(GLuint);
typedef void(APIENTRY* PFN_ActiveTexture2)(GLenum);

const char* kVS =
    "#version 120\n"
    "void main() { gl_Position = ftransform(); gl_TexCoord[0] = gl_MultiTexCoord0; }\n";
const char* kFS =
    "#version 120\n"
    "uniform sampler2D depthTex;\n"
    "uniform sampler1D lutTex;\n"
    "uniform sampler2D tideTex;\n"
    "uniform vec4 tideXf;\n"
    "uniform float lutScale, lutOffset, shadeK, opacity, tideScale, interpK;\n"
    "void main() {\n"
    "  vec4 d = texture2D(depthTex, gl_TexCoord[0].st);\n"
    "  if (d.a < 0.5) discard;\n"
    "  float t = texture2D(tideTex, vec2(tideXf.x + tideXf.y * gl_TexCoord[0].s,\n"
    "                                   tideXf.z + tideXf.w * gl_TexCoord[0].t)).r;\n"
    "  vec4 c = texture1D(lutTex, clamp(d.r * lutScale + lutOffset + t * tideScale, 0.0, 0.99999));\n"
    "  float f = 1.0 + shadeK * (d.g * (65535.0 / 160.0 / 256.0) - 1.0);\n"
    "  vec3 rgb = min(c.rgb * f, vec3(1.0));\n"
    "  rgb = mix(rgb, vec3(1.0), interpK * d.b);\n"  // zones interpolées éclaircies
    "  gl_FragColor = vec4(rgb, c.a * opacity);\n"
    "}\n";
}  // namespace

struct BathyRenderer::SmoothGL {
  bool tried = false, ok = false;
  GLuint prog = 0, lutTex = 0, tideTex = 0;
  unsigned lutStamp = 0, tideVersion = ~0u;
  bool tideGrid = false;  // texture de marée maillée valide
  GLint uDepth = -1, uLut = -1, uScale = -1, uOffset = -1, uShade = -1, uOpacity = -1, uInterp = -1;
  GLint uTide = -1, uTideXf = -1, uTideScale = -1;
  PFN_Uniform4f uniform4f = nullptr;
  PFN_UseProgram2 useProgram = nullptr;
  PFN_ActiveTexture2 activeTexture = nullptr;
  PFN_Uniform1i uniform1i = nullptr;
  PFN_Uniform1f uniform1f = nullptr;
};

// Atlas de glyphes OpenGL (Unicode) : rangées de hauteur texH dans une texture
// texW × atlasH ; un glyphe = position x, rangée, largeur. Jeu de départ :
// latin, latin étendu, cyrillique, symboles courants ; tout autre caractère
// rencontré est ajouté (atlas reconstruit).
struct BathyRenderer::Glyphs {
  GLuint tex = 0, texLight = 0;  // encre sombre / halo blanc ; encre blanche / halo sombre
  GLuint texPlain = 0;           // encre blanche sans halo (teintée par glColor)
  int texW = 0, texH = 0, atlasH = 0, lineH = 0, fontSize = 0;
  struct G1 { int x, row, w; };
  std::unordered_map<uint32_t, G1> map;
  const G1* get(uint32_t c) const {
    auto it = map.find(c);
    if (it != map.end()) return &it->second;
    it = map.find((uint32_t)'?');
    return it != map.end() ? &it->second : nullptr;
  }
  bool has(uint32_t c) const { return map.count(c) != 0; }
};

BathyRenderer::BathyRenderer() = default;
BathyRenderer::~BathyRenderer() = default;  // textures libérées via ReleaseGL()

void BathyRenderer::ReleaseGL() {
  for (auto& kv : m_tiles) {
    if (kv.second->tex) glDeleteTextures(1, &kv.second->tex);
    if (kv.second->dtex) glDeleteTextures(1, &kv.second->dtex);
  }
  if (m_sgl && m_sgl->lutTex) glDeleteTextures(1, &m_sgl->lutTex);
  if (m_sgl && m_sgl->tideTex) glDeleteTextures(1, &m_sgl->tideTex);
  m_sgl.reset();
  m_tiles.clear();
  if (m_glyphs && m_glyphs->tex) glDeleteTextures(1, &m_glyphs->tex);
  if (m_glyphs && m_glyphs->texLight) glDeleteTextures(1, &m_glyphs->texLight);
  if (m_glyphs && m_glyphs->texPlain) glDeleteTextures(1, &m_glyphs->texPlain);
  m_glyphs.reset();
}

// ------------------------------------------------------------ rendu lissé
bool BathyRenderer::InitSmoothGL() {
  if (!m_sgl) m_sgl = std::make_unique<SmoothGL>();
  SmoothGL& S = *m_sgl;
  if (S.tried) return S.ok;
  S.tried = true;
  auto createShader = (PFN_CreateShader)GLProc("glCreateShader");
  auto shaderSource = (PFN_ShaderSource)GLProc("glShaderSource");
  auto compileShader = (PFN_CompileShader)GLProc("glCompileShader");
  auto getShaderiv = (PFN_GetShaderiv)GLProc("glGetShaderiv");
  auto createProgram = (PFN_CreateProgram)GLProc("glCreateProgram");
  auto attachShader = (PFN_AttachShader)GLProc("glAttachShader");
  auto linkProgram = (PFN_LinkProgram)GLProc("glLinkProgram");
  auto getProgramiv = (PFN_GetProgramiv)GLProc("glGetProgramiv");
  auto getUniform = (PFN_GetUniformLocation)GLProc("glGetUniformLocation");
  S.useProgram = (PFN_UseProgram2)GLProc("glUseProgram");
  S.activeTexture = (PFN_ActiveTexture2)GLProc("glActiveTexture");
  S.uniform1i = (PFN_Uniform1i)GLProc("glUniform1i");
  S.uniform1f = (PFN_Uniform1f)GLProc("glUniform1f");
  S.uniform4f = (PFN_Uniform4f)GLProc("glUniform4f");
  if (!S.uniform4f ||!createShader || !shaderSource || !compileShader || !getShaderiv || !createProgram ||
      !attachShader || !linkProgram || !getProgramiv || !getUniform || !S.useProgram ||
      !S.activeTexture || !S.uniform1i || !S.uniform1f)
    return false;
  auto compile = [&](GLenum type, const char* src) -> GLuint {
    GLuint sh = createShader(type);
    shaderSource(sh, 1, &src, nullptr);
    compileShader(sh);
    GLint okc = 0;
    getShaderiv(sh, GL_COMPILE_STATUS, &okc);
    return okc ? sh : 0;
  };
  GLuint vs = compile(GL_VERTEX_SHADER, kVS), fs = compile(GL_FRAGMENT_SHADER, kFS);
  if (!vs || !fs) return false;
  S.prog = createProgram();
  attachShader(S.prog, vs);
  attachShader(S.prog, fs);
  linkProgram(S.prog);
  GLint okl = 0;
  getProgramiv(S.prog, GL_LINK_STATUS, &okl);
  if (!okl) return false;
  S.uDepth = getUniform(S.prog, "depthTex");
  S.uLut = getUniform(S.prog, "lutTex");
  S.uScale = getUniform(S.prog, "lutScale");
  S.uOffset = getUniform(S.prog, "lutOffset");
  S.uShade = getUniform(S.prog, "shadeK");
  S.uOpacity = getUniform(S.prog, "opacity");
  S.uInterp = getUniform(S.prog, "interpK");
  S.uTide = getUniform(S.prog, "tideTex");
  S.uTideXf = getUniform(S.prog, "tideXf");
  S.uTideScale = getUniform(S.prog, "tideScale");
  glGenTextures(1, &S.lutTex);
  glGenTextures(1, &S.tideTex);
  S.ok = S.lutTex != 0 && S.tideTex != 0;
  return S.ok;
}

// Texture 16 bits d'une tuile avec une cellule de marge (pas de couture
// entre tuiles) : R = profondeur normalisée, G = ombrage, A = donnée présente.
// Les cellules vides voisines reçoivent la moyenne de leurs voisines pour que
// l'interpolation ne fasse pas « plonger » le bord des données.
void BathyRenderer::UploadDepthTile(Tile& t, const GridLevel& g, const BathyLayer& l, int ti,
                                    int tj) {
  const int W = t.w + 2, H = t.h + 2;
  const float z0 = l.grid.zMin, zr = std::max(1e-6f, l.grid.zMax - l.grid.zMin);
  const bool sh = g.shade.size() == g.z.size();
  std::vector<uint16_t> buf((size_t)W * H * 4);
  auto Z = [&](int i, int j) {
    return (i < 0 || j < 0 || i >= g.nx || j >= g.ny) ? NAN : g.at(i, j);
  };
  for (int y = 0; y < H; ++y)
    for (int x = 0; x < W; ++x) {
      const int i = ti * kTile + x - 1, j = tj * kTile + y - 1;
      float z = Z(i, j);
      float sv = 160.f;
      bool valid = !std::isnan(z);
      float ip = 0.f;  // part de cellules interpolées (bord des données : voisines)
      if (valid) {
        if (sh) sv = g.shade[(size_t)j * g.nx + i];
        ip = g.isInterpFar((size_t)j * g.nx + i) ? 1.f : 0.f;
      } else {
        double acc = 0, sacc = 0;
        int n = 0;
        for (int dj = -1; dj <= 1; ++dj)
          for (int di = -1; di <= 1; ++di) {
            float v = Z(i + di, j + dj);
            if (std::isnan(v)) continue;
            acc += v; ++n;
            sacc += sh ? g.shade[(size_t)(j + dj) * g.nx + (i + di)] : 160.f;
            ip += g.isInterpFar((size_t)(j + dj) * g.nx + (i + di)) ? 1.f : 0.f;
          }
        z = n ? (float)(acc / n) : z0;
        sv = n ? (float)(sacc / n) : 160.f;
        ip = n ? ip / n : 0.f;
      }
      uint16_t* o = &buf[((size_t)y * W + x) * 4];
      o[0] = (uint16_t)std::lround(std::min(1.f, std::max(0.f, (z - z0) / zr)) * 65535.f);
      o[1] = (uint16_t)std::lround(sv * 256.f);  // 0..255 -> 0..65280
      o[2] = (uint16_t)std::lround(ip * 65535.f);
      o[3] = valid ? 65535 : 0;
    }
  if (!t.dtex) glGenTextures(1, &t.dtex);
  glBindTexture(GL_TEXTURE_2D, t.dtex);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
  glPixelStorei(GL_UNPACK_ALIGNMENT, 2);
  glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA16, W, H, 0, GL_RGBA, GL_UNSIGNED_SHORT, buf.data());
  glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
}

// ------------------------------------------------------------------ commun
bool BathyRenderer::PrepareLUT(const std::vector<LayerPtr>& layers, const DisplaySettings& s) {
  float lo = 1e30f, hi = -1e30f;
  for (auto& l : layers)
    if (l->visible && l->state() == BathyLayer::Ready) {
      lo = std::min(lo, l->grid.zMin); hi = std::max(hi, l->grid.zMax);
    }
  if (lo > hi) return false;
  // Plage de la palette : profondeurs du moment (ZH + marée), arrondie au
  // demi-mètre pour ne pas tout reconstruire à chaque minute
  const TideField& T = TideOf(s);
  lo += std::floor((T.gridded() ? T.hMin : T.h0) * 2.f) / 2.f;
  hi += std::ceil((T.gridded() ? T.hMax : T.h0) * 2.f) / 2.f;
  // Au-delà des seuils extrêmes la couleur ne change plus : la table couvre
  // seulement la plage utile (une sonde aberrante n'écrase pas la palette)
  if (!s.ramp.stops.empty()) {
    float a = s.ramp.stops.front().depth, b = s.ramp.stops.back().depth;
    if (s.ramp.safetyOn) { a = std::min(a, s.ramp.safetyDepth); b = std::max(b, s.ramp.safetyDepth); }
    a = std::floor(a) - 1.f; b = std::ceil(b) + 1.f;
    const float l2 = std::max(lo, a), h2 = std::min(hi, b);
    if (h2 > l2) { lo = l2; hi = h2; }
    else { lo = a; hi = b; }  // données toutes hors palette : couleurs extrêmes
  }
  if (s.version != m_lutVersion || lo != m_lutMin || hi != m_lutMax || T.version != m_tideVersion) {
    m_lut.build(s.ramp, lo, hi, 16384);
    m_lutVersion = s.version; m_lutMin = lo; m_lutMax = hi; m_lutOffset = 0.f;
    m_tideVersion = T.version;
    ++m_lutStamp;  // toutes les tuiles seront recolorées à la demande
  }
  return true;
}

bool BathyRenderer::FitMapper(PlugIn_ViewPort* vp, ScreenMapper& sm) {
  const int W = vp->pix_width, H = vp->pix_height;
  wxPoint p[3] = {wxPoint(0, H), wxPoint(W, H), wxPoint(0, 0)};
  double lon[3], lat[3], x[3], y[3];
  for (int k = 0; k < 3; ++k) {
    GetCanvasLLPix(vp, p[k], &lat[k], &lon[k]);
    x[k] = p[k].x; y[k] = p[k].y;
  }
  if (!sm.fit(lon, lat, x, y)) return false;
  m_ppd = sm.pixelsPerDegLon();
  return true;
}

// Profondeur (m) dans l'unité d'affichage d'OpenCPN, arrondie au centième
static wxString DepthText(const DisplaySettings& s, double m) {
  return wxString::Format("%g", std::round(m * s.depthK * 100.0) / 100.0);
}

void BathyRenderer::FormatDepth(float z, int decimals, wxString& ip, wxString& dp) {
  // Usage hydrographique : troncature vers la valeur la moins profonde
  double v = std::fabs((double)z), p = std::pow(10.0, decimals);
  double t = (z >= 0 ? std::floor(v * p + 1e-6) : std::ceil(v * p - 1e-6)) / p;
  long ipart = (long)std::floor(t + 1e-9);
  ip = wxString::Format("%ld", ipart);
  dp.clear();
  if (decimals > 0) {
    long frac = std::lround((t - ipart) * p);
    dp = wxString::Format("%0*ld", decimals, frac);
  }
}

std::vector<Label> BathyRenderer::CollectLabels(PlugIn_ViewPort* vp, const ScreenMapper& sm,
                                                const std::vector<LayerPtr>& layers,
                                                const DisplaySettings& s) {
  std::vector<Label> all;
  if (!s.soundings) return all;
  for (auto& l : layers) {
    if (!l->visible || l->state() != BathyLayer::Ready) continue;
    auto v = SelectSoundings(l->cloud, l->index, l->grid, sm, vp->lon_min, vp->lon_max,
                             vp->lat_min, vp->lat_max, vp->pix_width, vp->pix_height,
                             (float)s.spacingPx);
    all.insert(all.end(), v.begin(), v.end());
  }
  // profondeur du moment : sonde ZH + hauteur d'eau à sa position
  const TideField& T = TideOf(s);
  for (Label& L : all) {
    float h = T.h0;
    if (T.gridded()) {
      double la, lo;
      GetCanvasLLPix(vp, wxPoint((int)std::lround(L.x), (int)std::lround(L.y)), &la, &lo);
      h = T.at(lo, la);
    }
    L.depth += h;
  }
  return all;
}

// ------------------------------------------------------------- OpenGL
void BathyRenderer::BuildGlyphs(int fontSize) {
  if (m_glyphs && m_glyphs->fontSize == fontSize) return;
  if (m_glyphs && m_glyphs->tex) glDeleteTextures(1, &m_glyphs->tex);
  if (m_glyphs && m_glyphs->texLight) glDeleteTextures(1, &m_glyphs->texLight);
  if (m_glyphs && m_glyphs->texPlain) glDeleteTextures(1, &m_glyphs->texPlain);
  m_glyphs = std::make_unique<Glyphs>();
  Glyphs& G = *m_glyphs;
  G.fontSize = fontSize;
  std::vector<uint32_t> chars;
  for (uint32_t c = 32; c < 127; ++c) chars.push_back(c);
  for (uint32_t c = 0xA0; c <= 0x17F; ++c) chars.push_back(c);   // latin-1, latin étendu A
  for (uint32_t c = 0x400; c <= 0x45F; ++c) chars.push_back(c);  // cyrillique
  for (uint32_t c : {0x2013u, 0x2014u, 0x2018u, 0x2019u, 0x201Cu, 0x201Du, 0x2026u, 0x2116u,
                     0x2190u, 0x2191u, 0x2192u, 0x2193u, 0x2194u, 0x2195u, 0x21D4u, 0x21D5u,
                     0x2212u, 0x2248u, 0x2264u, 0x2265u, 0x2030u, 0x2032u, 0x2033u})
    chars.push_back(c);
  for (uint32_t c : m_glyphExtra) chars.push_back(c);  // rencontrés depuis
  wxFont font(fontSize, wxFONTFAMILY_SWISS, wxFONTSTYLE_NORMAL, wxFONTWEIGHT_BOLD);
  wxBitmap probe(8, 8);
  wxMemoryDC md(probe);
  md.SetFont(font);
  G.texW = 1024;
  int x = 2, row = 0, h = 0;
  std::vector<std::pair<uint32_t, Glyphs::G1>> placed;
  for (uint32_t c : chars) {
    if (G.map.count(c)) continue;
    const wxString str(wxUniChar((unsigned)c));
    const wxSize sz = md.GetTextExtent(str);
    if (sz.x <= 0 && c != 32) continue;
    if (x + sz.x + 4 > G.texW) { x = 2; ++row; }
    const Glyphs::G1 g{x, row, sz.x + 2};
    G.map[c] = g;
    placed.emplace_back(c, g);
    x += sz.x + 4;
    h = std::max(h, sz.y);
  }
  md.SelectObject(wxNullBitmap);
  G.lineH = h + 2;
  G.texH = h + 4;
  G.atlasH = (row + 1) * G.texH;
  wxBitmap bmp(G.texW, G.atlasH, 24);
  md.SelectObject(bmp);
  md.SetBackground(*wxBLACK_BRUSH);
  md.Clear();
  md.SetFont(font);
  md.SetTextForeground(*wxWHITE);
  for (const auto& pg : placed)
    md.DrawText(wxString(wxUniChar((unsigned)pg.first)), pg.second.x + 1, pg.second.row * G.texH + 2);
  md.SelectObject(wxNullBitmap);
  wxImage img = bmp.ConvertToImage();
  const unsigned char* d = img.GetData();
  std::vector<float> A((size_t)G.texW * G.atlasH);
  for (size_t k = 0; k < A.size(); ++k) A[k] = d[3 * k] / 255.f;
  auto build = [&](uint32_t ink, uint32_t halo, bool withHalo) {
    std::vector<uint32_t> px(A.size());
    for (int j = 0; j < G.atlasH; ++j)
      for (int i = 0; i < G.texW; ++i) {
        float a = A[(size_t)j * G.texW + i], hmax = 0;
        for (int dj = -1; dj <= 1; ++dj)
          for (int di = -1; di <= 1; ++di) {
            int ii = i + di, jj = j + dj;
            if (ii >= 0 && jj >= 0 && ii < G.texW && jj < G.atlasH)
              hmax = std::max(hmax, A[(size_t)jj * G.texW + ii]);
          }
        auto mix = [a](int in, int ha) { return (int)(in * a + ha * (1 - a)); };
        px[(size_t)j * G.texW + i] =
            RGBA(mix(R_(ink), R_(halo)), mix(G_(ink), G_(halo)), mix(B_(ink), B_(halo)),
                 (int)(255 * (withHalo ? std::max(a, hmax * 0.9f) : a)));
      }
    GLuint t = 0;
    glGenTextures(1, &t);
    glBindTexture(GL_TEXTURE_2D, t);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, G.texW, G.atlasH, 0, GL_RGBA, GL_UNSIGNED_BYTE,
                 px.data());
    return t;
  };
  G.tex = build(kInk, kHalo, true);
  G.texLight = build(RGBA(255, 255, 255), kBannerBg, true);
  G.texPlain = build(RGBA(255, 255, 255), RGBA(255, 255, 255), false);
}

// caractères absents de l'atlas : ajoutés (atlas reconstruit, contexte GL courant)
void BathyRenderer::EnsureGlyphs(const wxString& t) {
  if (!m_glyphs) return;
  bool add = false;
  for (wxUniChar ch : t) {
    const uint32_t c = (uint32_t)ch.GetValue();
    if (c >= 32 && !m_glyphs->has(c) && m_glyphExtra.size() < 2000 && m_glyphExtra.insert(c).second)
      add = true;
  }
  if (!add) return;
  const int fs = m_glyphs->fontSize;
  m_glyphs->fontSize = -1;  // force la reconstruction
  BuildGlyphs(fs);
}

float BathyRenderer::GLTextWidth(const wxString& t, float scale) {
  EnsureGlyphs(t);
  float w = 0;
  for (wxUniChar ch : t) {
    const uint32_t c = (uint32_t)ch.GetValue();
    if (c < 32) continue;
    if (const Glyphs::G1* g = m_glyphs->get(c)) w += (g->w - 2) * scale;
  }
  return w;
}

void BathyRenderer::DrawGLText(float x, float y, const wxString& t, float scale, bool center) {
  EnsureGlyphs(t);
  Glyphs& G = *m_glyphs;
  if (center) x -= GLTextWidth(t, scale) / 2;
  const float h = G.texH * scale;
  y -= h / 2;
  glBindTexture(GL_TEXTURE_2D, m_glPlain ? G.texPlain : m_glLight ? G.texLight : G.tex);
  glBegin(GL_QUADS);
  for (wxUniChar ch : t) {
    const uint32_t c = (uint32_t)ch.GetValue();
    if (c < 32) continue;
    const Glyphs::G1* g = G.get(c);
    if (!g) continue;
    const float u0 = (float)(g->x - 1) / G.texW, u1 = (float)(g->x + g->w) / G.texW;
    const float v0 = (float)(g->row * G.texH) / G.atlasH, v1 = (float)((g->row + 1) * G.texH) / G.atlasH;
    const float w = (g->w + 1) * scale;
    glTexCoord2f(u0, v0); glVertex2f(x, y);
    glTexCoord2f(u1, v0); glVertex2f(x + w, y);
    glTexCoord2f(u1, v1); glVertex2f(x + w, y + h);
    glTexCoord2f(u0, v1); glVertex2f(x, y + h);
    x += (g->w - 2) * scale;
  }
  glEnd();
}

namespace {
using PerfClock = std::chrono::steady_clock;
double MsSince(PerfClock::time_point t) {
  return std::chrono::duration<double, std::milli>(PerfClock::now() - t).count();
}
struct PerfFrame {  // durée totale d'une image du plugin
  BathyRenderer::Perf& p;
  PerfClock::time_point t0 = PerfClock::now();
  explicit PerfFrame(BathyRenderer::Perf& q) : p(q) {}
  ~PerfFrame() {
    const double ms = MsSince(t0);
    ++p.frames;
    p.totalMs += ms;
    p.maxMs = std::max(p.maxMs, ms);
  }
};
}  // namespace

// Texture d'une tuile prête (envoyée à la carte graphique si besoin)
bool BathyRenderer::EnsureTile(Tile& t, const GridLevel& g, const BathyLayer& l, int ti, int tj,
                               bool shader, const DisplaySettings& s, std::vector<uint32_t>& buf) {
  t.used = m_frame;
  t.w = std::min(kTile, g.nx - ti * kTile);
  t.h = std::min(kTile, g.ny - tj * kTile);
  if (shader) {
    if (t.dtex) return false;
    const auto tu = PerfClock::now();
    UploadDepthTile(t, g, l, ti, tj);
    m_perf.uploadMs += MsSince(tu);
    ++m_perf.uploads;
    return true;
  }
  if (t.tex && t.stamp == m_lutStamp) return false;
  // (re)coloration : LUT seulement
  const auto tu = PerfClock::now();
  buf.resize((size_t)t.w * t.h);
  Colorize(g, ti * kTile, tj * kTile, t.w, t.h, m_lut, s.hillshade ? s.shadeStrength : 0.f, buf.data(), t.w,
           &TideOf(s), s.interpDistinct ? kInterpLighten : 0.f);
  if (!t.tex) glGenTextures(1, &t.tex);
  glBindTexture(GL_TEXTURE_2D, t.tex);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
  glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
  glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, t.w, t.h, 0, GL_RGBA, GL_UNSIGNED_BYTE, buf.data());
  t.stamp = m_lutStamp;
  m_perf.uploadMs += MsSince(tu);
  ++m_perf.uploads;
  return true;
}

// Nouvelle vue en attente : ses textures (zone de l'écran) et ses isobathes
// sont préparées par petits lots, quelques millisecondes par image, pendant
// que la vue actuelle reste affichée ; elle remplace la vue actuelle une fois
// prête, sans à-coup.
void BathyRenderer::PrewarmPending(PlugIn_ViewPort* vp, const DisplaySettings& s, bool shader,
                                   std::vector<uint32_t>& buf) {
  if (!m_pending || m_pendingDone) return;
  const BathyLayer& l = *m_pending;
  if (l.state() != BathyLayer::Ready || l.grid.levels.empty()) { m_pendingDone = true; return; }
  const auto t0 = PerfClock::now();
  const int lev = l.grid.levelFor(m_ppd, 1.0);
  const GridLevel& g = l.grid.levels[lev];
  const int i0 = std::max(0, (int)std::floor((vp->lon_min - g.lonW) / g.dlon));
  const int i1 = std::min(g.nx - 1, (int)std::floor((vp->lon_max - g.lonW) / g.dlon));
  const int j0 = std::max(0, (int)std::floor((g.latN - vp->lat_max) / g.dlat));
  const int j1 = std::min(g.ny - 1, (int)std::floor((g.latN - vp->lat_min) / g.dlat));
  bool all = true;
  for (int tj = j0 / kTile; !m_tilesHidden && i0 <= i1 && tj <= j1 / kTile; ++tj)
    for (int ti = i0 / kTile; ti <= i1 / kTile; ++ti) {
      auto& slot = m_tiles[std::make_tuple((const void*)&l, lev, ti, tj)];
      if (!slot) slot = std::make_unique<Tile>();
      const bool ready = shader ? slot->dtex != 0 : (slot->tex && slot->stamp == m_lutStamp);
      if (ready) { slot->used = m_frame; continue; }
      if (MsSince(t0) > 8.0) { all = false; continue; }  // la suite à l'image suivante
      EnsureTile(*slot, g, l, ti, tj, shader, s, buf);
    }
  if (!all) return;
  if (s.contours && MsSince(t0) > 4.0) return;  // isobathes : image suivante
  if (s.contours) Contours(l, s);
  m_pendingDone = true;
}

void BathyRenderer::RenderGL(PlugIn_ViewPort* vp, const std::vector<LayerPtr>& layers,
                             const DisplaySettings& s, const wxString& cursorText, int parts) {
  PerfFrame perfFrame(m_perf);
  ++m_frame;
  if (m_purgedEpoch != m_epoch) {  // couches retirées : on vide le cache de textures
    // sauf la vue qui vient d'être adoptée (préparée d'avance) et celle en attente
    const void* pend = m_pending.get();
    for (auto it = m_tiles.begin(); it != m_tiles.end();) {
      const void* lp = std::get<0>(it->first);
      if (lp && (lp == m_keepLayer || lp == pend)) { ++it; continue; }
      if (it->second->tex) glDeleteTextures(1, &it->second->tex);
      if (it->second->dtex) glDeleteTextures(1, &it->second->dtex);
      it = m_tiles.erase(it);
    }
    m_keepLayer = nullptr;
    m_purgedEpoch = m_epoch;
  }
  if (!PrepareLUT(layers, s)) return;
  ScreenMapper sm;
  if (!FitMapper(vp, sm)) return;

  GLHostState hostState(vp->pix_width, vp->pix_height);
  glEnable(GL_TEXTURE_2D);
  glEnable(GL_BLEND);
  glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
  glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_MODULATE);
  glColor4f(1, 1, 1, s.opacity / 100.f);
  const GLint filt = GL_NEAREST;
  std::vector<uint32_t> buf;
  std::vector<wxPoint2DDouble> vtx((kMesh + 1) * (kMesh + 1));

  // Rendu lissé par shader si demandé et disponible
  const bool shader = s.smooth && InitSmoothGL();
  if (shader) {
    SmoothGL& S = *m_sgl;
    if (S.lutStamp != m_lutStamp) {  // palette : seule cette texture change
      std::vector<uint32_t> lut(4096);
      const float zr = m_lutMax - m_lutMin;
      for (int k = 0; k < 4096; ++k)
        lut[k] = s.ramp.colorAt(m_lutMin + (k + 0.5f) * zr / 4096.f + m_lutOffset);
      glBindTexture(GL_TEXTURE_1D, S.lutTex);
      glTexParameteri(GL_TEXTURE_1D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
      glTexParameteri(GL_TEXTURE_1D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
      glTexParameteri(GL_TEXTURE_1D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
      glTexImage1D(GL_TEXTURE_1D, 0, GL_RGBA, 4096, 0, GL_RGBA, GL_UNSIGNED_BYTE, lut.data());
      S.lutStamp = m_lutStamp;
    }
    const TideField& T = TideOf(s);
    if (S.tideVersion != T.version) {  // champ de marée : petite texture 16 bits
      S.activeTexture(GL_TEXTURE2);
      glBindTexture(GL_TEXTURE_2D, S.tideTex);
      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
      GLint maxTex = 2048;
      glGetIntegerv(GL_MAX_TEXTURE_SIZE, &maxTex);
      S.tideGrid = T.gridded() && T.nx <= maxTex && T.ny <= maxTex;
      std::vector<uint16_t> tb;
      int tw = 1, th = 1;
      if (S.tideGrid) {
        tw = T.nx; th = T.ny;
        const float r = T.hMax - T.hMin;
        tb.resize((size_t)tw * th);
        for (size_t k = 0; k < tb.size(); ++k)
          tb[k] = r > 1e-6f ? (uint16_t)std::lround((T.v[k] - T.hMin) / r * 65535.f) : 0;
      } else {
        tb.assign(1, 0);
      }
      glPixelStorei(GL_UNPACK_ALIGNMENT, 2);
      glTexImage2D(GL_TEXTURE_2D, 0, GL_LUMINANCE16, tw, th, 0, GL_LUMINANCE, GL_UNSIGNED_SHORT,
                   tb.data());
      glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
      S.activeTexture(GL_TEXTURE0);
      S.tideVersion = T.version;
    }
    S.useProgram(S.prog);
    S.activeTexture(GL_TEXTURE2);
    glBindTexture(GL_TEXTURE_2D, S.tideTex);
    S.activeTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_1D, S.lutTex);
    S.activeTexture(GL_TEXTURE0);
    S.uniform1i(S.uDepth, 0);
    S.uniform1i(S.uLut, 1);
    S.uniform1i(S.uTide, 2);
    S.uniform1f(S.uShade, s.hillshade ? s.shadeStrength : 0.f);
    S.uniform1f(S.uOpacity, s.opacity / 100.f);
    S.uniform1f(S.uInterp, s.interpDistinct ? kInterpLighten : 0.f);
  }

  for (auto& l : layers) {
    if (!(parts & kTiles)) break;  // passe des isobathes et étiquettes seulement
    if (!l->visible || l->state() != BathyLayer::Ready) continue;
    const int lev = l->grid.levelFor(m_ppd, 1.0);
    const GridLevel& g = l->grid.levels[lev];
    const TideField& TF = TideOf(s);
    const bool tideTexOn = shader && m_sgl->tideGrid;
    if (shader) {  // profondeur de la couche (+ marée) -> position dans la palette commune
      const float lr = std::max(1e-6f, m_lutMax - m_lutMin);
      const float tb = tideTexOn ? TF.hMin : TF.h0;
      m_sgl->uniform1f(m_sgl->uScale, (l->grid.zMax - l->grid.zMin) / lr);
      m_sgl->uniform1f(m_sgl->uOffset, (l->grid.zMin - m_lutMin + tb) / lr);
      m_sgl->uniform1f(m_sgl->uTideScale, tideTexOn ? (TF.hMax - TF.hMin) / lr : 0.f);
    }
    int i0 = std::max(0, (int)std::floor((vp->lon_min - g.lonW) / g.dlon));
    int i1 = std::min(g.nx - 1, (int)std::floor((vp->lon_max - g.lonW) / g.dlon));
    int j0 = std::max(0, (int)std::floor((g.latN - vp->lat_max) / g.dlat));
    int j1 = std::min(g.ny - 1, (int)std::floor((g.latN - vp->lat_min) / g.dlat));
    if (i0 > i1 || j0 > j1) continue;
    for (int tj = j0 / kTile; tj <= j1 / kTile; ++tj)
      for (int ti = i0 / kTile; ti <= i1 / kTile; ++ti) {
        auto& slot = m_tiles[std::make_tuple((const void*)l.get(), lev, ti, tj)];
        if (!slot) slot = std::make_unique<Tile>();
        Tile& t = *slot;
        EnsureTile(t, g, *l, ti, tj, shader, s, buf);
        if (shader) glBindTexture(GL_TEXTURE_2D, t.dtex);
        if (!shader) {
          glBindTexture(GL_TEXTURE_2D, t.tex);
          glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, filt);
          glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, filt);
        }
        // coordonnées de texture : marge d'une cellule autour de la tuile lissée
        const float pu = shader ? 1.f / (t.w + 2) : 0.f, su = shader ? (float)t.w / (t.w + 2) : 1.f;
        const float pv = shader ? 1.f / (t.h + 2) : 0.f, sv = shader ? (float)t.h / (t.h + 2) : 1.f;
        const double lonA = g.lonW + ti * kTile * g.dlon, lonB = lonA + t.w * g.dlon;
        const double latA = g.latN - tj * kTile * g.dlat, latB = latA - t.h * g.dlat;
        if (tideTexOn) {  // coordonnées de tuile -> coordonnées de la texture de marée
          const double lonBase = g.lonW + (ti * kTile - 1) * g.dlon;
          const double latBase = g.latN - (tj * kTile - 1) * g.dlat;
          const double sx = TF.dlon * TF.nx, sy = TF.dlat * TF.ny;
          m_sgl->uniform4f(m_sgl->uTideXf, (float)((lonBase - TF.lon0) / sx + 0.5 / TF.nx),
                           (float)((t.w + 2) * g.dlon / sx),
                           (float)((latBase - TF.lat0) / sy + 0.5 / TF.ny),
                           (float)(-(t.h + 2) * g.dlat / sy));
        }
        for (int r = 0; r <= kMesh; ++r)
          for (int c = 0; c <= kMesh; ++c)
            GetDoubleCanvasPixLL(vp, &vtx[r * (kMesh + 1) + c],
                                 latA + (latB - latA) * r / kMesh,
                                 lonA + (lonB - lonA) * c / kMesh);
        glBegin(GL_QUADS);
        for (int r = 0; r < kMesh; ++r)
          for (int c = 0; c < kMesh; ++c) {
            const int q[4][2] = {{r, c}, {r, c + 1}, {r + 1, c + 1}, {r + 1, c}};
            for (auto& k : q) {
              const wxPoint2DDouble& p = vtx[k[0] * (kMesh + 1) + k[1]];
              glTexCoord2f(pu + su * k[1] / kMesh, pv + sv * k[0] / kMesh);
              glVertex2d(p.m_x, p.m_y);
            }
          }
        glEnd();
      }
  }
  if (shader) m_sgl->useProgram(0);  // retour au pipeline fixe (lignes, texte)
  if ((parts & kTiles) || m_tilesHidden) PrewarmPending(vp, s, shader, buf);
  // Libère les tuiles inutilisées si le cache grossit
  if (m_tiles.size() > 160)
    for (auto it = m_tiles.begin(); it != m_tiles.end();) {
      if (it->second->used + 2 < m_frame) {
        if (it->second->tex) glDeleteTextures(1, &it->second->tex);
        if (it->second->dtex) glDeleteTextures(1, &it->second->dtex);
        it = m_tiles.erase(it);
      } else ++it;
    }

  m_perf.tileMs += MsSince(perfFrame.t0);
  if (!(parts & kOverlays)) return;  // imagerie, ou couche dessous : tuiles seules
  // Isobathes
  const auto tc = PerfClock::now();
  DrawContoursGL(vp, sm, layers, s);
  m_perf.contourMs += MsSince(tc);

  // Sondes chiffrées et valeurs d'isobathes
  BuildGlyphs(s.fontSize);
  wxString ip, dp;
  const wxString key = LabelKey(vp, layers, s, 'G');
  const wxString skey = LabelKey(vp, layers, s, 'G', false);
  if (key != m_labelKey && skey == m_labelStatic && m_labelLon.size() == m_labels.size()) {
    // carte seulement déplacée : mêmes étiquettes, décalées ; nouvelle
    // sélection quand la carte s'arrête (RefreshLabels)
    for (size_t k = 0; k < m_labels.size(); ++k) sm.map(m_labelLon[k], m_labelLat[k], m_labels[k].x, m_labels[k].y);
    m_labelKey = key;
    m_labelsShifted = true;
  }
  if (key != m_labelKey) {  // vue ou réglages changés : nouvelle sélection
    const auto tl = PerfClock::now();
    ++m_perf.labelRuns;
    m_labels = CollectLabels(vp, sm, layers, s);
    AddContourLabels(vp, sm, layers, s, m_labels);
    for (Label& L : m_labels) {
      if (L.contour) { L.w = GLTextWidth(DepthText(s, L.depth), 1.f) + 4; continue; }
      FormatDepth(L.depth * s.depthK, s.decimals, ip, dp);
      L.w = GLTextWidth(ip, 1.f) + GLTextWidth(dp, 0.72f);
    }
    Declutter(m_labels, (float)m_glyphs->lineH, 4.f);
    m_labelKey = key;
    m_labelStatic = skey;
    m_labelsShifted = false;
    // position géographique de chaque étiquette (inverse de la transformation affine)
    m_labelLon.resize(m_labels.size());
    m_labelLat.resize(m_labels.size());
    const double det = sm.a * sm.e - sm.b * sm.d;
    for (size_t k = 0; k < m_labels.size(); ++k) {
      const double dx = m_labels[k].x - sm.c, dy = m_labels[k].y - sm.f;
      const double lon = std::fabs(det) > 0 ? (sm.e * dx - sm.b * dy) / det : 0.0;
      const double my = std::fabs(det) > 0 ? (-sm.d * dx + sm.a * dy) / det : 0.0;
      m_labelLon[k] = lon;
      m_labelLat[k] = std::atan(std::sinh(my * M_PI / 180.0)) * 180.0 / M_PI;
    }
    m_perf.labelMs += MsSince(tl);
  }
  for (const Label& L : m_labels) {
    if (L.contour) {  // valeur d'isobathe : teinte des lignes, centrée sur la ligne
      uint32_t c = IsSafety(s, L.depth) ? s.ramp.safetyColor : s.contourColor;
      // chiffres pleins à la couleur des lignes, sans liseré
      glColor4f(R_(c) / 255.f, G_(c) / 255.f, B_(c) / 255.f, 1.f);
      m_glPlain = true;
      DrawGLText(L.x, L.y, DepthText(s, L.depth), 1.f, true);
      m_glPlain = false;
      continue;
    }
    FormatDepth(L.depth * s.depthK, s.decimals, ip, dp);
    if (L.fromGrid) glColor4f(0.55f, 0.6f, 0.7f, 1.f); else glColor4f(1, 1, 1, 1);
    float wi = GLTextWidth(ip, 1.f), wd = GLTextWidth(dp, 0.72f);
    float x0 = L.x - (wi + wd) / 2;
    DrawGLText(x0, L.y, ip, 1.f, false);
    if (!dp.empty())
      DrawGLText(x0 + wi, L.y + m_glyphs->lineH * 0.28f, dp, 0.72f, false);
    if (L.depth < 0) {  // découvrant : valeur soulignée (convention des cartes)
      glDisable(GL_TEXTURE_2D);
      glColor4f(R_(kInk) / 255.f, G_(kInk) / 255.f, B_(kInk) / 255.f, 1);
      glLineWidth(1.5f);
      glBegin(GL_LINES);
      float yy = L.y + m_glyphs->lineH * 0.55f;
      glVertex2f(x0, yy); glVertex2f(x0 + wi + wd, yy);
      glEnd();
      glEnable(GL_TEXTURE_2D);
    }
  }
  if (s.banner) DrawBannerGL(vp, s); else m_banner.valid = false;
  if (!cursorText.empty()) DrawCursorGL(vp, cursorText);
}

// ------------------------------------------------------------ isobathes
std::vector<float> ContourLevels(const DisplaySettings& s, float zMin, float zMax) {
  std::vector<float> lv;
  if (s.contourMode == 1) {
    for (const auto& st : s.ramp.stops)
      if (st.depth > zMin && st.depth <= zMax) lv.push_back(st.depth);
  } else {
    // valeurs rondes dans l'unité affichée (ex. tous les 10 ft), converties en m
    const float k = s.depthK > 0 ? s.depthK : 1.f;
    lv = RegularLevels(zMin * k, zMax * k, s.contourStep * k);
    for (float& z : lv) z /= k;
  }
  if (s.ramp.safetyOn && s.ramp.safetyDepth > zMin && s.ramp.safetyDepth <= zMax)
    lv.push_back(s.ramp.safetyDepth);
  std::sort(lv.begin(), lv.end());
  lv.erase(std::unique(lv.begin(), lv.end()), lv.end());
  return lv;
}

static bool IsSafety(const DisplaySettings& s, float level) {
  return s.ramp.safetyOn && std::fabs(level - s.ramp.safetyDepth) < 1e-3f;
}
static bool IsMajor(const DisplaySettings& s, float level) {
  if (!(s.contourMajor > 0)) return false;
  const double k = s.depthK > 0 ? s.depthK : 1.0;  // en unité affichée
  const double lv = std::round(level * k * 1000.0) / 1000.0, mj = std::round(s.contourMajor * k * 1000.0) / 1000.0;
  if (!(mj > 0)) return false;
  double r = std::fmod(lv, mj);
  return std::fabs(r) < 1e-3 || std::fabs(r - mj) < 1e-3;
}

int BathyRenderer::ContourLevelIndex(const BathyLayer& l) const {
  return l.grid.levelFor(m_ppd, 2.0);  // cellule <= 2 px : lignes nettes et peu coûteuses
}

const std::vector<Isoline>& BathyRenderer::Isolines(const BathyLayer& l, int lev,
                                                    const DisplaySettings& s) {
  // Niveaux exprimés en profondeur du moment (ZH + marée). Marée uniforme :
  // tracés sur la grille ZH au niveau - h ; marée maillée : tracés sur une
  // copie de la grille à laquelle la hauteur d'eau de chaque cellule est ajoutée
  const TideField& T = TideOf(s);
  const float h = T.gridded() ? 0.f : T.h0;
  const float tlo = T.gridded() ? T.hMin : h, thi = T.gridded() ? T.hMax : h;
  auto lv = ContourLevels(s, l.grid.zMin + tlo, l.grid.zMax + thi);
  wxString key = wxString::Format("%c %.3f %u", s.contourSmooth ? 'S' : 'R', h,
                                  T.gridded() ? T.version : 0u);
  for (float z : lv) key << wxString::Format(" %.3f", z);
  ContourCache& c = m_contours[std::make_pair((const void*)&l, lev)];
  if (c.key != key) {
    if (T.gridded()) {
      const GridLevel& g0 = l.grid.levels[lev];
      GridLevel g = g0;
      g.shade.clear();
      for (int j = 0; j < g.ny; ++j) {
        const double la = g.latN - (j + 0.5) * g.dlat;
        for (int i = 0; i < g.nx; ++i) {
          float& z = g.z[(size_t)j * g.nx + i];
          if (!std::isnan(z)) z += T.at(g.lonW + (i + 0.5) * g.dlon, la);
        }
      }
      c.lines = ComputeIsolines(g, lv);
    } else {
      std::vector<float> lz(lv);
      for (float& z : lz) z -= h;
      c.lines = ComputeIsolines(l.grid.levels[lev], lz);
      for (auto& il : c.lines) il.level += h;  // valeur affichée
    }
    if (s.contourSmooth)
      for (auto& il : c.lines) SmoothIsoline(il, 2);
    c.key = key;
    // coordonnées projetées une fois pour toutes
    c.uv.clear();
    c.runs.clear();
    c.ox = c.lines.empty() || c.lines[0].pts.empty() ? 0.0 : c.lines[0].pts[0];
    c.oy = c.lines.empty() || c.lines[0].pts.empty() ? 0.0 : ScreenMapper::mercY(c.lines[0].pts[1]);
    size_t n = 0;
    for (const Isoline& il : c.lines) n += il.size();
    c.uv.reserve(2 * n);
    for (const Isoline& il : c.lines) {
      if (il.size() < 2) continue;
      ContourRun r{(uint32_t)(c.uv.size() / 2), (uint32_t)il.size(), 1e30f, 1e30f, -1e30f, -1e30f, il.level};
      for (size_t k = 0; k < il.size(); ++k) {
        const float u = (float)(il.pts[2 * k] - c.ox);
        const float v = (float)(ScreenMapper::mercY(il.pts[2 * k + 1]) - c.oy);
        c.uv.push_back(u);
        c.uv.push_back(v);
        r.x0 = std::min(r.x0, u); r.x1 = std::max(r.x1, u);
        r.y0 = std::min(r.y0, v); r.y1 = std::max(r.y1, v);
      }
      c.runs.push_back(r);
    }
  }
  return c.lines;
}

const BathyRenderer::ContourCache& BathyRenderer::Contours(const BathyLayer& l, const DisplaySettings& s) {
  const int lev = ContourLevelIndex(l);
  Isolines(l, lev, s);
  return m_contours[std::make_pair((const void*)&l, lev)];
}

// Emprise d'un tracé à l'écran (transformation affine des 4 coins)
static void RunScreenBox(const ScreenMapper& sm, double ox, double oy, float x0, float y0, float x1,
                         float y1, float& sx0, float& sy0, float& sx1, float& sy1) {
  sx0 = sy0 = 1e30f; sx1 = sy1 = -1e30f;
  const float xs[2] = {x0, x1}, ys[2] = {y0, y1};
  for (float u : xs)
    for (float v : ys) {
      const float x = (float)(sm.a * (ox + u) + sm.b * (oy + v) + sm.c);
      const float y = (float)(sm.d * (ox + u) + sm.e * (oy + v) + sm.f);
      sx0 = std::min(sx0, x); sx1 = std::max(sx1, x);
      sy0 = std::min(sy0, y); sy1 = std::max(sy1, y);
    }
}

// Valeurs posées sur les isobathes assez longues à l'écran (une tous les ~450 px)
void BathyRenderer::AddContourLabels(PlugIn_ViewPort* vp, const ScreenMapper& sm,
                                     const std::vector<LayerPtr>& layers,
                                     const DisplaySettings& s, std::vector<Label>& out) {
  if (!s.contours || !s.contourLabels) return;
  const float W = (float)vp->pix_width, H = (float)vp->pix_height;
  for (auto& lp : layers) {
    if (!lp->visible || lp->state() != BathyLayer::Ready) continue;
    const ContourCache& c = Contours(*lp, s);
    const double ax = sm.a, bx = sm.b, ay = sm.d, by = sm.e;
    const double tx = sm.a * c.ox + sm.b * c.oy + sm.c, ty = sm.d * c.ox + sm.e * c.oy + sm.f;
    for (const ContourRun& r : c.runs) {
      float bx0, by0, bx1, by1;
      RunScreenBox(sm, c.ox, c.oy, r.x0, r.y0, r.x1, r.y1, bx0, by0, bx1, by1);
      if (bx1 < 0 || by1 < 0 || bx0 > W || by0 > H) continue;  // hors écran
      const float* uv = &c.uv[2 * (size_t)r.first];
      float px = 0, py = 0, acc = 0, next = 225.f;
      bool havePrev = false;
      for (size_t k = 0; k < r.count; ++k) {
        const float x = (float)(ax * uv[2 * k] + bx * uv[2 * k + 1] + tx);
        const float y = (float)(ay * uv[2 * k] + by * uv[2 * k + 1] + ty);
        const bool in = x >= 0 && y >= 0 && x < W && y < H;
        if (havePrev && in) {
          acc += std::hypot(x - px, y - py);
          if (acc >= next) {
            out.push_back(Label{x, y, r.level, false, 0.f, true});
            next += 450.f;
          }
        }
        px = x; py = y; havePrev = in;
      }
    }
  }
}

void BathyRenderer::DrawContoursGL(PlugIn_ViewPort* vp, const ScreenMapper& sm,
                                   const std::vector<LayerPtr>& layers,
                                   const DisplaySettings& s) {
  if (!s.contours) return;
  glDisable(GL_TEXTURE_2D);
  glEnable(GL_LINE_SMOOTH);
  glHint(GL_LINE_SMOOTH_HINT, GL_NICEST);
  const float W = (float)vp->pix_width, H = (float)vp->pix_height;
  glMatrixMode(GL_MODELVIEW);
  // tracé direct (glBegin) : les tableaux de sommets liraient aussi les
  // tableaux laissés actifs par OpenCPN (textures, couleurs) -> plantage
  for (auto& lp : layers) {
    if (!lp->visible || lp->state() != BathyLayer::Ready) continue;
    const ContourCache& c = Contours(*lp, s);
    if (c.runs.empty()) continue;
    // (u, v) relatifs -> pixels : x = a·u + b·v + tx ; y = d·u + e·v + ty
    const GLdouble M[16] = {sm.a, sm.d, 0, 0, sm.b, sm.e, 0, 0, 0, 0, 1, 0,
                            sm.a * c.ox + sm.b * c.oy + sm.c, sm.d * c.ox + sm.e * c.oy + sm.f, 0, 1};
    glPushMatrix();
    glMultMatrixd(M);
    for (int style = 0; style < 3; ++style) {  // fines, maîtresses, sécurité : un état GL par passe
      bool set = false;
      for (const ContourRun& r : c.runs) {
        const int st = IsSafety(s, r.level) ? 2 : IsMajor(s, r.level) ? 1 : 0;
        if (st != style) continue;
        float x0, y0, x1, y1;
        RunScreenBox(sm, c.ox, c.oy, r.x0, r.y0, r.x1, r.y1, x0, y0, x1, y1);
        if (x1 < 0 || y1 < 0 || x0 > W || y0 > H) continue;  // hors écran
        if (x1 - x0 < 3 && y1 - y0 < 3) continue;           // bruit sous le pixel
        if (!set) {
          uint32_t col = style == 2 ? s.ramp.safetyColor : s.contourColor;
          const float width = style == 2 ? 2.6f : style == 1 ? 2.0f : 1.1f;
          const float alpha = style == 2 ? 1.f : style == 1 ? 0.95f : 0.8f;
          glColor4ub(R_(col), G_(col), B_(col), (GLubyte)(255 * alpha));
          glLineWidth(width);
          set = true;
        }
        const float* uv = &c.uv[2 * (size_t)r.first];
        glBegin(GL_LINE_STRIP);
        for (uint32_t k = 0; k < r.count; ++k) glVertex2f(uv[2 * k], uv[2 * k + 1]);
        glEnd();
      }
    }
    glPopMatrix();
  }
  glDisable(GL_LINE_SMOOTH);
  glEnable(GL_TEXTURE_2D);
}

void BathyRenderer::DrawContoursDC(wxDC& dc, PlugIn_ViewPort* vp, const ScreenMapper& sm,
                                   const std::vector<LayerPtr>& layers,
                                   const DisplaySettings& s) {
  if (!s.contours) return;
  const int W = vp->pix_width, H = vp->pix_height;
  std::vector<wxPoint> pts;
  for (auto& lp : layers) {
    if (!lp->visible || lp->state() != BathyLayer::Ready) continue;
    for (const Isoline& il : Isolines(*lp, ContourLevelIndex(*lp), s)) {
      pts.resize(il.size());
      int x0 = INT_MAX, y0 = INT_MAX, x1 = INT_MIN, y1 = INT_MIN;
      for (size_t k = 0; k < il.size(); ++k) {
        float x, y;
        sm.map(il.pts[2 * k], il.pts[2 * k + 1], x, y);
        pts[k] = wxPoint((int)std::lround(x), (int)std::lround(y));
        x0 = std::min(x0, pts[k].x); x1 = std::max(x1, pts[k].x);
        y0 = std::min(y0, pts[k].y); y1 = std::max(y1, pts[k].y);
      }
      if (x1 < 0 || y1 < 0 || x0 > W || y0 > H) continue;
      if (x1 - x0 < 3 && y1 - y0 < 3) continue;
      uint32_t c = s.contourColor;
      int width = 1;
      if (IsSafety(s, il.level)) { c = s.ramp.safetyColor; width = 3; }
      else if (IsMajor(s, il.level)) width = 2;
      dc.SetPen(wxPen(wxColour(R_(c), G_(c), B_(c)), width));
      dc.DrawLines((int)pts.size(), pts.data());
    }
  }
}

wxString BathyRenderer::LabelKey(PlugIn_ViewPort* vp, const std::vector<LayerPtr>& layers,
                                 const DisplaySettings& s, char mode, bool center) const {
  int ready = 0;
  for (auto& l : layers) ready += (l->visible && l->state() == BathyLayer::Ready);
  return wxString::Format("%c %.9f %.9f %.6g %.6f %d %d %u %d %d %d %d %d %d %u %d %d %u",
                          mode, center ? vp->clat : 0.0, center ? vp->clon : 0.0, vp->view_scale_ppm,
                          vp->rotation + vp->skew, vp->pix_width, vp->pix_height, m_epoch,
                          ready, (int)layers.size(), s.soundings ? 1 : 0, s.spacingPx,
                          s.decimals, s.fontSize, s.version, s.contours ? 1 : 0,
                          s.contourLabels ? 1 : 0, s.contourVersion) +
         wxString::Format(" %.3f %u", TideOf(s).h0, TideOf(s).version);
}

void BathyRenderer::RenderCursorGL(PlugIn_ViewPort* vp, const wxString& text, int fontSize) {
  if (text.empty() || !m_curOn) return;
  GLHostState hostState(vp->pix_width, vp->pix_height);
  glEnable(GL_TEXTURE_2D);
  glEnable(GL_BLEND);
  glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
  glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_MODULATE);
  BuildGlyphs(fontSize);
  DrawCursorGL(vp, text);
}

// Bandeau seul (imagerie sans couche de bathymétrie affichée)
void BathyRenderer::RenderBannerGL(PlugIn_ViewPort* vp, const DisplaySettings& s) {
  if (!s.banner) { m_banner.valid = false; return; }
  GLHostState hostState(vp->pix_width, vp->pix_height);
  glEnable(GL_TEXTURE_2D);
  glEnable(GL_BLEND);
  glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
  glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_MODULATE);
  BuildGlyphs(s.fontSize);
  DrawBannerGL(vp, s);
}

void BathyRenderer::RenderBannerDC(wxDC& dc, PlugIn_ViewPort* vp, const DisplaySettings& s) {
  if (!s.banner) { m_banner.valid = false; return; }
  dc.SetFont(wxFont(s.fontSize, wxFONTFAMILY_SWISS, wxFONTSTYLE_NORMAL, wxFONTWEIGHT_BOLD));
  DrawBannerDC(dc, vp, s);
}

void BathyRenderer::RenderCursorDC(wxDC& dc, PlugIn_ViewPort* vp, const wxString& text,
                                   int fontSize) {
  if (text.empty() || !m_curOn) return;
  dc.SetFont(wxFont(fontSize, wxFONTFAMILY_SWISS, wxFONTSTYLE_NORMAL, wxFONTWEIGHT_BOLD));
  DrawCursorDC(dc, vp, text);
}

// Étiquette de lecture placée à côté du curseur, gardée dans l'écran
bool BathyRenderer::CursorBox(PlugIn_ViewPort* vp, int w, int h, int& x, int& y) const {
  if (!m_curOn) return false;
  wxPoint p;
  GetCanvasPixLL(vp, &p, m_curLat, m_curLon);
  if (p.x < 0 || p.y < 0 || p.x >= vp->pix_width || p.y >= vp->pix_height) return false;
  x = p.x + 18; y = p.y + 18;
  if (x + w > vp->pix_width) x = p.x - 18 - w;
  if (y + h > vp->pix_height) y = p.y - 18 - h;
  return true;
}

void BathyRenderer::DrawCursorGL(PlugIn_ViewPort* vp, const wxString& text) {
  const int w = (int)GLTextWidth(text, 1.f) + 14, h = m_glyphs->lineH + 8;
  int x, y;
  if (!CursorBox(vp, w, h, x, y)) return;
  glDisable(GL_TEXTURE_2D);
  glColor4f(1, 1, 1, 0.92f);
  glBegin(GL_QUADS);
  glVertex2i(x, y); glVertex2i(x + w, y); glVertex2i(x + w, y + h); glVertex2i(x, y + h);
  glEnd();
  glColor4f(R_(kInk) / 255.f, G_(kInk) / 255.f, B_(kInk) / 255.f, 1);
  glLineWidth(1.f);
  glBegin(GL_LINE_LOOP);
  glVertex2f(x + 0.5f, y + 0.5f); glVertex2f(x + w - 0.5f, y + 0.5f);
  glVertex2f(x + w - 0.5f, y + h - 0.5f); glVertex2f(x + 0.5f, y + h - 0.5f);
  glEnd();
  glEnable(GL_TEXTURE_2D);
  glColor4f(1, 1, 1, 1);
  DrawGLText(x + 7.f, y + h / 2.f, text, 1.f, false);
}

void BathyRenderer::DrawCursorDC(wxDC& dc, PlugIn_ViewPort* vp, const wxString& text) {
  wxSize t = dc.GetTextExtent(text);
  const int w = t.x + 14, h = t.y + 8;
  int x, y;
  if (!CursorBox(vp, w, h, x, y)) return;
  dc.SetPen(wxPen(wxColour(R_(kInk), G_(kInk), B_(kInk)), 1));
  dc.SetBrush(*wxWHITE_BRUSH);
  dc.DrawRectangle(x, y, w, h);
  dc.SetTextForeground(wxColour(R_(kInk), G_(kInk), B_(kInk)));
  dc.DrawText(text, x + 7, y + 4);
}

// ------------------------------------------------- bandeau de palette
// Primitives de dessin du bandeau (OpenGL ou wxDC)
struct BannerPainter {
  virtual ~BannerPainter() = default;
  virtual int textW(const wxString& t) = 0;
  virtual int lineH() = 0;
  virtual void fill(const wxRect& r, uint32_t c, float alpha, int radius) = 0;
  // align : 0 x = bord gauche, 1 x = centre, 2 x = bord droit ; cy = ligne médiane
  virtual void text(int x, float cy, const wxString& t, int align) = 0;
  virtual void gear(const wxRect& r, uint32_t bg) = 0;
};

#ifndef GL_TRIANGLE_FAN
#define GL_TRIANGLE_FAN 0x0006
#endif
static const double kPiB = 3.14159265358979323846;
// Roue dentée : 8 dents (polygone étoilé autour du centre)
static std::vector<wxPoint2DDouble> GearShape(double cx, double cy, double r) {
  std::vector<wxPoint2DDouble> p;
  const int n = 8 * 4;
  for (int k = 0; k < n; ++k) {
    const double a = (k + 0.5) * 2 * kPiB / n;
    const double rr = (k % 4 < 2) ? r : r * 0.7;
    p.emplace_back(cx + rr * std::cos(a), cy + rr * std::sin(a));
  }
  return p;
}

// Horizontal :  marée (hauteur, date courte) sur une ligne en tête, puis
//               [min]  (A)(Vue)  [max unité] [⇕] ⚙   puis la rampe.
// Vertical :    marée sur deux lignes en tête (hauteur / date)
//               [⇔]        ⚙
//                (A)(Vue)
//                 min
//                 ▇ – seuils
//                 ▇ – …
//                 max unité
// Bandeau : une ou deux sections. La première (bathymétrie, ou imagerie seule)
// porte la marée, l'orientation et la roue ; l'imagerie, quand les deux
// couches sont affichées, s'ajoute en dessous avec sa rampe de gris.
// Icône isobathes / sondes (menu) près de l'orientation et de la roue ;
// seule avec elles quand les couleurs de profondeur sont masquées.
void BathyRenderer::DrawBanner(BannerPainter& P, PlugIn_ViewPort* vp, const DisplaySettings& s) {
  m_banner = BannerHit();
  struct Sec {
    bool img = false;
    wxString minT, maxT, modeT, srcT;
    bool autoOn = false;
    std::function<uint32_t(float)> color;  // t de 0 (min) à 1 (max)
    float safetyT = -1;
    uint32_t safetyColor = 0;
    std::vector<std::pair<float, wxString>> ticks;  // seuils intermédiaires (t, texte)
    wxRect minR, maxR, modeR, srcR, barR;
  };
  std::vector<Sec> secs;
  const auto& st = s.ramp.stops;
  if (s.bannerBathy && !st.empty()) {
    Sec b;
    const ColorRamp& ramp = s.ramp;
    const float lo = st.front().depth, hi = st.back().depth;
    b.minT = DepthText(s, lo);
    b.maxT = DepthText(s, hi) + " " + s.depthUnit;
    b.modeT = s.autoPalette ? "A" : "M";
    b.autoOn = s.autoPalette;
    b.srcT = s.autoPalette ? wxString::FromUTF8(pd::tr(s.autoSource == 1 ? "Vue" : "Tout")) : wxString();
    b.color = [&ramp, lo, hi](float t) { return ramp.colorAt(hi > lo ? lo + (hi - lo) * t : lo); };
    if (ramp.safetyOn && hi > lo && ramp.safetyDepth > lo && ramp.safetyDepth < hi) {
      b.safetyT = (ramp.safetyDepth - lo) / (hi - lo);
      b.safetyColor = ramp.safetyColor;
    }
    if (hi > lo)
      for (size_t i = 1; i + 1 < st.size(); ++i) b.ticks.push_back({(st[i].depth - lo) / (hi - lo), DepthText(s, st[i].depth)});
    secs.push_back(b);
  }
  ColorRamp gray;
  if (s.bannerImg) {
    Sec g;
    g.img = true;
    gray = ImageryRamp(s);
    const float lo = std::max(0, std::min(99, s.imgMin)) / 100.f;
    const float hi = std::max(lo + 0.01f, std::max(1, std::min(100, s.imgMax)) / 100.f);
    g.minT = wxString::Format("%d %%", (int)std::lround(lo * 100));
    g.maxT = wxString::Format("%d %%", (int)std::lround(hi * 100));
    g.modeT = s.imgAuto ? "A" : "M";
    g.autoOn = s.imgAuto;
    g.srcT = s.imgAuto ? wxString::FromUTF8(pd::tr(s.imgAutoSource == 1 ? "Vue" : "Tout")) : wxString();
    g.color = [&gray, lo, hi](float t) { return gray.colorAt(lo + (hi - lo) * t); };
    secs.push_back(g);
  }
  // icône isobathes / sondes (menu) : dès que des profondeurs sont chargées ;
  // seule (avec l'orientation et la roue) quand les couleurs sont masquées
  const bool chart = s.bannerChart;
  if (secs.empty() && !chart) return;

  const int lineH = P.lineH();
  // marée : hauteur et date courte (horizontal : une ligne ; vertical : deux)
  const bool tideOn = s.bannerBathy && !s.tideLegend.empty();
  const wxString tide = tideOn ? s.tideLegend + "  " + s.tideWhen : wxString();
  const wxString tide1 = tideOn ? s.tideLegend : wxString(), tide2 = tideOn ? s.tideWhen : wxString();
  const bool vert = s.bannerVertical;
  const int pad = 8, gap = 16, barT = 14;  // barT : épaisseur de la rampe
  const int rowH = lineH + 6, icon = lineH + 2;
  const int badgeW = std::max(P.textW("M") + 12, rowH);
  const int srcW = std::max(P.textW("Tout"), P.textW(wxString::FromUTF8(pd::tr("Tout")))) + 12;
  auto midW = [&](const Sec& c) { return badgeW + (c.srcT.empty() ? 0 : 3 + srcW); };
  const int W = vp->pix_width, H = vp->pix_height;

  // ---- mise en page relative (origine 0,0 du bandeau)
  wxRect gearR, orientR, chartR;
  int boxW = 0, boxH = 0, tideH = 0;
  struct Tick { int pos; wxString t; size_t sec; };
  std::vector<Tick> ticks;
  std::vector<int> seps;  // séparation entre sections (y relatif, ou x en horizontal)
  if (!vert) {
    int leftNeed = 0, rightNeed = 0, mid = 0;
    for (const auto& c : secs) {
      leftNeed = std::max(leftNeed, pad + std::max(P.textW(c.minT), P.textW("-000.0")) + gap);
      rightNeed = std::max(rightNeed, gap + std::max(P.textW(c.maxT), P.textW("000.0 ft")) + gap +
                                          (chart ? icon + 8 : 0) + icon + 8 + icon + pad);
      mid = std::max(mid, midW(c));
    }
    boxW = secs.empty() ? 0 : std::max(380, mid + 2 * std::max(leftNeed, rightNeed));
    if (!tide.empty()) boxW = std::max(boxW, P.textW(tide) + 2 * pad);
    if (secs.empty()) boxW = pad + 3 * icon + 16 + pad;  // icônes seules
    tideH = tide.empty() ? 0 : lineH + 2;
    int y = pad + tideH;
    for (size_t k = 0; k < secs.size(); ++k) {
      Sec& c = secs[k];
      if (k) { seps.push_back(y + 2); y += 8; }
      const int bx = (boxW - midW(c)) / 2;
      c.modeR = wxRect(bx, y, badgeW, rowH);
      if (!c.srcT.empty()) c.srcR = wxRect(bx + badgeW + 3, y, srcW, rowH);
      if (k == 0) {
        gearR = wxRect(boxW - pad - icon, y + (rowH - icon) / 2, icon, icon);
        orientR = wxRect(gearR.x - 8 - icon, gearR.y, icon, icon);
        if (chart) chartR = wxRect(orientR.x - 8 - icon, gearR.y, icon, icon);
      }
      const int rightEdge = k == 0 ? (chart ? chartR.x : orientR.x) - 6 : boxW - pad;
      c.minR = wxRect(2, y - 2, bx - 6, rowH + 4);
      c.maxR = wxRect(bx + midW(c) + 4, y - 2, rightEdge - (bx + midW(c) + 4), rowH + 4);
      c.barR = wxRect(pad, y + rowH + 6, boxW - 2 * pad, barT);
      y += rowH + 6 + barT;
    }
    if (secs.empty()) {  // icônes seules : isobathes, orientation, roue
      gearR = wxRect(boxW - pad - icon, y + (rowH - icon) / 2, icon, icon);
      orientR = wxRect(gearR.x - 8 - icon, gearR.y, icon, icon);
      chartR = wxRect(orientR.x - 8 - icon, gearR.y, icon, icon);
      y += rowH;
    }
    boxH = y + pad;
  } else {
    const int barLen0 = std::max(140, std::min(320, H * 35 / 100));
    int labW = 0;
    for (const auto& c : secs) {
      for (const auto& t : c.ticks) labW = std::max(labW, P.textW(t.second));
      boxW = std::max({boxW, pad + midW(c) + pad, P.textW(c.minT) + 2 * pad, P.textW(c.maxT) + 2 * pad});
    }
    boxW = std::max({boxW, pad + icon + 12 + icon + (chart ? 12 + icon : 0) + pad, pad + barT + 8 + labW + pad, 96,
                     tide1.empty() ? 0 : P.textW(tide1) + 2 * pad, tide2.empty() ? 0 : P.textW(tide2) + 2 * pad});
    int y = pad;
    if (!tide1.empty()) {  // marée en tête du bandeau, sur deux lignes
      tideH = 2 * (lineH + 2) + 4;
      y += tideH;
    }
    orientR = wxRect(pad, y, icon, icon);
    gearR = wxRect(boxW - pad - icon, y, icon, icon);
    if (chart) chartR = wxRect((orientR.GetRight() + gearR.x - icon) / 2, y, icon, icon);
    y += icon + 6;
    for (size_t k = 0; k < secs.size(); ++k) {
      Sec& c = secs[k];
      // deux sections : la rampe de l'imagerie est plus courte
      const int barLen = secs.size() > 1 && c.img ? std::max(100, barLen0 * 6 / 10) : barLen0;
      if (k) { seps.push_back(y); y += 8; }
      const int bx = (boxW - midW(c)) / 2;
      c.modeR = wxRect(bx, y, badgeW, rowH);
      if (!c.srcT.empty()) c.srcR = wxRect(bx + badgeW + 3, y, srcW, rowH);
      y += rowH + 4;
      c.minR = wxRect(2, y, boxW - 4, lineH + 4);
      y += lineH + 6;
      c.barR = wxRect(pad, y, barT, barLen);
      int last = -1000;
      for (const auto& t : c.ticks) {  // seuils espacés d'au moins une ligne
        const int pos = (int)std::lround(barLen * t.first);
        if (pos - last < lineH + 2 || pos < lineH / 2 + 2 || barLen - pos < lineH / 2 + 2) continue;
        ticks.push_back({y + pos, t.second, k});
        last = pos;
      }
      y += barLen + 4;
      c.maxR = wxRect(2, y, boxW - 4, lineH + 4);
      y += lineH + 4;
    }
    boxH = y + pad - 4;
  }
  // ---- position sur la carte : hors de la marge où OpenCPN fait défiler la
  // carte (5 % de la largeur), de l'échelle (bas gauche) et du zoom (bas droite)
  const int m = std::max(24, W * 5 / 100 + 10);
  int x, y;
  switch (s.bannerPos) {
    case 1: x = (W - boxW) / 2; y = m; break;               // haut, centré
    case 2: x = m; y = H - m - boxH; break;                  // bas gauche
    case 3:                                                  // libre (déplacé à la souris)
      x = (int)std::lround(s.bannerFx * W - boxW / 2.0);
      y = (int)std::lround(s.bannerFy * H - boxH / 2.0);
      break;
    default: x = (W - boxW) / 2; y = H - m - boxH; break;   // bas, centré
  }
  x = std::max(0, std::min(x, W - boxW));
  y = std::max(0, std::min(y, H - boxH));
  auto mv = [&](wxRect& r) { if (!r.IsEmpty()) r.Offset(x, y); };
  for (auto& c : secs) { mv(c.minR); mv(c.maxR); mv(c.modeR); mv(c.srcR); mv(c.barR); }
  mv(chartR);
  mv(gearR);
  mv(orientR);
  for (auto& t : ticks) t.pos += y;
  const wxRect box(x, y, boxW, boxH);

  // ---- dessin
  P.fill(box, kBannerBg, 0.86f, 5);
  for (int sp : seps)
    P.fill(vert ? wxRect(x + pad, y + sp, boxW - 2 * pad, 1) : wxRect(x + pad, y + sp, boxW - 2 * pad, 1),
           RGBA(90, 100, 116), 1.f, 0);
  for (const auto& c : secs) {
    P.fill(c.modeR, c.autoOn ? kBadgeAuto : kBadgeMan, 1.f, 3);
    if (!c.srcR.IsEmpty()) P.fill(c.srcR, kBadgeSrc, 1.f, 3);
    // rampe : une tranche par pixel, couleur exacte de la palette
    const wxRect& r = c.barR;
    const int len = vert ? r.height : r.width;
    for (int i = 0; i < len; ++i) {
      const wxRect sl = vert ? wxRect(r.x, r.y + i, r.width, 1) : wxRect(r.x + i, r.y, 1, r.height);
      P.fill(sl, c.color((i + 0.5f) / len), 1.f, 0);
    }
    if (c.safetyT >= 0) {  // profondeur de sécurité : repère sur la rampe
      const int q = (int)(len * c.safetyT);
      if (vert) {
        P.fill(wxRect(r.x - 3, r.y + q - 1, r.width + 6, 3), RGBA(255, 255, 255), 1.f, 0);
        P.fill(wxRect(r.x - 2, r.y + q, r.width + 4, 1), c.safetyColor, 1.f, 0);
      } else {
        P.fill(wxRect(r.x + q - 1, r.y - 3, 3, r.height + 6), RGBA(255, 255, 255), 1.f, 0);
        P.fill(wxRect(r.x + q, r.y - 2, 1, r.height + 4), c.safetyColor, 1.f, 0);
      }
    }
  }
  for (const auto& t : ticks)
    P.fill(wxRect(secs[t.sec].barR.GetRight() + 2, t.pos, 4, 1), RGBA(220, 225, 232), 1.f, 0);
  {  // bouton d'orientation : petite rampe dans l'autre sens (couleurs de la 1re section)
    const wxRect& o = orientR;
    const wxRect ic = vert ? wxRect(o.x, o.y + o.height / 2 - 3, o.width, 7)    // vers horizontal
                           : wxRect(o.x + o.width / 2 - 3, o.y, 7, o.height);  // vers vertical
    P.fill(wxRect(ic.x - 1, ic.y - 1, ic.width + 2, ic.height + 2), RGBA(220, 225, 232), 1.f, 0);
    const int n = vert ? ic.width : ic.height;
    const ColorRamp& pr = s.ramp;
    const float plo = st.empty() ? 0.f : st.front().depth, phi = st.empty() ? 1.f : st.back().depth;
    for (int i = 0; i < n; ++i) {
      const float t = (i + 0.5f) / n;
      P.fill(vert ? wxRect(ic.x + i, ic.y, 1, ic.height) : wxRect(ic.x, ic.y + i, ic.width, 1),
             !secs.empty() ? secs[0].color(t) : pr.colorAt(plo + (phi - plo) * t), 1.f, 0);
    }
  }
  P.gear(gearR, kBannerBg);
  // textes
  if (tideH) {
    if (!vert) P.text(x + pad, y + pad + lineH / 2.f, tide, 0);
    else {
      P.text(x + pad, y + pad + lineH / 2.f, tide1, 0);
      P.text(x + pad, y + pad + lineH + 2 + lineH / 2.f, tide2, 0);
    }
  }
  for (size_t k = 0; k < secs.size(); ++k) {
    const Sec& c = secs[k];
    const float cy = c.modeR.y + c.modeR.height / 2.f;
    P.text(c.modeR.x + c.modeR.width / 2, cy, c.modeT, 1);
    if (!c.srcR.IsEmpty()) P.text(c.srcR.x + c.srcR.width / 2, cy, c.srcT, 1);
    if (!vert) {
      P.text(x + pad, cy, c.minT, 0);
      P.text(k == 0 ? (chartR.IsEmpty() ? orientR.x : chartR.x) - 10 : x + boxW - pad, cy, c.maxT, 2);
    } else {
      P.text(x + pad, c.minR.y + c.minR.height / 2.f, c.minT, 0);
      P.text(x + pad, c.maxR.y + c.maxR.height / 2.f, c.maxT, 0);
    }
  }
  for (const auto& t : ticks) P.text(secs[t.sec].barR.GetRight() + 8, (float)t.pos, t.t, 0);
  if (!chartR.IsEmpty()) {  // icône isobathes : anneaux concentriques (clairs si affichées)
    const uint32_t on = s.contours || s.soundings ? RGBA(220, 225, 232) : RGBA(120, 130, 146);
    const wxRect& c = chartR;
    const int r0 = std::min(c.width, c.height);
    for (int k = 0; k < 3; ++k) {
      const int inset = k * r0 / 5;
      const wxRect ring(c.x + inset, c.y + inset, r0 - 2 * inset, r0 - 2 * inset);
      if (ring.width < 3) break;
      P.fill(ring, on, 1.f, ring.width / 2);
      if (ring.width > 4)
        P.fill(wxRect(ring.x + 1, ring.y + 1, ring.width - 2, ring.height - 2), kBannerBg, 1.f, ring.width / 2 - 1);
    }
    P.fill(wxRect(c.x + r0 / 2 - 1, c.y + r0 / 2 - 1, 2, 2), on, 1.f, 0);
  }

  // ---- zones cliquables
  m_banner.valid = true;
  m_banner.box = box;
  for (const auto& c : secs) {
    BannerHit::Zones& z = c.img ? m_banner.img : m_banner.bathy;
    z.on = true;
    z.minR = c.minR;
    z.maxR = c.maxR;
    z.modeR = c.modeR;
    z.srcR = c.srcR;
    z.barR = vert ? wxRect(c.barR.x - 3, c.barR.y, boxW - 2 * pad + 3, c.barR.height)  // rampe et étiquettes
                  : wxRect(c.barR.x, c.barR.y - 3, c.barR.width, c.barR.height + 6);
  }
  m_banner.chartR = chartR.IsEmpty() ? wxRect() : wxRect(chartR.x - 3, chartR.y - 3, chartR.width + 6, chartR.height + 6);
  m_banner.gearR = wxRect(gearR.x - 4, gearR.y - 4, gearR.width + 8, gearR.height + 8);
  m_banner.orientR = wxRect(orientR.x - 3, orientR.y - 3, orientR.width + 6, orientR.height + 6);
  m_banner.pixW = W;
  m_banner.pixH = H;
}

namespace {
struct GLBannerPainter : BannerPainter {
  BathyRenderer& R;
  std::function<void(float, float, const wxString&, bool)> draw;
  std::function<float(const wxString&)> width;
  int lh;
  GLBannerPainter(BathyRenderer& r, std::function<void(float, float, const wxString&, bool)> d,
                  std::function<float(const wxString&)> w, int l)
      : R(r), draw(std::move(d)), width(std::move(w)), lh(l) {}
  int textW(const wxString& t) override { return (int)width(t); }
  int lineH() override { return lh; }
  void fill(const wxRect& r, uint32_t c, float a, int) override {
    glDisable(GL_TEXTURE_2D);
    glColor4f(R_(c) / 255.f, G_(c) / 255.f, B_(c) / 255.f, a);
    glBegin(GL_QUADS);
    glVertex2f(r.x, r.y); glVertex2f(r.x + r.width, r.y);
    glVertex2f(r.x + r.width, r.y + r.height); glVertex2f(r.x, r.y + r.height);
    glEnd();
  }
  void text(int x, float cy, const wxString& t, int align) override {
    glEnable(GL_TEXTURE_2D);
    glColor4f(1, 1, 1, 1);
    if (align == 1) draw((float)x, cy, t, true);
    else draw(align == 2 ? x - width(t) : (float)x, cy, t, false);
  }
  void gear(const wxRect& r, uint32_t bg) override {
    glDisable(GL_TEXTURE_2D);
    glColor4f(0.85f, 0.88f, 0.92f, 1);
    const double gr = r.width / 2.0, gx = r.x + gr, gy = r.y + gr;
    const auto g = GearShape(gx, gy, gr);
    glBegin(GL_TRIANGLE_FAN);
    glVertex2d(gx, gy);
    for (const auto& p : g) glVertex2d(p.m_x, p.m_y);
    glVertex2d(g.front().m_x, g.front().m_y);
    glEnd();
    glColor4f(R_(bg) / 255.f, G_(bg) / 255.f, B_(bg) / 255.f, 1);
    glBegin(GL_TRIANGLE_FAN);
    glVertex2d(gx, gy);
    for (int k = 0; k <= 16; ++k)
      glVertex2d(gx + gr * 0.32 * std::cos(k * kPiB / 8), gy + gr * 0.32 * std::sin(k * kPiB / 8));
    glEnd();
  }
};

struct DCBannerPainter : BannerPainter {
  wxDC& dc;
  explicit DCBannerPainter(wxDC& d) : dc(d) {}
  static wxColour wc(uint32_t c) { return wxColour(R_(c), G_(c), B_(c)); }
  int textW(const wxString& t) override { return dc.GetTextExtent(t).x; }
  int lineH() override { return dc.GetTextExtent("0").y + 2; }
  void fill(const wxRect& r, uint32_t c, float, int radius) override {
    dc.SetPen(*wxTRANSPARENT_PEN);
    dc.SetBrush(wxBrush(wc(c)));
    if (radius > 0) dc.DrawRoundedRectangle(r, radius);
    else dc.DrawRectangle(r);
  }
  void text(int x, float cy, const wxString& t, int align) override {
    const wxSize e = dc.GetTextExtent(t);
    const int tx = align == 1 ? x - e.x / 2 : align == 2 ? x - e.x : x;
    dc.SetTextForeground(*wxWHITE);
    dc.DrawText(t, tx, (int)std::lround(cy - e.y / 2.0));
  }
  void gear(const wxRect& r, uint32_t bg) override {
    const double gr = r.width / 2.0, gx = r.x + gr, gy = r.y + gr;
    std::vector<wxPoint> gp;
    for (const auto& p : GearShape(gx, gy, gr))
      gp.emplace_back((int)std::lround(p.m_x), (int)std::lround(p.m_y));
    dc.SetPen(*wxTRANSPARENT_PEN);
    dc.SetBrush(wxBrush(wxColour(217, 224, 235)));
    dc.DrawPolygon((int)gp.size(), gp.data());
    dc.SetBrush(wxBrush(wc(bg)));
    dc.DrawCircle((int)std::lround(gx), (int)std::lround(gy), (int)std::lround(gr * 0.32));
  }
};
}  // namespace

void BathyRenderer::DrawBannerGL(PlugIn_ViewPort* vp, const DisplaySettings& s) {
  GLBannerPainter P(
      *this, [this](float x, float y, const wxString& t, bool c) { DrawGLText(x, y, t, 1.f, c); },
      [this](const wxString& t) { return GLTextWidth(t, 1.f); }, m_glyphs->lineH);
  m_glLight = true;
  DrawBanner(P, vp, s);
  m_glLight = false;
  glDisable(GL_TEXTURE_2D);
}

void BathyRenderer::DrawBannerDC(wxDC& dc, PlugIn_ViewPort* vp, const DisplaySettings& s) {
  DCBannerPainter P(dc);
  DrawBanner(P, vp, s);
}

// ---------------------------------------------------------------- wxDC
void BathyRenderer::RenderDC(wxDC& dc, PlugIn_ViewPort* vp, const std::vector<LayerPtr>& layers,
                             const DisplaySettings& s, const wxString& cursorText, int parts) {
  if (!PrepareLUT(layers, s)) return;
  ScreenMapper sm;
  if (!FitMapper(vp, sm)) return;
  const int W = vp->pix_width, H = vp->pix_height;
  if (W <= 0 || H <= 0) return;
  int ready = 0;
  for (auto& l : layers) ready += (l->visible && l->state() == BathyLayer::Ready);
  wxString key = wxString::Format("%.8f %.8f %.6f %.5f %d %d %u %u %d %d %d", vp->clat, vp->clon,
                                  vp->view_scale_ppm, vp->rotation + vp->skew, W, H, m_lutStamp,
                                  m_epoch, ready, s.opacity, s.smooth ? 1 : 0);
  if ((parts & kTiles) && (key != m_dcKey || !m_dcBitmap.IsOk())) {
    // Grille de points de contrôle : lat/lon interpolées entre eux (exact à <1 px)
    const int step = 32, cw = W / step + 2, ch = H / step + 2;
    std::vector<double> cl((size_t)cw * ch), cp((size_t)cw * ch);
    for (int j = 0; j < ch; ++j)
      for (int i = 0; i < cw; ++i)
        GetCanvasLLPix(vp, wxPoint(i * step, j * step), &cp[(size_t)j * cw + i],
                       &cl[(size_t)j * cw + i]);
    wxImage img(W, H);
    img.InitAlpha();
    unsigned char* rgb = img.GetData();
    unsigned char* al = img.GetAlpha();
    std::fill(al, al + (size_t)W * H, 0);
    const float op = s.opacity / 100.f, k = s.hillshade ? s.shadeStrength : 0.f;
    for (auto& l : layers) {
      if (!l->visible || l->state() != BathyLayer::Ready) continue;
      const GridLevel& g = l->grid.levels[l->grid.levelFor(m_ppd, 1.0)];
      const bool sh = k > 0 && g.shade.size() == g.z.size();
      for (int y = 0; y < H; ++y) {
        int j = y / step; float fy = (y - j * step) / (float)step;
        for (int x = 0; x < W; ++x) {
          int i = x / step; float fx = (x - i * step) / (float)step;
          size_t a = (size_t)j * cw + i;
          double lon = (cl[a] * (1 - fx) + cl[a + 1] * fx) * (1 - fy) +
                       (cl[a + cw] * (1 - fx) + cl[a + cw + 1] * fx) * fy;
          double lat = (cp[a] * (1 - fx) + cp[a + 1] * fx) * (1 - fy) +
                       (cp[a + cw] * (1 - fx) + cp[a + cw + 1] * fx) * fy;
          int ci, cj;
          if (!g.cellOf(lon, lat, ci, cj)) continue;
          size_t c = (size_t)cj * g.nx + ci;
          float z = g.z[c];
          if (std::isnan(z)) continue;
          float shv = sh ? (float)g.shade[c] : 160.f;
          if (s.smooth) {  // interpolation bilinéaire de la profondeur et de l'ombrage
            const double fx = (lon - g.lonW) / g.dlon - 0.5, fy = (g.latN - lat) / g.dlat - 0.5;
            const int i0 = (int)std::floor(fx), j0 = (int)std::floor(fy);
            const double tx = fx - i0, ty = fy - j0;
            double zs = 0, ss = 0, ws = 0;
            for (int dj = 0; dj <= 1; ++dj)
              for (int di = 0; di <= 1; ++di) {
                const int ii = i0 + di, jj = j0 + dj;
                if (ii < 0 || jj < 0 || ii >= g.nx || jj >= g.ny) continue;
                const size_t cc = (size_t)jj * g.nx + ii;
                if (std::isnan(g.z[cc])) continue;
                const double w = (di ? tx : 1 - tx) * (dj ? ty : 1 - ty);
                zs += w * g.z[cc]; ss += w * (sh ? g.shade[cc] : 160); ws += w;
              }
            if (ws > 1e-9) { z = (float)(zs / ws); shv = (float)(ss / ws); }
          }
          const TideField& TT = TideOf(s);
          uint32_t col = m_lut(z + (TT.gridded() ? TT.at(lon, lat) : TT.h0));
          float f = sh ? 1.f + k * (shv / 160.f - 1.f) : 1.f;
          size_t o = (size_t)y * W + x;
          const float li = (s.interpDistinct && g.isInterpFar(c)) ? kInterpLighten : 0.f;
          auto L = [li](float v) { return v + (255.f - v) * li; };  // zones interpolées éclaircies
          rgb[3 * o] = (unsigned char)L(std::min(255.f, R_(col) * f));
          rgb[3 * o + 1] = (unsigned char)L(std::min(255.f, G_(col) * f));
          rgb[3 * o + 2] = (unsigned char)L(std::min(255.f, B_(col) * f));
          al[o] = (unsigned char)(A_(col) * op);
        }
      }
    }
    m_dcBitmap = wxBitmap(img);
    m_dcKey = key;
  }
  if (parts & kTiles) dc.DrawBitmap(m_dcBitmap, 0, 0, true);
  if (!(parts & kOverlays)) return;

  // Sondes chiffrées
  wxFont fontBig(s.fontSize, wxFONTFAMILY_SWISS, wxFONTSTYLE_NORMAL, wxFONTWEIGHT_BOLD);
  wxFont fontSmall(std::max(5, (int)(s.fontSize * 0.72)), wxFONTFAMILY_SWISS, wxFONTSTYLE_NORMAL,
               wxFONTWEIGHT_BOLD);
  const wxColour ink(R_(kInk), G_(kInk), B_(kInk)), grey(120, 130, 150);
  auto halo = [&](const wxString& t, int x, int y, const wxColour& c) {
    dc.SetTextForeground(*wxWHITE);
    for (int dx = -1; dx <= 1; ++dx)
      for (int dy = -1; dy <= 1; ++dy)
        if (dx || dy) dc.DrawText(t, x + dx, y + dy);
    dc.SetTextForeground(c);
    dc.DrawText(t, x, y);
  };
  DrawContoursDC(dc, vp, sm, layers, s);
  wxString ip, dp;
  const wxString labelKey = LabelKey(vp, layers, s, 'D');
  if (labelKey != m_labelKey) {
    m_labels = CollectLabels(vp, sm, layers, s);
    AddContourLabels(vp, sm, layers, s, m_labels);
    int lineH = 0;
    for (Label& L : m_labels) {
      if (L.contour) {
        dc.SetFont(fontBig);
        L.w = (float)dc.GetTextExtent(DepthText(s, L.depth)).x + 4;
        continue;
      }
      FormatDepth(L.depth * s.depthK, s.decimals, ip, dp);
      dc.SetFont(fontBig);
      wxSize a = dc.GetTextExtent(ip);
      dc.SetFont(fontSmall);
      wxSize b = dp.empty() ? wxSize(0, 0) : dc.GetTextExtent(dp);
      L.w = (float)(a.x + b.x);
      lineH = std::max(lineH, a.y);
    }
    Declutter(m_labels, (float)lineH, 4.f);
    m_labelKey = labelKey;
    m_labelStatic.clear();  // pas de décalage en rendu DC
  }
  for (const Label& L : m_labels) {
    if (L.contour) {
      uint32_t c = IsSafety(s, L.depth) ? s.ramp.safetyColor : s.contourColor;
      wxString t = DepthText(s, L.depth);
      dc.SetFont(fontBig);
      wxSize e = dc.GetTextExtent(t);
      dc.SetTextForeground(wxColour(R_(c), G_(c), B_(c)));
      dc.DrawText(t, (int)L.x - e.x / 2, (int)L.y - e.y / 2);
      continue;
    }
    FormatDepth(L.depth * s.depthK, s.decimals, ip, dp);
    dc.SetFont(fontBig);
    wxSize si = dc.GetTextExtent(ip);
    dc.SetFont(fontSmall);
    wxSize sd = dp.empty() ? wxSize(0, 0) : dc.GetTextExtent(dp);
    int x0 = (int)L.x - (si.x + sd.x) / 2, y0 = (int)L.y - si.y / 2;
    const wxColour& c = L.fromGrid ? grey : ink;
    dc.SetFont(fontBig);
    halo(ip, x0, y0, c);
    if (!dp.empty()) { dc.SetFont(fontSmall); halo(dp, x0 + si.x, y0 + si.y / 3, c); }
    if (L.depth < 0) {
      dc.SetPen(wxPen(c, 1));
      dc.DrawLine(x0, y0 + si.y, x0 + si.x + sd.x, y0 + si.y);
    }
  }

  if (!cursorText.empty()) { dc.SetFont(fontBig); DrawCursorDC(dc, vp, cursorText); }

  // Bandeau de palette (remplace la légende)
  if (s.banner) { dc.SetFont(fontBig); DrawBannerDC(dc, vp, s); }
  else m_banner.valid = false;
}

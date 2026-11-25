// PersonalDepth_pi - unité des hauteurs des stations de marée d'OpenCPN (voir TideUnits.h)
//
// Format .tcd : « libtcd » (D. Flater, J. Depner, domaine public) : entête
// texte « [CLÉ] = valeur » donnant la taille en bits de chaque champ, tables,
// puis enregistrements bit à bit (bit de poids fort d'abord).
//
// Copyright (C) 2026 Marcaurelio Franzetti
// SPDX-License-Identifier: GPL-2.0-or-later
#include "TideUnits.h"

#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <map>
#include <vector>

#include "FileUtil.h"

namespace pd {
namespace {

constexpr double kNaN = std::numeric_limits<double>::quiet_NaN();

bool ReadAll(const std::string& path, std::string& d) {
  FILE* f = OpenFile(path, "rb");
  if (!f) return false;
  d.clear();
  char b[65536];
  size_t k;
  while ((k = std::fread(b, 1, sizeof b, f)) > 0) d.append(b, k);
  std::fclose(f);
  return true;
}

std::string Trim(const std::string& s) {
  size_t a = 0, b = s.size();
  while (a < b && std::isspace((unsigned char)s[a])) ++a;
  while (b > a && std::isspace((unsigned char)s[b - 1])) --b;
  return s.substr(a, b - a);
}

struct Bits {
  const std::string& d;
  uint64_t size() const { return (uint64_t)d.size() * 8; }
  bool ok(uint64_t pos, unsigned n) const { return n <= 32 && pos + n <= size(); }
  uint32_t u(uint64_t pos, unsigned n) const {
    uint32_t v = 0;
    for (unsigned k = 0; k < n; ++k) {
      const uint64_t b = pos + k;
      v = (v << 1) | (((unsigned char)d[(size_t)(b >> 3)] >> (7 - (b & 7))) & 1u);
    }
    return v;
  }
  int32_t s(uint64_t pos, unsigned n) const {
    const uint32_t v = u(pos, n);
    if (n == 0 || n >= 32) return (int32_t)v;
    return (v >> (n - 1)) & 1u ? (int32_t)(v | (~0u << n)) : (int32_t)v;
  }
  // chaîne de 8 bits terminée par un nul ; false si hors du fichier
  bool str(uint64_t& pos, std::string* out = nullptr) const {
    for (int k = 0; k < 4096; ++k) {
      if (!ok(pos, 8)) return false;
      const char c = (char)u(pos, 8);
      pos += 8;
      if (!c) return true;
      if (out) *out += c;
    }
    return false;
  }
};

}  // namespace

double TideUnitToMetres(const std::string& unit) {
  std::string u = Trim(unit);
  for (char& c : u) c = (char)std::tolower((unsigned char)c);
  if (u == "meters" || u == "metres" || u == "meter" || u == "metre" || u == "m") return 1.0;
  if (u == "feet" || u == "foot" || u == "ft") return 0.3048;
  return kNaN;
}

double TcdStationToMetres(const std::string& tcdPath, const std::string& station, double lat, double lon) {
  std::string d;
  if (station.size() < 2 || !ReadAll(tcdPath, d)) return kNaN;
  // entête texte
  std::map<std::string, long long> h;
  const size_t endHead = d.find("[END OF ASCII HEADER DATA]");
  if (endHead == std::string::npos) return kNaN;
  size_t p = 0;
  while (p < endHead) {
    size_t e = d.find('\n', p);
    if (e == std::string::npos || e > endHead) e = endHead;
    const std::string line = d.substr(p, e - p);
    const size_t a = line.find('['), b = line.find(']'), q = line.find('=');
    if (a != std::string::npos && b != std::string::npos && q != std::string::npos && b < q)
      h[line.substr(a + 1, b - a - 1)] = std::atoll(line.c_str() + q + 1);
    p = e + 1;
  }
  auto key = [&](const char* k) { auto it = h.find(k); return it == h.end() ? -1LL : it->second; };
  const long long major = key("MAJOR REV"), headerSize = key("HEADER SIZE");
  const long long nUnits = key("LEVEL UNIT TYPES"), unitSize = key("LEVEL UNIT SIZE");
  const int rsb = (int)key("RECORD SIZE BITS"), rtb = (int)key("RECORD TYPE BITS"), latb = (int)key("LATITUDE BITS"),
            lonb = (int)key("LONGITUDE BITS"), tzb = (int)key("TZFILE BITS"), stb = (int)key("STATION BITS"),
            cob = (int)key("COUNTRY BITS"), reb = (int)key("RESTRICTION BITS"), leb = (int)key("LEGALESE BITS"),
            dab = (int)key("DATE BITS"), dub = (int)key("DIRECTION UNIT BITS"), dib = (int)key("DIRECTION BITS"),
            lub = (int)key("LEVEL UNIT BITS"), peb = (int)key("PEDIGREE BITS");
  const double latScale = (double)key("LATITUDE SCALE"), lonScale = (double)key("LONGITUDE SCALE");
  if (headerSize <= 0 || nUnits <= 0 || nUnits > 64 || unitSize <= 0 || unitSize > 256 || rsb < 0 || rtb < 0 ||
      latb <= 0 || lonb <= 0 || tzb < 0 || stb < 0 || cob < 0 || reb < 0 || lub <= 0 || !(latScale > 0) ||
      !(lonScale > 0) || (major >= 2 && (leb < 0 || dab < 0 || dub < 0 || dib < 0)) || (major < 2 && peb < 0))
    return kNaN;
  // noms des unités de niveau (juste après l'entête et sa somme de contrôle)
  std::vector<std::string> units;
  const size_t u0 = (size_t)headerSize + 4;
  if (u0 + (size_t)(nUnits * unitSize) > d.size()) return kNaN;
  for (long long k = 0; k < nUnits; ++k) {
    const char* c = d.data() + u0 + (size_t)(k * unitSize);
    units.emplace_back(c, strnlen(c, (size_t)unitSize));
  }
  // le nom (suivi de son nul) cherché aux 8 décalages de bit possibles
  const Bits B{d};
  std::string pat = station;
  pat += '\0';
  std::string sh(d.size(), '\0');
  for (int s = 0; s < 8; ++s) {
    const std::string* hay = &d;
    if (s) {
      for (size_t i = 0; i + 1 < d.size(); ++i)
        sh[i] = (char)((((unsigned char)d[i]) << s) | (((unsigned char)d[i + 1]) >> (8 - s)));
      hay = &sh;
    }
    for (size_t at = hay->find(pat, (size_t)headerSize); at != std::string::npos; at = hay->find(pat, at + 1)) {
      const uint64_t name = (uint64_t)at * 8 + (uint64_t)s;
      const uint64_t pre = (uint64_t)(rsb + rtb + latb + lonb + tzb);
      if (name < pre) continue;
      uint64_t pos = name - pre;
      if (!B.ok(pos, (unsigned)(rsb + rtb))) continue;
      pos += (uint64_t)rsb;
      const uint32_t type = B.u(pos, (unsigned)rtb);
      pos += (uint64_t)rtb;
      if (type != 1 && type != 2) continue;
      const double la = B.s(pos, (unsigned)latb) / latScale;
      pos += (uint64_t)latb;
      const double lo = B.s(pos, (unsigned)lonb) / lonScale;
      if (std::fabs(la - lat) > 0.01 || std::fabs(lo - lon) > 0.01) continue;  // homonyme ailleurs
      pos = name;
      if (!B.str(pos)) continue;  // nom
      pos += (uint64_t)stb;       // station de référence
      pos += (uint64_t)cob;
      if (major < 2) {
        pos += (uint64_t)peb;
        if (!B.str(pos)) continue;  // source
        pos += (uint64_t)reb;
        if (!B.str(pos)) continue;  // commentaires
      } else {
        if (!B.str(pos)) continue;  // source
        pos += (uint64_t)reb;
        if (!B.str(pos) || !B.str(pos)) continue;  // commentaires, notes
        pos += (uint64_t)leb;
        if (!B.str(pos) || !B.str(pos)) continue;  // identifiant : contexte, numéro
        pos += (uint64_t)dab;                       // date d'import
        if (!B.str(pos)) continue;                  // champs supplémentaires
        pos += (uint64_t)(dub + 2 * dib);           // unité de direction, directions min / max
      }
      if (!B.ok(pos, (unsigned)lub)) continue;
      const uint32_t lu = B.u(pos, (unsigned)lub);
      if (lu < units.size()) return TideUnitToMetres(units[lu]);
      return kNaN;
    }
  }
  return kNaN;
}

double HarmonicStationToMetres(const std::string& harmonicPath, const std::string& station) {
  std::string d;
  if (station.size() < 2 || !ReadAll(harmonicPath, d)) return kNaN;
  auto lookup = [&](const std::string& name) -> double {
    for (size_t at = d.find(name); at != std::string::npos; at = d.find(name, at + 1)) {
      if (at > 0 && d[at - 1] != '\n') continue;
      const size_t e = d.find('\n', at);
      if (e == std::string::npos || Trim(d.substr(at, e - at)) != name) continue;
      const size_t e2 = d.find('\n', e + 1);  // fuseau
      if (e2 == std::string::npos) return kNaN;
      const size_t e3 = d.find('\n', e2 + 1);  // « niveau moyen unité »
      const std::string line = Trim(d.substr(e2 + 1, (e3 == std::string::npos ? d.size() : e3) - e2 - 1));
      const size_t sp = line.find_last_of(" \t");
      return sp == std::string::npos ? kNaN : TideUnitToMetres(line.substr(sp + 1));
    }
    return kNaN;
  };
  double f = lookup(station);
  if (!std::isnan(f)) return f;
  // station secondaire : unité de sa station de référence (index .IDX, ligne
  // « &Hmin Hmpy Hoff Lmin Lmpy Loff StaID RefFileNum RefName » qui la suit)
  std::string idx;
  if (!ReadAll(harmonicPath + ".IDX", idx) && !ReadAll(harmonicPath + ".idx", idx)) return kNaN;
  for (size_t at = idx.find(station); at != std::string::npos; at = idx.find(station, at + 1)) {
    const size_t bol = idx.rfind('\n', at) == std::string::npos ? 0 : idx.rfind('\n', at) + 1;
    const size_t e = idx.find('\n', at);
    if (e == std::string::npos || Trim(idx.substr(at, e - at)) != station) continue;
    if (idx[bol] != 't') continue;
    const size_t e2 = idx.find('\n', e + 1);
    std::string amp = idx.substr(e + 1, (e2 == std::string::npos ? idx.size() : e2) - e - 1);
    if (amp.empty() || amp[0] != '&') continue;
    // 8 champs, puis le nom de la référence
    size_t k = 1;
    for (int n = 0; n < 8 && k < amp.size(); ++n) {
      while (k < amp.size() && std::isspace((unsigned char)amp[k])) ++k;
      while (k < amp.size() && !std::isspace((unsigned char)amp[k])) ++k;
    }
    const std::string ref = Trim(amp.substr(k));
    if (!ref.empty()) return lookup(ref);
  }
  return kNaN;
}

}  // namespace pd

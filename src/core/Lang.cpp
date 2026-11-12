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

#include "Lang.h"

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "FileUtil.h"

namespace pd {
namespace {
std::atomic<int> g_lang{kFrench};

const std::unordered_map<std::string, const char*>& Table() {
  static const std::unordered_map<std::string, const char*> t = {
#include "LangTable.inc"
  };
  return t;
}

const std::unordered_map<std::string, const char*>& TableRu() {
  static const std::unordered_map<std::string, const char*> t = {
#include "LangTableRu.inc"
  };
  return t;
}

// Traductions lues dans les fichiers .po : clé = "fr\x04" (contexte) ou anglais.
// Une table remplacée n'est jamais libérée (les pointeurs rendus par tr()
// restent valides) ; elle est petite et rarement rechargée.
using Map = std::unordered_map<std::string, std::string>;
std::mutex g_mx;
std::vector<std::unique_ptr<Map>> g_all;
std::atomic<const Map*> g_po{nullptr};

std::string Unescape(const std::string& s) {
  std::string o;
  for (size_t i = 0; i < s.size(); ++i) {
    if (s[i] != '\\' || i + 1 >= s.size()) { o += s[i]; continue; }
    const char c = s[++i];
    o += c == 'n' ? '\n' : c == 't' ? '\t' : c == 'r' ? '\r' : c;
  }
  return o;
}

std::string Escape(const std::string& s) {
  std::string o;
  for (char c : s) {
    if (c == '\\' || c == '"') { o += '\\'; o += c; }
    else if (c == '\n') o += "\\n";
    else if (c == '\t') o += "\\t";
    else o += c;
  }
  return o;
}

// chaîne entre guillemets d'une ligne .po (après le mot-clé)
bool Quoted(const std::string& line, size_t from, std::string& out) {
  const size_t a = line.find('"', from), b = line.rfind('"');
  if (a == std::string::npos || b <= a) return false;
  out += Unescape(line.substr(a + 1, b - a - 1));
  return true;
}
}  // namespace

std::string FormatSignature(const char* s) {
  std::string sig;
  for (const char* p = s; p && *p; ++p) {
    if (*p != '%') continue;
    ++p;
    if (*p == '%') continue;
    while (*p && std::strchr("-+ #0123456789.*'", *p)) ++p;
    while (*p && std::strchr("hlLqjzt", *p)) ++p;  // longueur (ll, z…)
    if (!*p) break;
    sig += *p;
  }
  return sig;
}

void SetLanguage(int lang) {
  g_lang = (lang == kEnglish || lang == kRussian || lang == kOther) ? lang : kFrench;
}
int Language() { return g_lang; }

const char* tr(const char* fr) {
  const int lang = g_lang;
  if (!fr || lang == kFrench) return fr;
  const auto& t = Table();
  auto en = t.find(fr);
  if (lang != kEnglish) {
    if (const Map* po = g_po.load()) {  // fichier de traduction
      auto it = po->find(std::string(fr) + '\x04');
      if (it == po->end() && en != t.end()) it = po->find(en->second);
      if (it != po->end()) return it->second.c_str();
    }
    if (lang == kRussian) {  // table russe compilée
      const auto& r = TableRu();
      auto it = r.find(fr);
      if (it != r.end()) return it->second;
    }
  }
  return en == t.end() ? fr : en->second;  // anglais (repli)
}

int LoadTranslations(const std::vector<std::string>& paths, std::string* notes) {
  auto m = std::make_unique<Map>();
  int any = -1;
  for (const std::string& path : paths) {
    FILE* f = OpenFile(path, "rb");
    if (!f) continue;
    any = std::max(any, 0);
    std::string ctx, id, str, line;
    int field = 0;  // 1 msgctxt, 2 msgid, 3 msgstr
    auto commit = [&]() {
      if (!id.empty() && !str.empty()) {
        const std::string key = ctx.empty() ? id : ctx + '\x04';
        if (FormatSignature(id.c_str()) != FormatSignature(str.c_str())) {
          if (notes) *notes += "format %… différent de l'anglais : " + id + "\n";
        } else {
          (*m)[key] = str;
        }
      }
      ctx.clear(); id.clear(); str.clear(); field = 0;
    };
    char buf[8192];
    while (std::fgets(buf, sizeof buf, f)) {
      line = buf;
      while (!line.empty() && (line.back() == '\n' || line.back() == '\r')) line.pop_back();
      if (line.size() >= 3 && (unsigned char)line[0] == 0xEF && (unsigned char)line[1] == 0xBB &&
          (unsigned char)line[2] == 0xBF)
        line.erase(0, 3);  // BOM UTF-8
      size_t p = line.find_first_not_of(" \t");
      if (p == std::string::npos || line[p] == '#') {
        if (p == std::string::npos && field == 3) commit();
        continue;
      }
      if (line.compare(p, 7, "msgctxt") == 0) {
        if (field >= 2) commit();
        field = 1;
        Quoted(line, p + 7, ctx);
      } else if (line.compare(p, 6, "msgid ") == 0 || line.compare(p, 6, "msgid\t") == 0) {
        if (field == 3) commit();
        field = 2;
        Quoted(line, p + 5, id);
      } else if (line.compare(p, 6, "msgstr") == 0) {
        field = 3;
        Quoted(line, p + 6, str);
      } else if (line[p] == '"') {  // suite de la chaîne précédente
        Quoted(line, p, field == 1 ? ctx : field == 2 ? id : str);
      }
    }
    commit();
    std::fclose(f);
  }
  const int n = (int)m->size();
  {
    std::lock_guard<std::mutex> lk(g_mx);
    g_po = m.get();
    g_all.push_back(std::move(m));
  }
  return any < 0 ? -1 : n;
}

void ClearTranslations() { g_po = nullptr; }
bool HasTranslations() { return g_po.load() != nullptr; }

bool WriteTranslationTemplate(const std::string& path, int lang, std::string& err) {
  FILE* f = OpenFile(path, "wb");
  if (!f) { err = path; return false; }
  // anglais -> textes français ; contexte (msgctxt) quand deux textes français
  // de même anglais ont des traductions différentes
  std::unordered_map<std::string, std::vector<std::string>> frOf;
  for (const auto& kv : Table()) frOf[kv.second].push_back(kv.first);
  // ordre stable : celui de la table (premier texte français rencontré)
  std::vector<std::pair<std::string, std::string>> order;  // (fr, en)
  static const char* const kOrder[][2] = {
#include "LangTable.inc"
  };
  for (const auto& e : kOrder) order.push_back({e[0], e[1]});
  std::fputs(
      "# PersonalDepth_pi — interface translation (gettext .po, UTF-8)\n"
      "# msgid = English text, msgstr = translation (empty: English is shown).\n"
      "# msgctxt, when present, is the exact French source text: it separates two\n"
      "# texts that are the same in English but need different translations.\n"
      "# Keep the %d, %s, %.2f… of the English text, in the same order.\n"
      "# \\n is a line break.\n"
      "#\n"
      "# Your corrected copy: put it in the PersonalDepth_pi user folder (the one\n"
      "# that holds the palettes folder), in a \"lang\" subfolder, with the same\n"
      "# file name. It is read after this one and is kept on plugin updates.\n"
      "msgid \"\"\n"
      "msgstr \"\"\n"
      "\"Content-Type: text/plain; charset=UTF-8\\n\"\n\n",
      f);
  const auto& ru = TableRu();
  std::unordered_map<std::string, bool> done;
  for (const auto& e : order) {
    const std::string& fr = e.first;
    const std::string& en = e.second;
    std::string tr;
    if (lang == kRussian) {
      auto it = ru.find(fr);
      if (it != ru.end()) tr = it->second;
    }
    // contexte si un autre texte français de même anglais se traduit autrement
    bool ctx = false;
    if (lang == kRussian)
      for (const std::string& o : frOf[en])
        if (o != fr) {
          auto it = ru.find(o);
          if (it != ru.end() && tr != it->second) ctx = true;
        }
    if (!ctx && done.count(en)) continue;
    if (!ctx) done[en] = true;
    std::fprintf(f, "#. fr: %s\n", Escape(fr).c_str());
    if (ctx) std::fprintf(f, "msgctxt \"%s\"\n", Escape(fr).c_str());
    std::fprintf(f, "msgid \"%s\"\nmsgstr \"%s\"\n\n", Escape(en).c_str(), Escape(tr).c_str());
  }
  std::fclose(f);
  return true;
}
}  // namespace pd

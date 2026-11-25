// PersonalDepth_pi - catalogue des données externes
// Copyright (C) 2026 Marcaurelio Franzetti
// SPDX-License-Identifier: GPL-2.0-or-later
#include "Catalog.h"

#include <cctype>
#include <cstdio>
#include <filesystem>

#include "FileUtil.h"
#include "Json.h"
#include "Lang.h"
#include "Sha256.h"

namespace fs = std::filesystem;

namespace pd {

std::string CatalogItem::text(const std::map<std::string, std::string>& m, const std::string& lang) const {
  auto it = m.find(lang);
  if (it != m.end() && !it->second.empty()) return it->second;
  it = m.find("en");
  if (it != m.end() && !it->second.empty()) return it->second;
  it = m.find("fr");
  if (it != m.end()) return it->second;
  return m.empty() ? id : m.begin()->second;
}

const CatalogItem* Catalog::find(const std::string& id) const {
  for (const auto& i : items)
    if (i.id == id) return &i;
  return nullptr;
}

namespace catalog {

const char* kRemoteUrl = "https://raw.githubusercontent.com/margouilla/PersonalDepth_pi/main/data/catalog.json";

const std::string& BuiltinJson() {
  static const unsigned char bytes[] = {
#include "CatalogBuiltin.inc"
  };
  static const std::string s((const char*)bytes, sizeof bytes);
  return s;
}

static bool IsHttps(const std::string& u) { return u.compare(0, 8, "https://") == 0; }

bool SafeFileName(const std::string& n) {
  if (n.empty() || n == "." || n == "..") return false;
  for (char c : n)
    if (c == '/' || c == '\\' || c == ':' || (unsigned char)c < 32) return false;
  return true;
}

bool Parse(const std::string& json, Catalog& out, std::string& err) {
  out = Catalog();
  Json j;
  if (!Json::Parse(json, j, err)) return false;
  out.format = (int)j["format"].integer();
  out.version = (int)j["version"].integer();
  out.date = j["date"].str();
  if (out.format != 1) { err = tr("Catalogue des données externes : format non pris en charge"); return false; }
  auto texts = [](const Json& v) {
    std::map<std::string, std::string> m;
    if (v.isObject())
      for (const auto& kv : v.members()) m[kv.first] = kv.second.str();
    else if (!v.str().empty())
      m["fr"] = v.str();
    return m;
  };
  for (const auto& v : j["items"].items()) {
    CatalogItem it;
    it.id = v["id"].str();
    if (it.id.empty()) continue;
    it.group = v["group"].str();
    it.use = v["use"].str();
    it.name = texts(v["name"]);
    it.desc = texts(v["desc"]);
    it.producer = v["producer"].str();
    it.licence = v["licence"].str();
    it.licenceUrl = v["licence_url"].str();
    it.citation = v["citation"].str();
    it.page = v["page"].str();
    it.manual = v["manual"].boolean();
    it.check = v["check"].str();
    it.convert = v["convert"].str();
    if (it.convert != "fes" && it.convert != "mss" && it.convert != "cd") it.convert.clear();
    const Json& d = v["download"];
    if (d.isObject()) {
      it.url = d["url"].str();
      it.file = d["file"].str();
      it.size = d["size"].integer();
      it.sha256 = d["sha256"].str();
      it.extract = d["extract"].boolean();
      it.subdir = d["subdir"].str();
      // seulement des adresses https et des noms sans dossier
      if (!IsHttps(it.url) || !SafeFileName(it.file) || (!it.subdir.empty() && !SafeFileName(it.subdir))) {
        it.url.clear();
        it.file.clear();
        it.subdir.clear();
      }
    }
    for (const auto& f : v["files"].items()) {
      const std::string n = f["name"].str(), h = f["sha256"].str();
      if (SafeFileName(n) && h.size() == 64) it.files.emplace_back(n, h);
    }
    if (!it.page.empty() && !IsHttps(it.page)) it.page.clear();
    if (!it.licenceUrl.empty() && !IsHttps(it.licenceUrl)) it.licenceUrl.clear();
    if (it.check.empty() && !it.extract) it.check = it.file;
    out.items.push_back(std::move(it));
  }
  if (out.items.empty()) { err = tr("Catalogue des données externes vide"); return false; }
  return true;
}

Catalog Load(const std::string& cachePath) {
  Catalog c, d;
  std::string err;
  Parse(BuiltinJson(), c, err);
  if (!cachePath.empty()) {
    FILE* f = OpenFile(cachePath, "rb");
    if (f) {
      std::string s;
      char b[8192];
      size_t n;
      while ((n = std::fread(b, 1, sizeof b, f)) > 0) s.append(b, n);
      std::fclose(f);
      if (Parse(s, d, err) && d.version > c.version) return d;
    }
  }
  return c;
}

bool WildMatch(const std::string& pattern, const std::string& name) {
  const char* p = pattern.c_str();
  const char* s = name.c_str();
  const char *star = nullptr, *ss = nullptr;
  auto eq = [](char a, char b) { return std::tolower((unsigned char)a) == std::tolower((unsigned char)b); };
  while (*s) {
    if (*p == '?' || (*p && *p != '*' && eq(*p, *s))) { ++p; ++s; continue; }
    if (*p == '*') { star = p++; ss = s; continue; }
    if (star) { p = star + 1; s = ++ss; continue; }
    return false;
  }
  while (*p == '*') ++p;
  return *p == 0;
}

std::string FindFile(const std::string& dir, const std::string& pattern) {
  if (dir.empty() || pattern.empty()) return std::string();
  std::error_code ec;
  const fs::path root = U8Path(dir);
  if (!fs::is_directory(root, ec)) return std::string();
  for (fs::recursive_directory_iterator it(root, fs::directory_options::skip_permission_denied, ec), end;
       it != end; it.increment(ec)) {
    if (ec) break;
    if (it.depth() > 4) { it.disable_recursion_pending(); continue; }
    if (!it->is_regular_file(ec)) continue;
    if (WildMatch(pattern, it->path().filename().u8string())) return it->path().u8string();
  }
  return std::string();
}

std::vector<FileCheck> Verify(const CatalogItem& it, const std::string& dir) {
  std::vector<FileCheck> out;
  auto one = [&](const std::string& name, const std::string& sha) {
    FileCheck c;
    c.name = name;
    c.path = FindFile(dir, name);
    if (c.path.empty()) c.status = FileCheck::Missing;
    else if (sha.empty()) c.status = FileCheck::NoFingerprint;
    else c.status = Sha256::OfFile(c.path) == sha ? FileCheck::Ok : FileCheck::Different;
    out.push_back(c);
  };
  if (!it.files.empty()) {
    for (const auto& f : it.files) one(f.first, f.second);
  } else if (!it.check.empty()) {
    one(it.check, it.extract ? std::string() : it.sha256);
  }
  return out;
}

}  // namespace catalog
}  // namespace pd

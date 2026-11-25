// PersonalDepth_pi - catalogue des données externes (téléchargement guidé)
//
// Fichier JSON (data/catalog.json, copie intégrée au plugin, mise à jour
// possible depuis le dépôt sans nouvelle version) :
//   {"format":1, "version":N, "date":"AAAA-MM-JJ", "items":[
//     {"id", "group", "use" (france | tide | mss | national),
//      "name":{"fr","en","ru"}, "producer", "desc":{...},
//      "licence", "licence_url", "citation", "page",
//      "download":{"url", "file", "size", "sha256", "extract", "subdir"},
//      "manual": true (pas de téléchargement direct : page à ouvrir),
//      "check": motif du fichier installé (« *ZH*.glhi »),
//      "files": [{"name", "sha256"}, ...] (fichiers installés, pour « Vérifier »),
//      "convert": "fes" | "mss" | "cd" (conversion intégrée du fichier du producteur)}, ...]}
//
// Copyright (C) 2026 Marcaurelio Franzetti
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <map>
#include <string>
#include <vector>

namespace pd {

struct CatalogItem {
  std::string id, group, use;
  std::map<std::string, std::string> name, desc;  // langue -> texte
  std::string producer, licence, licenceUrl, citation, page;
  bool manual = false;
  // téléchargement direct
  std::string url, file, sha256, subdir;
  long long size = 0;
  bool extract = false;
  std::string check;  // motif (* et ?) du nom du fichier installé
  std::string convert;  // "fes" (.pdtide), "mss" ou "cd" (.pdgrid) : bouton « Convertir… »
  // fichiers installés et leur empreinte (contenu des archives) ; vide : le
  // fichier téléchargé lui-même (sha256), s'il n'est pas une archive
  std::vector<std::pair<std::string, std::string>> files;

  std::string text(const std::map<std::string, std::string>& m, const std::string& lang) const;
  std::string title(const std::string& lang) const { return text(name, lang); }
  std::string description(const std::string& lang) const { return text(desc, lang); }
  bool downloadable() const { return !manual && !url.empty() && !file.empty(); }
};

struct Catalog {
  int format = 0, version = 0;
  std::string date;
  std::vector<CatalogItem> items;
  const CatalogItem* find(const std::string& id) const;
};

namespace catalog {

extern const char* kRemoteUrl;  // catalogue à jour dans le dépôt
const std::string& BuiltinJson();
bool Parse(const std::string& json, Catalog& out, std::string& err);
// Catalogue intégré, remplacé par cachePath s'il est lisible et plus récent
Catalog Load(const std::string& cachePath);

// Motif simple : * (n'importe quelle suite), ? (un caractère), sans casse
bool WildMatch(const std::string& pattern, const std::string& name);
// Premier fichier du dossier (sous-dossiers compris) dont le nom correspond
// au motif ; vide si aucun
std::string FindFile(const std::string& dir, const std::string& pattern);
// Contrôle des fichiers installés d'une donnée (dossier dir) par leurs empreintes
struct FileCheck {
  enum Status { Ok, Different, Missing, NoFingerprint } status = Missing;
  std::string name;  // nom attendu
  std::string path;  // fichier trouvé (vide si absent)
};
std::vector<FileCheck> Verify(const CatalogItem& it, const std::string& dir);
// Nom de fichier sûr pour l'enregistrement (sans dossier ni « .. »)
bool SafeFileName(const std::string& name);

}  // namespace catalog
}  // namespace pd

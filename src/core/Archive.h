// PersonalDepth_pi - extraction des archives téléchargées (.zip, .7z)
//
// Archives des données externes : BathyElli du Shom (.7z, LZMA), régions
// NOAA VDatum (.zip)… Zip : stockés ou deflate (miniz) ; 7z : décodeur du
// LZMA SDK (domaine public, third_party/lzma). Les noms des entrées sont
// nettoyés : jamais de chemin absolu ni de « .. » (rien n'est écrit hors du
// dossier de destination).
//
// Copyright (C) 2026 Marcaurelio Franzetti
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace pd {

struct ArchiveEntry {
  std::string name;  // chemin relatif dans l'archive, séparateur « / », nettoyé
  uint64_t size = 0;
  bool dir = false;
};

enum class ArchiveKind { None, Zip, SevenZip };
ArchiveKind DetectArchive(const std::string& path);

bool ListArchive(const std::string& path, std::vector<ArchiveEntry>& out, std::string& err);

// Extrait les fichiers retenus par want(nom relatif) (tous si want est vide)
// dans destDir. flatten : sans les sous-dossiers de l'archive (nom de fichier
// seul). written : chemins écrits.
bool ExtractArchive(const std::string& path, const std::string& destDir,
                    const std::function<bool(const std::string&)>& want, bool flatten,
                    std::vector<std::string>& written, std::string& err);

// Nom d'entrée sûr (séparateurs « / », sans « . », « .. » ni lecteur) ; vide
// si le nom est refusé
std::string SafeEntryName(const std::string& name);

}  // namespace pd

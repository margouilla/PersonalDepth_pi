// PersonalDepth_pi - personal bathymetry plugin for OpenCPN
// Copyright (C) 2026 Marcaurelio Franzetti <marcaurelio.franzetti@gmail.com>
// SPDX-License-Identifier: GPL-2.0-or-later
//
// Écrit le fichier de traduction .po d'une langue (anglais -> langue) depuis les
// tables du plugin : data/lang/PersonalDepth_ru.po (traductions compilées) ou
// un modèle vide pour une nouvelle langue.
//   g++ -std=c++17 -o make_po tools/lang/make_po.cpp src/core/Lang.cpp src/core/FileUtil.cpp
//   ./make_po ru data/lang/PersonalDepth_ru.po
//   ./make_po xx PersonalDepth_xx.po      (modèle vide)
#include <cstdio>
#include <string>

#include "../../src/core/Lang.h"

int main(int argc, char** argv) {
  if (argc < 3) { std::fprintf(stderr, "usage : make_po <ru|xx> <fichier.po>\n"); return 2; }
  const std::string code = argv[1];
  std::string err;
  if (!pd::WriteTranslationTemplate(argv[2], code == "ru" ? pd::kRussian : pd::kOther, err)) {
    std::fprintf(stderr, "écriture impossible : %s\n", err.c_str());
    return 1;
  }
  std::printf("%s écrit\n", argv[2]);
  return 0;
}

// PersonalDepth_pi - empreinte SHA-256 (contrôle des fichiers téléchargés)
// Copyright (C) 2026 Marcaurelio Franzetti
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

namespace pd {

class Sha256 {
 public:
  Sha256() { reset(); }
  void reset();
  void update(const void* data, size_t n);
  std::string hex();  // termine le calcul : 64 caractères hexadécimaux minuscules

  static std::string OfString(const std::string& s);
  // Empreinte d'un fichier (chemin UTF-8) ; chaîne vide si illisible
  static std::string OfFile(const std::string& path);

 private:
  void block(const uint8_t* p);
  uint32_t h_[8];
  uint8_t buf_[64];
  size_t used_ = 0;
  uint64_t len_ = 0;
};

}  // namespace pd

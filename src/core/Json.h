// PersonalDepth_pi - lecture JSON minimale (catalogue des données externes,
// liste des marégraphes REFMAR)
// Copyright (C) 2026 Marcaurelio Franzetti
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <map>
#include <memory>
#include <string>
#include <vector>

namespace pd {

class Json {
 public:
  enum Type { Null, Bool, Number, String, Array, Object };

  Type type() const { return t_; }
  bool isNull() const { return t_ == Null; }
  bool isArray() const { return t_ == Array; }
  bool isObject() const { return t_ == Object; }
  // Valeurs (valeur par défaut si le type ne correspond pas ; un nombre écrit
  // entre guillemets est accepté par num())
  double num(double def = 0) const;
  long long integer(long long def = 0) const { return (long long)num((double)def); }
  bool boolean(bool def = false) const { return t_ == Bool ? b_ : def; }
  std::string str(const std::string& def = std::string()) const;
  // Tableau
  size_t size() const { return t_ == Array ? a_.size() : t_ == Object ? o_.size() : 0; }
  const Json& operator[](size_t i) const;
  // Objet : membre absent -> valeur nulle
  const Json& operator[](const std::string& k) const;
  const Json& operator[](const char* k) const { return (*this)[std::string(k)]; }
  bool has(const std::string& k) const { return t_ == Object && o_.count(k); }
  const std::map<std::string, Json>& members() const { return o_; }
  const std::vector<Json>& items() const { return a_; }

  // Texte UTF-8 -> arbre ; false et message en cas d'erreur
  static bool Parse(const std::string& text, Json& out, std::string& err);

 private:
  friend class JsonParser;
  Type t_ = Null;
  bool b_ = false;
  double n_ = 0;
  std::string s_;
  std::vector<Json> a_;
  std::map<std::string, Json> o_;
};

}  // namespace pd

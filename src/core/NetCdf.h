// PersonalDepth_pi - lecture des fichiers netCDF sans bibliothèque externe
//
// netCDF-3 classique (CDF-1, CDF-2 64 bits, CDF-5) et netCDF-4, c'est-à-dire
// HDF5, limité à ce qu'écrivent netCDF-C et GDAL pour des grilles :
//   superbloc v0 à v3, en-têtes d'objet v1 et v2, groupes anciens (table des
//   symboles) et nouveaux (liens compacts ou denses, tas fractal, B-tree v2),
//   attributs compacts ou denses, types entiers et flottants, chaînes fixes ;
//   stockage compact, contigu ou par blocs (B-tree v1, bloc unique, blocs
//   implicites, tableau fixe non paginé), filtres deflate, shuffle, fletcher32.
// Le reste (tableaux extensibles, B-tree v2 de blocs, szip, scale-offset,
// types composés…) est refusé avec un message clair.
//
// Copyright (C) 2026 Marcaurelio Franzetti
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace pd {

struct NcAtt {
  std::string name;
  bool text = false;
  std::string str;          // texte (text)
  std::vector<double> num;  // valeurs numériques (!text)
};

struct NcVar {
  std::string name;
  std::vector<uint64_t> shape;
  std::vector<std::string> dims;  // noms des dimensions (netCDF-3 ; vide en netCDF-4)
  std::vector<NcAtt> atts;
  bool numeric = true;  // type lisible par Read
  bool floating = false;
  int bytes = 0;  // taille d'un élément
  const NcAtt* att(const std::string& name) const;
  double num(const std::string& name, double def) const;  // premier nombre
  std::string str(const std::string& name) const;
  uint64_t size() const;
};

class NcFile {
 public:
  NcFile();
  ~NcFile();
  NcFile(const NcFile&) = delete;
  NcFile& operator=(const NcFile&) = delete;

  bool open(const std::string& path, std::string& err);
  void close();
  int format() const { return format_; }  // 3 ou 4 (0 si fermé)
  const std::vector<NcVar>& vars() const { return vars_; }
  const std::vector<NcAtt>& globals() const { return globals_; }
  const NcVar* var(const std::string& name) const;
  // Coordonnée 1-D associée à la dimension `axis` de v : dimension nommée
  // (netCDF-3), sinon variable 1-D de même taille nommée lat/latitude/y
  // (want 'y') ou lon/longitude/x (want 'x'), ou d'unités degrees_north/east
  const NcVar* coord(const NcVar& v, size_t axis, char want) const;

  // Hyperrectangle start/count, valeurs brutes converties en double, ordre
  // C (dernière dimension la plus rapide). Ni masque ni facteur d'échelle.
  bool read(const NcVar& v, const std::vector<uint64_t>& start, const std::vector<uint64_t>& count,
            std::vector<double>& out, std::string& err);
  // Ensemble de la variable
  bool readAll(const NcVar& v, std::vector<double>& out, std::string& err);

  // Taille maximale du cache des blocs décompressés (octets)
  void setCacheBytes(size_t n);

  struct Impl;

 private:
  std::unique_ptr<Impl> d_;
  std::vector<NcVar> vars_;
  std::vector<NcAtt> globals_;
  int format_ = 0;
};

}  // namespace pd

// PersonalDepth_pi - conversion des grilles netCDF d'AVISO dans les formats
// compacts du plugin, sans Python :
//   - atlas FES2022 (une onde par fichier) -> .pdtide (TideFES)
//   - surface moyenne océanique CNES_CLS, grilles de zéro des cartes des pays
//     (netCDF, GeoTIFF en lon / lat, .gtx, .asc) -> .pdgrid (GeoGrid)
// Même résultat que tools/pdtide_convert.py et tools/pdgrid_convert.py
// (mêmes valeurs ; la compression zlib peut différer d'octets). La conversion
// reprend là où elle s'est arrêtée : parties dans <sortie>.parts, au même
// format que les scripts.
//
// Copyright (C) 2026 Marcaurelio Franzetti
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <functional>
#include <string>

namespace pd {

// Avancement (0..1) et étape ; renvoie false pour interrompre
using ConvertProgress = std::function<bool(double frac, const std::string& step)>;

struct PdgridOptions {
  std::string var = "mssh";  // netCDF ; vide : première grille lat / lon du fichier
  std::string name;    // vide : attribut global OriginalName ou title
  std::string period;  // vide : lue entre [ ] dans long_name
  std::string quantity = "MSS/WGS84";
  bool tp2wgs84 = false;  // grille au-dessus de l'ellipsoïde TOPEX/Poseidon
  bool negate = false;    // valeurs de signe opposé (ex. niveau moyen au-dessus du zéro des cartes)
};

// `in` : .nc (netCDF-3 / netCDF-4), .tif / .tiff (longitude / latitude),
// .gtx, .asc, .tac, .mnt.
// Résultat : 1 terminé, 0 interrompu (parties gardées), -1 erreur (err)
int ConvertToPdgrid(const std::string& in, const std::string& out, const PdgridOptions& opt,
                    const ConvertProgress& progress, std::string& err);

// `dir` : dossier ocean_tide_extrapolated (Lambda2 cherchée dans ../ocean_tide)
int ConvertFesToPdtide(const std::string& dir, const std::string& out, const ConvertProgress& progress,
                       std::string& err);

// Dossier ocean_tide_extrapolated à partir du choix de l'utilisateur (ce
// dossier, le dossier FES2022b au-dessus, ou un fichier .nc dedans) ; vide si
// m2_fes2022.nc n'y est pas
std::string FindFesFolder(const std::string& chosen);

}  // namespace pd

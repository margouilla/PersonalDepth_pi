# -*- coding: utf-8 -*-
# PersonalDepth_pi - personal bathymetry plugin for OpenCPN
# Copyright (C) 2026 Marcaurelio Franzetti <marcaurelio.franzetti@gmail.com>
#
# This program is free software; you can redistribute it and/or modify
# it under the terms of the GNU General Public License as published by
# the Free Software Foundation; either version 2 of the License, or
# (at your option) any later version.
#
# This program is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
# GNU General Public License for more details.
#
# SPDX-License-Identifier: GPL-2.0-or-later
"""Conversion des atlas FES2022 (NetCDF, une onde par fichier) en un fichier
compact .pdtide lisible par PersonalDepth_pi (sans bibliothèque NetCDF).

Usage :
    python pdtide_convert.py <dossier ocean_tide_extrapolated> <sortie.pdtide> [--max-seconds=N]

La conversion reprend là où elle s'est arrêtée (parties dans <sortie>.parts),
puis assemble le fichier final quand toutes les ondes sont prêtes.
Dépendances : numpy, netCDF4. Compatible Python 3.5.

Format (petit-boutiste)
  0   char[8]  "PDTIDE01"
  8   uint32   nombre d'ondes W
  12  uint32   taille de tuile T (32)
  16  int32    nx, ny (nœuds), ntx, nty (tuiles)
  32  float64  lon0, lat0 (nœud 0,0 = sud-ouest), dlon, dlat
  64  float64  unité des valeurs (m) = 0.001
  72  uint32   drapeaux (bit 0 : longitude périodique), réservé
  80  W × char[16]  noms des ondes (convention FES : M2, Msqm, Lambda2…)
  puis index W × nty × ntx × 16 octets : uint64 position, uint32 taille,
       uint8 type (0 vide, 1 pleine, 2 partielle), 3 octets nuls
  puis tuiles : flux zlib contenant
       [masque T*T bits si partielle, ligne sud d'abord, bit i%8 de l'octet i/8]
       Re puis Im : int16, différence avec le nœud précédent de la ligne
       (le premier nœud de chaque ligne avec 0), octets séparés (tous les
       octets de poids faible puis tous les forts). Re = A cos(G), Im = A sin(G)
       en mm, G = retard de phase de Greenwich.
"""
import os
import struct
import sys
import time
import zlib

import numpy as np
import netCDF4 as nc

T = 32
NAMES = ["2N2", "Eps2", "J1", "K1", "K2", "L2", "Lambda2", "M2", "M3", "M4", "M6", "M8",
         "MKS2", "MN4", "MS4", "MSf", "Mf", "Mm", "Msqm", "Mtm", "Mu2", "N2", "N4", "Nu2",
         "O1", "P1", "Q1", "R2", "S1", "S2", "S4", "Sa", "Ssa", "T2"]
IDX = struct.Struct("<QIB3x")


def grid_info(path):
    d = nc.Dataset(path)
    lon = d["lon"][:]
    lat = d["lat"][:]
    d.close()
    return len(lon), len(lat), float(lon[0]), float(lat[0]), float(lon[1] - lon[0]), float(lat[1] - lat[0])


def convert_wave(src, name, part_dir):
    path = os.path.join(src, name.lower() + "_fes2022.nc")
    if not os.path.exists(path):  # ex. Lambda2 : absente des grilles extrapolées
        path = os.path.join(os.path.dirname(os.path.abspath(src)), "ocean_tide",
                            name.lower() + "_fes2022.nc")
    nx0 = grid_info(os.path.join(src, "m2_fes2022.nc"))[0]
    d = nc.Dataset(path)
    d.set_auto_mask(True)
    a = d["amplitude"][:, :nx0].filled(np.nan).astype(np.float64)  # cm
    g = np.deg2rad(d["phase"][:, :nx0].filled(np.nan).astype(np.float64))
    d.close()
    ny, nx = a.shape
    re = a * np.cos(g) * 10.0  # mm
    im = a * np.sin(g) * 10.0
    del a, g
    valid = ~np.isnan(re)
    mx = np.nanmax(np.abs(np.concatenate([re[valid], im[valid]])))
    if mx > 16000:
        raise RuntimeError("%s : amplitude %.0f mm hors limites int16" % (name, mx))
    nty, ntx = (ny + T - 1) // T, (nx + T - 1) // T
    def pad(x, fill):
        out = np.full((nty * T, ntx * T), fill, dtype=x.dtype)
        out[:ny, :nx] = x
        return out.reshape(nty, T, ntx, T).transpose(0, 2, 1, 3)
    R = pad(np.where(valid, np.round(re), 0).astype(np.int32), 0)
    I = pad(np.where(valid, np.round(im), 0).astype(np.int32), 0)
    V = pad(valid, False)
    del re, im
    data_path = os.path.join(part_dir, name + ".bin.tmp")
    index = bytearray()
    off = 0
    with open(data_path, "wb") as f:
        for tj in range(nty):
            for ti in range(ntx):
                m = V[tj, ti]
                nvalid = int(m.sum())
                if nvalid == 0:
                    index += IDX.pack(0, 0, 0)
                    continue
                parts = []
                kind = 1
                if nvalid < T * T:
                    kind = 2
                    parts.append(np.packbits(m.reshape(-1), bitorder="little").tobytes())
                for C in (R[tj, ti], I[tj, ti]):
                    dd = np.diff(C, axis=1, prepend=0).astype("<i2")
                    parts.append(dd.view(np.uint8).reshape(-1, 2).T.tobytes())
                blob = zlib.compress(b"".join(parts), 6)
                f.write(blob)
                index += IDX.pack(off, len(blob), kind)
                off += len(blob)
    with open(os.path.join(part_dir, name + ".idx"), "wb") as f:
        f.write(index)
    os.replace(data_path, os.path.join(part_dir, name + ".bin"))
    return off


def assemble(src, out, part_dir):
    nx, ny, lon0, lat0, dlon, dlat = grid_info(os.path.join(src, "m2_fes2022.nc"))
    nty, ntx = (ny + T - 1) // T, (nx + T - 1) // T
    W = len(NAMES)
    periodic = 1 if abs(nx * dlon - 360.0) < 1e-6 else 0
    head = struct.pack("<8sII4i4dd2I", b"PDTIDE01", W, T, nx, ny, ntx, nty,
                       lon0, lat0, dlon, dlat, 0.001, periodic, 0)
    head += b"".join(struct.pack("16s", n.encode("ascii")) for n in NAMES)
    ntile = nty * ntx
    data0 = len(head) + W * ntile * IDX.size
    tmp = out + ".tmp"
    with open(tmp, "wb") as f:
        f.write(head)
        base = data0
        for n in NAMES:  # index absolu
            raw = open(os.path.join(part_dir, n + ".idx"), "rb").read()
            assert len(raw) == ntile * IDX.size
            buf = bytearray()
            for k in range(ntile):
                o, s, t = IDX.unpack_from(raw, k * IDX.size)
                buf += IDX.pack(base + o if t else 0, s, t)
            f.write(buf)
            base += os.path.getsize(os.path.join(part_dir, n + ".bin"))
        for n in NAMES:
            with open(os.path.join(part_dir, n + ".bin"), "rb") as b:
                while True:
                    c = b.read(1 << 24)
                    if not c:
                        break
                    f.write(c)
    os.replace(tmp, out)


def main():
    args = [a for a in sys.argv[1:] if not a.startswith("--")]
    max_s = 1e9
    for a in sys.argv[1:]:
        if a.startswith("--max-seconds="):
            max_s = float(a.split("=")[1])
    src, out = args[0], args[1]
    part_dir = out + ".parts"
    if not os.path.isdir(part_dir):
        os.makedirs(part_dir)
    t0 = time.time()
    for n in NAMES:
        if os.path.exists(os.path.join(part_dir, n + ".bin")):
            continue
        if time.time() - t0 > max_s:
            print("pause : relancer pour continuer")
            return 2
        t = time.time()
        size = convert_wave(src, n, part_dir)
        print("%-8s %7.2f Mo  %4.0f s" % (n, size / 1e6, time.time() - t))
        sys.stdout.flush()
    assemble(src, out, part_dir)
    print("Fichier : %s (%.1f Mo)" % (out, os.path.getsize(out) / 1e6))
    return 0


if __name__ == "__main__":
    sys.exit(main())

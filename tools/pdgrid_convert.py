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
"""Conversion d'une grille mondiale lon/lat (NetCDF) en un fichier compact
.pdgrid lisible par PersonalDepth_pi par tuiles (sans bibliothèque NetCDF).
Sert aux surfaces de référence verticale, en premier lieu la surface moyenne
océanique (MSS) CNES_CLS 2025 d'AVISO : hauteur du niveau moyen au-dessus de
l'ellipsoïde WGS84.

Usage :
    python pdgrid_convert.py <entrée.nc> <sortie.pdgrid> [options]

Options :
    --var=mssh            variable à convertir (défaut : mssh)
    --name=...            nom du modèle (défaut : attribut OriginalName / title)
    --period=1993-2012    période de référence (défaut : lue dans long_name)
    --tp2wgs84            la grille est au-dessus de l'ellipsoïde TOPEX/Poseidon :
                          conversion vers WGS84
    --max-seconds=N       s'arrêter après N s ; relancer la même commande pour
                          continuer (parties dans <sortie>.parts)

Exemple :
    python pdgrid_convert.py mss_cnes_cls_2025.nc MSS_CNES_CLS_2025.pdgrid

Dépendances : numpy, netCDF4. Compatible Python 3.5.

Format (petit-boutiste)
  0   char[8]  "PDGRID01"
  8   uint32   taille de tuile T (32)
  12  int32    nx, ny (nœuds), ntx, nty (tuiles)
  28  uint32   drapeaux (bit 0 : longitude périodique)
  32  float64  lon0, lat0 (nœud 0,0 = sud-ouest), dlon, dlat
  64  float64  unité des valeurs (m) = 0.001
  72  char[32]  grandeur ("MSS/WGS84" : niveau moyen au-dessus de WGS84, m)
  104 char[128] modèle (ex. "CNES_CLS 2025")
  232 char[32]  période de référence (ex. "1993-2012")
  264 char[8]   réservé
  272 index nty × ntx × 16 octets : uint64 position, uint32 taille,
       uint8 type (0 vide, 1 pleine, 2 partielle, 3 pleine int32, 4 partielle
       int32), 3 octets nuls ; tuile (tj, ti) à l'entrée tj*ntx + ti, tj = 0 au sud
  puis tuiles : flux zlib contenant
       [masque T*T bits si partielle, ligne sud d'abord, bit i%8 de l'octet i/8]
       types 1, 2 : int32 base (premier nœud, en unités), puis T*T int16 :
         différence avec le nœud précédent de la ligne, le premier nœud d'une
         ligne avec le premier nœud de la ligne précédente (0 pour le tout
         premier) ; octets séparés (tous les poids faibles puis tous les forts)
       types 3, 4 : T*T int32 bruts (écarts trop grands pour int16)
       Les nœuds absents valent la dernière valeur présente (sans effet).
"""
import os
import struct
import sys
import time
import zlib

import numpy as np
import netCDF4 as nc

T = 32
BAND = 16  # rangées de tuiles par partie
IDX = struct.Struct("<QIB3x")
HEAD = struct.Struct("<8sI4iI4dd32s128s32s8s")
assert HEAD.size == 272


def tp_to_wgs84(lat_deg):
    """Hauteur au-dessus de WGS84 moins hauteur au-dessus de T/P (m) : les deux
    ellipsoïdes sont concentriques, l'écart est la différence de leurs rayons
    le long de la normale (approximation au premier ordre, < 0,1 mm)."""
    a_tp, f_tp = 6378136.3, 1.0 / 298.257
    a_w, f_w = 6378137.0, 1.0 / 298.257223563
    s2 = np.sin(np.deg2rad(lat_deg)) ** 2
    r_tp = a_tp * (1.0 - f_tp * s2)
    r_w = a_w * (1.0 - f_w * s2)
    return r_tp - r_w


class Source(object):
    def __init__(self, path, var):
        self.d = nc.Dataset(path)
        self.d.set_auto_mask(False)
        self.v = self.d[var]
        dims = self.v.dimensions
        if len(dims) != 2:
            raise RuntimeError("variable %s : 2 dimensions attendues (lat, lon)" % var)
        self.lat = np.asarray(self.d[dims[0]][:], dtype=np.float64)
        self.lon = np.asarray(self.d[dims[1]][:], dtype=np.float64)
        self.flip = self.lat[1] < self.lat[0]  # lignes du nord au sud
        self.ny, self.nx = len(self.lat), len(self.lon)
        self.dlat = abs(self.lat[1] - self.lat[0])
        self.dlon = self.lon[1] - self.lon[0]
        self.lat0 = min(self.lat[0], self.lat[-1])
        self.lon0 = self.lon[0]
        self.periodic = abs(self.nx * self.dlon - 360.0) < 1e-6
        fill = getattr(self.v, "_FillValue", None)
        self.fill = None if fill is None or (isinstance(fill, float) and np.isnan(fill)) else fill
        self.scale = getattr(self.v, "scale_factor", 1.0)
        self.offset = getattr(self.v, "add_offset", 0.0)

    def rows(self, j0, j1):
        """Lignes j0..j1-1 comptées depuis le sud, en m (NaN = absent)."""
        j0c, j1c = max(0, j0), min(self.ny, j1)
        out = np.full((j1 - j0, self.nx), np.nan)
        if j1c > j0c:
            if self.flip:
                a = self.v[self.ny - j1c:self.ny - j0c, :][::-1]
            else:
                a = self.v[j0c:j1c, :]
            a = np.asarray(a, dtype=np.float64)
            if self.fill is not None:
                a = np.where(a == self.fill, np.nan, a)
            a = a * self.scale + self.offset
            out[j0c - j0:j1c - j0] = a
        return out


def encode_band(src, b, ntx, nty, part_dir, tp):
    tj0, tj1 = b * BAND, min(nty, (b + 1) * BAND)
    a = src.rows(tj0 * T, tj1 * T)
    if tp:
        lat = src.lat0 + src.dlat * (np.arange(tj0 * T, tj1 * T) + 0.0)
        a = a + tp_to_wgs84(lat)[:, None]
    pad = np.full((a.shape[0], ntx * T), np.nan)
    pad[:, :src.nx] = a
    valid = ~np.isnan(pad)
    mm = np.zeros(pad.shape, dtype=np.int64)
    mm[valid] = np.round(pad[valid] * 1000.0).astype(np.int64)
    index = bytearray()
    data = bytearray()
    for tj in range(tj1 - tj0):
        for ti in range(ntx):
            m = valid[tj * T:(tj + 1) * T, ti * T:(ti + 1) * T]
            nv = int(m.sum())
            if nv == 0:
                index += IDX.pack(0, 0, 0)
                continue
            v = mm[tj * T:(tj + 1) * T, ti * T:(ti + 1) * T].copy()
            if nv < T * T:  # absents : dernière valeur présente (ligne, puis colonne)
                flat = v.reshape(-1)
                fm = m.reshape(-1)
                idx = np.where(fm, np.arange(flat.size), 0)
                np.maximum.accumulate(idx, out=idx)
                first = int(np.argmax(fm))
                idx[:first] = first
                flat[:] = flat[idx]
            parts = []
            if nv < T * T:  # masque : bit k%8 de l'octet k/8
                fm = m.reshape(-1).astype(np.uint8)
                bits = np.zeros(T * T // 8, dtype=np.uint8)
                for k in range(8):
                    bits |= fm[k::8] << k
                parts.append(bits.tobytes())
            d = np.empty_like(v)
            d[:, 1:] = v[:, 1:] - v[:, :-1]
            d[0, 0] = 0
            d[1:, 0] = v[1:, 0] - v[:-1, 0]
            if np.abs(d).max() <= 32767:
                kind = 1 if nv == T * T else 2
                parts.append(struct.pack("<i", int(v[0, 0])))
                dd = d.astype("<i2")
                parts.append(dd.view(np.uint8).reshape(-1, 2).T.tobytes())
            else:
                kind = 3 if nv == T * T else 4
                parts.append(v.astype("<i4").tobytes())
            blob = zlib.compress(b"".join(parts), 6)
            index += IDX.pack(len(data), len(blob), kind)
            data += blob
    tmp = os.path.join(part_dir, "%03d.tmp" % b)
    with open(tmp, "wb") as f:
        f.write(struct.pack("<I", len(index)))
        f.write(index)
        f.write(data)
    os.replace(tmp, os.path.join(part_dir, "%03d.part" % b))
    return len(data)


def text(s, n):
    return s.encode("utf-8")[:n - 1]


def assemble(src, out, part_dir, nbands, ntx, nty, name, period, quantity):
    flags = 1 if src.periodic else 0
    head = HEAD.pack(b"PDGRID01", T, src.nx, src.ny, ntx, nty, flags,
                     src.lon0, src.lat0, src.dlon, src.dlat, 0.001,
                     text(quantity, 32), text(name, 128), text(period, 32), b"")
    ntile = ntx * nty
    base = HEAD.size + ntile * IDX.size
    idx_all = bytearray()
    sizes = []
    for b in range(nbands):
        with open(os.path.join(part_dir, "%03d.part" % b), "rb") as f:
            n = struct.unpack("<I", f.read(4))[0]
            raw = f.read(n)
        dsize = os.path.getsize(os.path.join(part_dir, "%03d.part" % b)) - 4 - n
        for k in range(n // IDX.size):
            o, s, t = IDX.unpack_from(raw, k * IDX.size)
            idx_all += IDX.pack(base + o if t else 0, s, t)
        base += dsize
        sizes.append((n, dsize))
    assert len(idx_all) == ntile * IDX.size
    tmp = out + ".tmp"
    with open(tmp, "wb") as f:
        f.write(head)
        f.write(idx_all)
        for b in range(nbands):
            with open(os.path.join(part_dir, "%03d.part" % b), "rb") as p:
                p.seek(4 + sizes[b][0])
                while True:
                    c = p.read(1 << 24)
                    if not c:
                        break
                    f.write(c)
    os.replace(tmp, out)


def main():
    args = [a for a in sys.argv[1:] if not a.startswith("--")]
    opts = dict(a[2:].split("=", 1) if "=" in a else (a[2:], "1")
                for a in sys.argv[1:] if a.startswith("--"))
    if len(args) != 2:
        print(__doc__)
        return 1
    inp, out = args
    var = opts.get("var", "mssh")
    max_s = float(opts.get("max-seconds", 1e9))
    tp = "tp2wgs84" in opts
    src = Source(inp, var)
    name = opts.get("name") or getattr(src.d, "OriginalName", "") or getattr(src.d, "title", "")
    period = opts.get("period", "")
    if not period:
        ln = getattr(src.v, "long_name", "")
        if "[" in ln and "]" in ln:
            period = ln[ln.index("[") + 1:ln.index("]")].replace(",", "-").replace(" ", "")
    quantity = opts.get("quantity", "MSS/WGS84")
    nty, ntx = (src.ny + T - 1) // T, (src.nx + T - 1) // T
    nbands = (nty + BAND - 1) // BAND
    part_dir = out + ".parts"
    if not os.path.isdir(part_dir):
        os.makedirs(part_dir)
    print("%s : %d x %d nœuds, pas %.5f°, %s, %s" % (name, src.nx, src.ny, src.dlon,
                                                     period or "période inconnue",
                                                     "T/P -> WGS84" if tp else "WGS84"))
    t0 = time.time()
    total = 0
    for b in range(nbands):
        if os.path.exists(os.path.join(part_dir, "%03d.part" % b)):
            continue
        if time.time() - t0 > max_s:
            done = sum(os.path.exists(os.path.join(part_dir, "%03d.part" % k)) for k in range(nbands))
            print("pause (%d/%d parties) : relancer la même commande pour continuer" % (done, nbands))
            return 2
        t = time.time()
        total += encode_band(src, b, ntx, nty, part_dir, tp)
        print("partie %d/%d  %6.1f Mo  %4.0f s" % (b + 1, nbands, total / 1e6, time.time() - t))
        sys.stdout.flush()
    assemble(src, out, part_dir, nbands, ntx, nty, name, period, quantity)
    try:
        for b in range(nbands):
            os.remove(os.path.join(part_dir, "%03d.part" % b))
        os.rmdir(part_dir)
    except OSError:
        print("(dossier %s à supprimer à la main)" % part_dir)
    print("Fichier : %s (%.1f Mo)" % (out, os.path.getsize(out) / 1e6))
    return 0


if __name__ == "__main__":
    sys.exit(main())

# PersonalDepth_pi - fichiers d'essai netCDF-3 des tests (test_core), écrits
# par scipy (lecteur indépendant du nôtre) : python make_nc3.py <dossier>
# Copyright (C) 2026 Marcaurelio Franzetti
# SPDX-License-Identifier: GPL-2.0-or-later
import os
import sys

import numpy as np
from scipy.io import netcdf_file

d = sys.argv[1] if len(sys.argv) > 1 else "."
for version, name in ((1, "nc3_classic.nc"), (2, "nc3_64bit.nc")):
    f = netcdf_file(os.path.join(d, name), "w", version=version)
    f.title = "classic fixture"
    f.ints = np.array([3, -4], dtype=">i4")
    f.createDimension("time", None)  # scipy : la dimension illimitée d'abord
    f.createDimension("lat", 4)
    f.createDimension("lon", 5)
    lat = f.createVariable("lat", "f8", ("lat",))
    lat[:] = [40.0, 40.5, 41.0, 41.5]
    lon = f.createVariable("lon", "f8", ("lon",))
    lon[:] = [-5.0, -4.75, -4.5, -4.25, -4.0]
    z = f.createVariable("Band1", "f4", ("lat", "lon"))
    z.units = "m"
    z._FillValue = np.float32(-9999.0)
    z[:] = np.arange(20, dtype="f4").reshape(4, 5) * 1.5 - 3.0
    z[1, 2] = -9999.0
    t = f.createVariable("t", "i4", ("time",))
    r = f.createVariable("rec", "i2", ("time", "lon"))
    for k in range(3):
        t[k] = 100 + k
        r[k, :] = np.arange(5, dtype="i2") + 10 * k - 20
    f.close()

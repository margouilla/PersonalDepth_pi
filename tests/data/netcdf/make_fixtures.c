/* PersonalDepth_pi - fichiers d'essai netCDF-4 / HDF5 des tests (test_core)
 * Copyright (C) 2026 Marcaurelio Franzetti
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * Écrits avec la bibliothèque HDF5 officielle (lecteur indépendant du nôtre) :
 *   cc make_fixtures.c -lhdf5 -o make_fixtures && ./make_fixtures <dossier>
 * Les fichiers netCDF-3 viennent de make_nc3.py (scipy).
 */
#include <hdf5.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

static hid_t create(const char* dir, const char* name, int latest) {
  char p[1024];
  snprintf(p, sizeof p, "%s/%s", dir, name);
  hid_t fapl = H5Pcreate(H5P_FILE_ACCESS);
  if (latest) H5Pset_libver_bounds(fapl, H5F_LIBVER_V114, H5F_LIBVER_LATEST);
  else H5Pset_libver_bounds(fapl, H5F_LIBVER_EARLIEST, H5F_LIBVER_V18);
  hid_t f = H5Fcreate(p, H5F_ACC_TRUNC, H5P_DEFAULT, fapl);
  H5Pclose(fapl);
  return f;
}

static void attr_str(hid_t o, const char* n, const char* v) {
  hid_t t = H5Tcopy(H5T_C_S1);
  H5Tset_size(t, strlen(v));
  hid_t s = H5Screate(H5S_SCALAR);
  hid_t a = H5Acreate2(o, n, t, s, H5P_DEFAULT, H5P_DEFAULT);
  H5Awrite(a, t, v);
  H5Aclose(a), H5Sclose(s), H5Tclose(t);
}
static void attr_vstr(hid_t o, const char* n, const char* v) {
  hid_t t = H5Tcopy(H5T_C_S1);
  H5Tset_size(t, H5T_VARIABLE);
  hid_t s = H5Screate(H5S_SCALAR);
  hid_t a = H5Acreate2(o, n, t, s, H5P_DEFAULT, H5P_DEFAULT);
  H5Awrite(a, t, &v);
  H5Aclose(a), H5Sclose(s), H5Tclose(t);
}
static void attr_num(hid_t o, const char* n, hid_t ftype, hid_t mtype, const void* v, hsize_t cnt) {
  hid_t s = cnt == 1 ? H5Screate(H5S_SCALAR) : H5Screate_simple(1, &cnt, NULL);
  hid_t a = H5Acreate2(o, n, ftype, s, H5P_DEFAULT, H5P_DEFAULT);
  H5Awrite(a, mtype, v);
  H5Aclose(a), H5Sclose(s);
}
static void vec(hid_t loc, const char* n, hid_t ftype, hid_t mtype, const void* v, hsize_t cnt) {
  hid_t s = H5Screate_simple(1, &cnt, NULL);
  hid_t d = H5Dcreate2(loc, n, ftype, s, H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT);
  H5Dwrite(d, mtype, H5S_ALL, H5S_ALL, H5P_DEFAULT, v);
  H5Dclose(d), H5Sclose(s);
}

/* Ancien format : superbloc v0, en-têtes v1, table des symboles */
static void file_v0(const char* dir) {
  hid_t f = create(dir, "h5_v0.nc", 0);
  double lat[37], lon[53];
  for (int j = 0; j < 37; ++j) lat[j] = -90 + 5 * j;
  for (int i = 0; i < 53; ++i) lon[i] = i * 0.25;
  vec(f, "lat", H5T_IEEE_F64LE, H5T_NATIVE_DOUBLE, lat, 37);
  vec(f, "lon", H5T_IEEE_F64LE, H5T_NATIVE_DOUBLE, lon, 53);
  /* z : blocs 10x16, shuffle + deflate, un bloc jamais écrit (remplissage) */
  static float z[37][53];
  for (int j = 0; j < 37; ++j)
    for (int i = 0; i < 53; ++i) z[j][i] = j * 100 + i + 0.5f;
  hsize_t dims[2] = {37, 53}, ch[2] = {10, 16};
  hid_t sp = H5Screate_simple(2, dims, NULL);
  hid_t dc = H5Pcreate(H5P_DATASET_CREATE);
  H5Pset_chunk(dc, 2, ch);
  H5Pset_shuffle(dc);
  H5Pset_deflate(dc, 4);
  float fill = -999.0f;
  H5Pset_fill_value(dc, H5T_NATIVE_FLOAT, &fill);
  hid_t d = H5Dcreate2(f, "z", H5T_IEEE_F32LE, sp, H5P_DEFAULT, dc, H5P_DEFAULT);
  hsize_t s0[2] = {10, 0}, c0[2] = {27, 53}, s1[2] = {0, 0}, c1[2] = {10, 16}, s2[2] = {0, 32}, c2[2] = {10, 21};
  H5Sselect_hyperslab(sp, H5S_SELECT_SET, s0, NULL, c0, NULL);
  H5Sselect_hyperslab(sp, H5S_SELECT_OR, s1, NULL, c1, NULL);
  H5Sselect_hyperslab(sp, H5S_SELECT_OR, s2, NULL, c2, NULL);
  H5Dwrite(d, H5T_NATIVE_FLOAT, sp, sp, H5P_DEFAULT, z);
  attr_num(d, "_FillValue", H5T_IEEE_F32LE, H5T_NATIVE_FLOAT, &fill, 1);
  attr_str(d, "units", "m");
  attr_str(d, "long_name", "Test grid over the [1993, 2012] period");
  double sc = 0.5;
  attr_num(d, "scale_factor", H5T_IEEE_F64LE, H5T_NATIVE_DOUBLE, &sc, 1);
  H5Dclose(d), H5Pclose(dc), H5Sclose(sp);
  /* be : entiers gros-boutistes, contigu */
  int be[4][5];
  for (int j = 0; j < 4; ++j)
    for (int i = 0; i < 5; ++i) be[j][i] = j * 10 + i - 7;
  hsize_t d2[2] = {4, 5};
  sp = H5Screate_simple(2, d2, NULL);
  d = H5Dcreate2(f, "be", H5T_STD_I32BE, sp, H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT);
  H5Dwrite(d, H5T_NATIVE_INT, H5S_ALL, H5S_ALL, H5P_DEFAULT, be);
  H5Dclose(d), H5Sclose(sp);
  /* u8 : compact */
  unsigned char u8[6] = {0, 1, 2, 200, 254, 255};
  hsize_t n6 = 6;
  sp = H5Screate_simple(1, &n6, NULL);
  dc = H5Pcreate(H5P_DATASET_CREATE);
  H5Pset_layout(dc, H5D_COMPACT);
  d = H5Dcreate2(f, "u8", H5T_STD_U8LE, sp, H5P_DEFAULT, dc, H5P_DEFAULT);
  H5Dwrite(d, H5T_NATIVE_UCHAR, H5S_ALL, H5S_ALL, H5P_DEFAULT, u8);
  H5Dclose(d), H5Pclose(dc), H5Sclose(sp);
  /* sous-groupe */
  hid_t g = H5Gcreate2(f, "grp", H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT);
  short w[3] = {-3, 0, 32767};
  vec(g, "w", H5T_STD_I16LE, H5T_NATIVE_SHORT, w, 3);
  H5Gclose(g);
  attr_str(f, "title", "v0 fixture");
  H5Fclose(f);
}

/* Format récent : superbloc v3, en-têtes v2, liens et attributs denses,
 * disposition v4 (tableau fixe, bloc unique, implicite, tableau extensible) */
static void file_latest(const char* dir) {
  hid_t f = create(dir, "h5_latest.nc", 1);
  for (int k = 0; k < 10; ++k) {
    char n[16];
    snprintf(n, sizeof n, "d%d", k);
    short v[2] = {(short)k, (short)-k};
    vec(f, n, H5T_STD_I16LE, H5T_NATIVE_SHORT, v, 2);
  }
  for (int k = 0; k < 12; ++k) {
    char n[16];
    snprintf(n, sizeof n, "a%d", k);
    int v = k * 11;
    attr_num(f, n, H5T_STD_I32LE, H5T_NATIVE_INT, &v, 1);
  }
  attr_vstr(f, "vstr", "hello vlen");
  double arr[3] = {1.5, -2.25, 1e300};
  attr_num(f, "arr", H5T_IEEE_F64BE, H5T_NATIVE_DOUBLE, arr, 3);
  /* fa : tableau fixe, fletcher32 + shuffle + deflate */
  static double fa[20][30];
  for (int j = 0; j < 20; ++j)
    for (int i = 0; i < 30; ++i) fa[j][i] = j * 1000 + i * 0.5;
  hsize_t dims[2] = {20, 30}, ch[2] = {7, 8};
  hid_t sp = H5Screate_simple(2, dims, NULL);
  hid_t dc = H5Pcreate(H5P_DATASET_CREATE);
  H5Pset_chunk(dc, 2, ch);
  H5Pset_fletcher32(dc);
  H5Pset_shuffle(dc);
  H5Pset_deflate(dc, 6);
  hid_t d = H5Dcreate2(f, "fa", H5T_IEEE_F64LE, sp, H5P_DEFAULT, dc, H5P_DEFAULT);
  H5Dwrite(d, H5T_NATIVE_DOUBLE, H5S_ALL, H5S_ALL, H5P_DEFAULT, fa);
  H5Dclose(d), H5Pclose(dc), H5Sclose(sp);
  /* single : bloc unique filtré */
  short si[5][6];
  for (int j = 0; j < 5; ++j)
    for (int i = 0; i < 6; ++i) si[j][i] = (short)(j * 6 + i - 10);
  hsize_t d2[2] = {5, 6};
  sp = H5Screate_simple(2, d2, NULL);
  dc = H5Pcreate(H5P_DATASET_CREATE);
  H5Pset_chunk(dc, 2, d2);
  H5Pset_deflate(dc, 1);
  d = H5Dcreate2(f, "single", H5T_STD_I16LE, sp, H5P_DEFAULT, dc, H5P_DEFAULT);
  H5Dwrite(d, H5T_NATIVE_SHORT, H5S_ALL, H5S_ALL, H5P_DEFAULT, si);
  H5Dclose(d), H5Pclose(dc), H5Sclose(sp);
  /* implicit : blocs alloués d'emblée, sans filtre */
  int im[6][6];
  for (int j = 0; j < 6; ++j)
    for (int i = 0; i < 6; ++i) im[j][i] = j * 6 + i;
  hsize_t d3[2] = {6, 6}, c3[2] = {3, 3};
  sp = H5Screate_simple(2, d3, NULL);
  dc = H5Pcreate(H5P_DATASET_CREATE);
  H5Pset_chunk(dc, 2, c3);
  H5Pset_alloc_time(dc, H5D_ALLOC_TIME_EARLY);
  d = H5Dcreate2(f, "implicit", H5T_STD_I32LE, sp, H5P_DEFAULT, dc, H5P_DEFAULT);
  H5Dwrite(d, H5T_NATIVE_INT, H5S_ALL, H5S_ALL, H5P_DEFAULT, im);
  H5Dclose(d), H5Pclose(dc), H5Sclose(sp);
  /* ext : dimension illimitée -> tableau extensible (non pris en charge) */
  float ex[4][5] = {{0}};
  hsize_t d4[2] = {4, 5}, m4[2] = {H5S_UNLIMITED, 5}, c4[2] = {2, 5};
  sp = H5Screate_simple(2, d4, m4);
  dc = H5Pcreate(H5P_DATASET_CREATE);
  H5Pset_chunk(dc, 2, c4);
  d = H5Dcreate2(f, "ext", H5T_IEEE_F32LE, sp, H5P_DEFAULT, dc, H5P_DEFAULT);
  H5Dwrite(d, H5T_NATIVE_FLOAT, H5S_ALL, H5S_ALL, H5P_DEFAULT, ex);
  H5Dclose(d), H5Pclose(dc), H5Sclose(sp);
  /* cmp : compact */
  float cmp[3] = {1.25f, -4.5f, 3e10f};
  hsize_t n3 = 3;
  sp = H5Screate_simple(1, &n3, NULL);
  dc = H5Pcreate(H5P_DATASET_CREATE);
  H5Pset_layout(dc, H5D_COMPACT);
  d = H5Dcreate2(f, "cmp", H5T_IEEE_F32LE, sp, H5P_DEFAULT, dc, H5P_DEFAULT);
  H5Dwrite(d, H5T_NATIVE_FLOAT, H5S_ALL, H5S_ALL, H5P_DEFAULT, cmp);
  H5Dclose(d), H5Pclose(dc), H5Sclose(sp);
  hid_t g = H5Gcreate2(f, "sub", H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT);
  signed char in[2] = {-128, 127};
  vec(g, "inner", H5T_STD_I8LE, H5T_NATIVE_SCHAR, in, 2);
  H5Gclose(g);
  H5Fclose(f);
}

/* Petite surface moyenne : latitudes du nord au sud, NaN = absent */
static void file_mss(const char* dir) {
  hid_t f = create(dir, "mss_small.nc", 0);
  enum { NY = 40, NX = 70 };
  double lat[NY], lon[NX];
  for (int j = 0; j < NY; ++j) lat[j] = 10.0 - 0.5 * j;
  for (int i = 0; i < NX; ++i) lon[i] = -5.0 + 0.25 * i;
  vec(f, "lat", H5T_IEEE_F64LE, H5T_NATIVE_DOUBLE, lat, NY);
  vec(f, "lon", H5T_IEEE_F64LE, H5T_NATIVE_DOUBLE, lon, NX);
  static double m[NY][NX];
  for (int j = 0; j < NY; ++j)
    for (int i = 0; i < NX; ++i)
      m[j][i] = (i + j) % 17 == 0 ? NAN : 40.0 * sin(0.1 * i) + 0.37 * j - 12.3456;
  m[3][3] = 120.0;  /* écart > int16 en mm avec ses voisins */
  hsize_t dims[2] = {NY, NX}, ch[2] = {16, 24};
  hid_t sp = H5Screate_simple(2, dims, NULL);
  hid_t dc = H5Pcreate(H5P_DATASET_CREATE);
  H5Pset_chunk(dc, 2, ch);
  H5Pset_deflate(dc, 5);
  hid_t d = H5Dcreate2(f, "mssh", H5T_IEEE_F64LE, sp, H5P_DEFAULT, dc, H5P_DEFAULT);
  H5Dwrite(d, H5T_NATIVE_DOUBLE, H5S_ALL, H5S_ALL, H5P_DEFAULT, m);
  double nan = NAN;
  attr_num(d, "_FillValue", H5T_IEEE_F64LE, H5T_NATIVE_DOUBLE, &nan, 1);
  attr_str(d, "long_name", "Mean Sea Surface Height over the [1993, 2012] period");
  H5Dclose(d), H5Pclose(dc), H5Sclose(sp);
  attr_str(f, "title", "Small MSS");
  H5Fclose(f);
}

/* Petit atlas FES : 34 ondes, lambda2 seulement dans ocean_tide */
static const char* kWaves[] = {"2n2", "eps2", "j1", "k1", "k2", "l2", "lambda2", "m2", "m3", "m4", "m6", "m8",
                               "mks2", "mn4", "ms4", "msf", "mf", "mm", "msqm", "mtm", "mu2", "n2", "n4",
                               "nu2", "o1", "p1", "q1", "r2", "s1", "s2", "s4", "sa", "ssa", "t2"};
static void file_fes(const char* dir) {
  enum { NY = 7, NX = 12 };
  for (int w = 0; w < 34; ++w) {
    char sub[1024];
    snprintf(sub, sizeof sub, "%s/fes/%s", dir, w == 6 ? "ocean_tide" : "ocean_tide_extrapolated");
    char name[64];
    snprintf(name, sizeof name, "%s_fes2022.nc", kWaves[w]);
    hid_t f = create(sub, name, 1);
    float lat[NY], lon[NX];
    for (int j = 0; j < NY; ++j) lat[j] = -90.0f + 30.0f * j;
    for (int i = 0; i < NX; ++i) lon[i] = 30.0f * i;
    vec(f, "lat", H5T_IEEE_F32LE, H5T_NATIVE_FLOAT, lat, NY);
    vec(f, "lon", H5T_IEEE_F32LE, H5T_NATIVE_FLOAT, lon, NX);
    static float a[NY][NX], g[NY][NX];
    const float fill = 18446744073709551616.0f;
    for (int j = 0; j < NY; ++j)
      for (int i = 0; i < NX; ++i) {
        const int land = (i * 7 + j * 3 + w) % 11 == 0;
        a[j][i] = land ? fill : 10.0f + w + (i + j) % 7 + 0.37f;
        g[j][i] = land ? fill : (float)((i * 10 + j * 5 + w * 3) % 360) + 0.25f;
      }
    hsize_t dims[2] = {NY, NX}, ch[2] = {NY, 5};
    const char* vn[2] = {"amplitude", "phase"};
    float(*vv[2])[NX] = {a, g};
    for (int k = 0; k < 2; ++k) {
      hid_t sp = H5Screate_simple(2, dims, NULL);
      hid_t dc = H5Pcreate(H5P_DATASET_CREATE);
      H5Pset_chunk(dc, 2, ch);
      H5Pset_deflate(dc, 1);
      hid_t d = H5Dcreate2(f, vn[k], H5T_IEEE_F32LE, sp, H5P_DEFAULT, dc, H5P_DEFAULT);
      H5Dwrite(d, H5T_NATIVE_FLOAT, H5S_ALL, H5S_ALL, H5P_DEFAULT, vv[k]);
      attr_num(d, "_FillValue", H5T_IEEE_F32LE, H5T_NATIVE_FLOAT, &fill, 1);
      H5Dclose(d), H5Pclose(dc), H5Sclose(sp);
    }
    H5Fclose(f);
  }
}

int main(int argc, char** argv) {
  const char* dir = argc > 1 ? argv[1] : ".";
  file_v0(dir);
  file_latest(dir);
  file_mss(dir);
  file_fes(dir);
  return 0;
}

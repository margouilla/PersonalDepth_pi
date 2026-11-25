// PersonalDepth_pi - tests du lecteur netCDF / HDF5 et des conversions
// .pdgrid / .pdtide (fichiers d'essai : tests/data/netcdf, voir make_fixtures.c
// et make_nc3.py)
// Copyright (C) 2026 Marcaurelio Franzetti
// SPDX-License-Identifier: GPL-2.0-or-later

#ifndef PD_TEST_DATA
#define PD_TEST_DATA "tests/data"
#endif

static std::string NcData(const char* n) { return std::string(PD_TEST_DATA) + "/netcdf/" + n; }
static std::string NcLower(std::string s) {
  for (char& c : s) c = (char)std::tolower((unsigned char)c);
  return s;
}

static int TestNetCdf() {
  std::string err;
  std::vector<double> v;
  {  // HDF5 ancien format : superbloc v0, table des symboles, blocs B-tree v1
    NcFile f;
    CHECK(f.open(NcData("h5_v0.nc"), err));
    CHECK(f.format() == 4);
    const NcVar* z = f.var("z");
    CHECK(z && z->shape.size() == 2 && z->shape[0] == 37 && z->shape[1] == 53 && z->floating && z->bytes == 4);
    CHECK(z->str("units") == "m" && z->num("scale_factor", 0) == 0.5 && z->num("_FillValue", 0) == -999.0);
    CHECK(z->str("long_name").find("[1993, 2012]") != std::string::npos);
    CHECK(f.coord(*z, 0, 'y') == f.var("lat") && f.coord(*z, 1, 'x') == f.var("lon"));
    CHECK(f.readAll(*z, v, err) && v.size() == 37 * 53);
    for (int j = 0; j < 37; ++j)
      for (int i = 0; i < 53; ++i) {
        const double want = (j < 10 && i >= 16 && i < 32) ? -999.0 : j * 100 + i + 0.5;  // bloc non écrit
        CHECK(v[j * 53 + i] == want);
      }
    CHECK(f.read(*z, {9, 15}, {3, 18}, v, err) && v.size() == 54);  // à cheval sur 4 blocs
    CHECK(v[0] == 9 * 100 + 15 + 0.5 && v[1] == -999.0 && v[18] == 10 * 100 + 15 + 0.5 && v[53] == 11 * 100 + 32 + 0.5);
    CHECK(!f.read(*z, {30, 0}, {8, 53}, v, err));  // hors des dimensions
    const NcVar* be = f.var("be");
    CHECK(be && f.readAll(*be, v, err) && v.size() == 20 && v[0] == -7 && v[19] == 3 * 10 + 4 - 7);
    const NcVar* u8 = f.var("u8");
    CHECK(u8 && f.readAll(*u8, v, err) && v.size() == 6 && v[3] == 200 && v[5] == 255);
    const NcVar* w = f.var("grp/w");
    CHECK(w && f.readAll(*w, v, err) && v[0] == -3 && v[2] == 32767);
    bool title = false;
    for (const auto& a : f.globals()) title = title || (a.name == "title" && a.str == "v0 fixture");
    CHECK(title);
    const NcVar* lat = f.var("lat");
    CHECK(lat && f.readAll(*lat, v, err) && v.front() == -90 && v.back() == 90);
  }
  {  // HDF5 récent : liens et attributs denses, disposition v4
    NcFile f;
    CHECK(f.open(NcData("h5_latest.nc"), err));
    int nd = 0;
    for (int k = 0; k < 10; ++k) nd += f.var("d" + std::to_string(k)) != nullptr;
    CHECK(nd == 10);
    int na = 0;
    std::string vs;
    std::vector<double> arr;
    for (const auto& a : f.globals()) {
      if (a.name.size() >= 2 && a.name[0] == 'a' && std::isdigit((unsigned char)a.name[1]) && a.num.size() == 1 &&
          a.num[0] == std::stoi(a.name.substr(1)) * 11)
        ++na;
      if (a.name == "vstr") vs = a.str;
      if (a.name == "arr") arr = a.num;
    }
    CHECK(na == 12 && vs == "hello vlen");
    CHECK(arr.size() == 3 && arr[0] == 1.5 && arr[1] == -2.25 && arr[2] == 1e300);
    const NcVar* fa = f.var("fa");
    CHECK(fa && f.readAll(*fa, v, err) && v.size() == 600);
    for (int j = 0; j < 20; ++j)
      for (int i = 0; i < 30; ++i) CHECK(v[j * 30 + i] == j * 1000 + i * 0.5);
    const NcVar* si = f.var("single");
    CHECK(si && f.read(*si, {1, 2}, {3, 4}, v, err) && v.size() == 12 && v[0] == 1 * 6 + 2 - 10 && v[11] == 3 * 6 + 5 - 10);
    const NcVar* im = f.var("implicit");
    CHECK(im && f.readAll(*im, v, err) && v.size() == 36);
    for (int k = 0; k < 36; ++k) CHECK(v[k] == k);
    const NcVar* cmp = f.var("cmp");
    CHECK(cmp && f.readAll(*cmp, v, err) && v[0] == 1.25 && v[1] == -4.5 && v[2] == (double)3e10f);
    const NcVar* in = f.var("sub/inner");
    CHECK(in && f.readAll(*in, v, err) && v[0] == -128 && v[1] == 127);
    const NcVar* ex = f.var("ext");  // tableau extensible : listé, lecture refusée avec un message
    CHECK(ex && !ex->numeric);
    err.clear();
    CHECK(!f.readAll(*ex, v, err) && err.find("extensible") != std::string::npos);
  }
  for (const char* n : {"nc3_classic.nc", "nc3_64bit.nc"}) {  // netCDF-3, variables d'enregistrement
    NcFile f;
    CHECK(f.open(NcData(n), err));
    CHECK(f.format() == 3);
    const NcVar* b = f.var("Band1");
    CHECK(b && b->shape.size() == 2 && b->shape[0] == 4 && b->shape[1] == 5 && b->dims[0] == "lat");
    CHECK(b->str("units") == "m" && b->num("_FillValue", 0) == -9999.0);
    CHECK(f.coord(*b, 0, 'y') == f.var("lat") && f.coord(*b, 1, 'x') == f.var("lon"));
    CHECK(f.readAll(*b, v, err) && v[0] == -3.0 && v[7] == -9999.0 && v[19] == 19 * 1.5 - 3.0);
    const NcVar* t = f.var("t");
    CHECK(t && t->shape[0] == 3 && f.readAll(*t, v, err) && v[0] == 100 && v[2] == 102);
    const NcVar* r = f.var("rec");
    CHECK(r && f.read(*r, {1, 1}, {2, 3}, v, err) && v.size() == 6 && v[0] == 1 + 10 - 20 && v[5] == 3 + 20 - 20);
    bool ints = false;
    for (const auto& a : f.globals()) ints = ints || (a.name == "ints" && a.num.size() == 2 && a.num[1] == -4);
    CHECK(ints);
  }
  {  // fichiers refusés proprement
    NcFile f;
    CHECK(!f.open(NcData("absent.nc"), err));
    const std::string bad = Tmp("pd_bad.nc");
    FILE* o = std::fopen(bad.c_str(), "wb");
    CHECK(o);
    std::fputs("ceci n'est pas un fichier netCDF", o);
    std::fclose(o);
    CHECK(!f.open(bad, err) && !err.empty());
    // HDF5 tronqué
    std::vector<uint8_t> all;
    FILE* s = std::fopen(NcData("h5_latest.nc").c_str(), "rb");
    CHECK(s);
    uint8_t buf[4096];
    size_t k;
    while ((k = std::fread(buf, 1, sizeof buf, s)) > 0) all.insert(all.end(), buf, buf + k);
    std::fclose(s);
    for (size_t cut : {(size_t)40, (size_t)700, all.size() / 2}) {
      o = std::fopen(bad.c_str(), "wb");
      std::fwrite(all.data(), 1, cut, o);
      std::fclose(o);
      NcFile g;
      std::string e2;
      if (g.open(bad, e2)) {  // ouverture possible : la lecture doit échouer ou réussir sans planter
        for (const auto& x : g.vars()) g.readAll(x, v, e2);
      } else {
        CHECK(!e2.empty());
      }
    }
    RemoveFile(bad);
  }
  {  // surface moyenne -> .pdgrid (latitudes du nord au sud, NaN, écart > int16)
    const std::string out = Tmp("pd_mss_small.pdgrid");
    RemoveFile(out);
    int calls = 0;
    ConvertProgress prog = [&](double, const std::string&) { ++calls; return true; };
    CHECK(ConvertToPdgrid(NcData("mss_small.nc"), out, PdgridOptions(), prog, err) == 1);
    CHECK(calls > 0);
    GeoGridFile g;
    CHECK(g.open(out, err));
    CHECK(g.quantity() == "MSS/WGS84" && g.model() == "Small MSS" && g.period() == "1993-2012");
    NcFile f;
    CHECK(f.open(NcData("mss_small.nc"), err));
    std::vector<double> m, lat, lon;
    CHECK(f.readAll(*f.var("mssh"), m, err) && f.readAll(*f.var("lat"), lat, err) && f.readAll(*f.var("lon"), lon, err));
    int nodes = 0;
    for (int j = 0; j < 40; ++j)
      for (int i = 0; i < 70; ++i) {
        const double x = m[j * 70 + i];
        if (std::isnan(x)) continue;
        const double got = g.sample(lon[i], lat[j]);
        CHECK(std::fabs(got - std::nearbyint(x * 1000.0) / 1000.0) < 1e-9);
        ++nodes;
      }
    CHECK(nodes > 2400);
    // interruption puis reprise : même fichier
    const std::string out2 = Tmp("pd_mss_small2.pdgrid");
    RemoveFile(out2);
    int n = 0;
    ConvertProgress stop = [&](double, const std::string&) { return ++n < 2; };
    CHECK(ConvertToPdgrid(NcData("mss_small.nc"), out2, PdgridOptions(), stop, err) == 0);
    CHECK(ConvertToPdgrid(NcData("mss_small.nc"), out2, PdgridOptions(), prog, err) == 1);
    GeoGridFile g2;
    CHECK(g2.open(out2, err));
    for (int j = 0; j < 40; j += 3)
      for (int i = 0; i < 70; i += 5) {
        const double a = g.sample(lon[i], lat[j]), b = g2.sample(lon[i], lat[j]);
        CHECK((std::isnan(a) && std::isnan(b)) || a == b);
      }
    // TOPEX/Poseidon -> WGS84 : environ +0,7 m à l'équateur, une fois de plus que sans
    PdgridOptions tp;
    tp.tp2wgs84 = true;
    const std::string out3 = Tmp("pd_mss_tp.pdgrid");
    RemoveFile(out3);
    CHECK(ConvertToPdgrid(NcData("mss_small.nc"), out3, tp, prog, err) == 1);
    GeoGridFile g3;
    CHECK(g3.open(out3, err));
    const double d0 = g3.sample(lon[1], lat[20]) - g.sample(lon[1], lat[20]);
    CHECK(d0 < -0.69 && d0 > -0.71);
    CHECK(ConvertToPdgrid(NcData("absent.nc"), out3, PdgridOptions(), prog, err) == -1 && !err.empty());
    PdgridOptions bad;
    bad.var = "nope";
    CHECK(ConvertToPdgrid(NcData("mss_small.nc"), out3, bad, prog, err) == -1 && err.find("nope") != std::string::npos);
    g.close();
    g2.close();
    g3.close();
    RemoveFile(out);
    RemoveFile(out2);
    RemoveFile(out3);
  }
  {  // atlas FES -> .pdtide (34 ondes, Lambda2 dans ocean_tide)
    const std::string dir = FindFesFolder(NcData("fes"));
    CHECK(!dir.empty() && dir.find("ocean_tide_extrapolated") != std::string::npos);
    CHECK(FindFesFolder(NcData("fes/ocean_tide_extrapolated/m2_fes2022.nc")) == dir);
    CHECK(FindFesFolder(NcData("")).empty());
    const std::string out = Tmp("pd_fes_small.pdtide");
    RemoveFile(out);
    int n = 0;
    ConvertProgress stop = [&](double, const std::string&) { return ++n < 40; };
    CHECK(ConvertFesToPdtide(dir, out, stop, err) == 0);  // interrompue, puis reprise
    ConvertProgress go = [](double, const std::string&) { return true; };
    CHECK(ConvertFesToPdtide(dir, out, go, err) == 1);
    TideAtlas atlas;
    CHECK(atlas.open(out, err));
    CHECK(atlas.waveNames().size() == 34 && atlas.waveNames()[6] == "Lambda2");
    atlas.setInferMinor(false);
    TideRegion r;
    CHECK(atlas.loadRegion(25, 95, -35, 35, r, err));
    int checked = 0;
    for (size_t w = 0; w < r.waves.size(); ++w) {
      int wi = -1;
      for (size_t k = 0; k < atlas.waveNames().size(); ++k)
        if (NcLower(atlas.waveNames()[k]) == NcLower(r.waves[w]->name)) wi = (int)k;
      if (wi < 0) continue;
      for (int j = 0; j < r.ny; ++j)
        for (int i = 0; i < r.nx; ++i) {
          const int gi = (int)std::lround((r.lon0 + i * r.dlon) / 30.0), gj = (int)std::lround((r.lat0 + j * r.dlat + 90) / 30.0);
          if (gi < 0 || gi >= 12 || gj < 0 || gj >= 7) continue;
          const bool land = (gi * 7 + gj * 3 + wi) % 11 == 0;
          const size_t at = ((size_t)w * r.ny + j) * r.nx + i;
          if (land) continue;  // un nœud sec d'une onde suffit à l'invalider pour toutes
          if (!r.valid[(size_t)j * r.nx + i]) continue;
          const double a = (double)(10.0f + wi + (gi + gj) % 7 + 0.37f);
          const double g = ((double)((float)((gi * 10 + gj * 5 + wi * 3) % 360) + 0.25f)) * (3.141592653589793 / 180.0);
          CHECK(std::fabs(r.re[at] - std::nearbyint(a * std::cos(g) * 10.0) / 1000.0) < 1e-6);
          CHECK(std::fabs(r.im[at] - std::nearbyint(a * std::sin(g) * 10.0) / 1000.0) < 1e-6);
          ++checked;
        }
    }
    CHECK(checked > 200);
    RemoveFile(out);
  }
  {  // grilles de zéro des cartes des pays -> .pdgrid : netCDF de GDAL, GeoTIFF, .gtx
    const std::string dir = Tmp("pd_national");
    std::filesystem::remove_all(dir);
    std::filesystem::create_directories(dir);
    ConvertProgress go = [](double, const std::string&) { return true; };
    // netCDF-3 de gdal_translate (Band1), niveau moyen au-dessus du ZH -> signe inversé
    PdgridOptions o;
    o.var.clear();
    o.quantity = "CD/MSL";
    o.negate = true;
    o.name = "Essai CD";
    CHECK(ConvertToPdgrid(NcData("nc3_classic.nc"), dir + "/a.pdgrid", o, go, err) == 1);
    GeoGridFile a;
    CHECK(a.open(dir + "/a.pdgrid", err) && a.quantity() == "CD/MSL" && a.model() == "Essai CD");
    CHECK(std::fabs(a.sample(-5.0, 40.0) - 3.0) < 1e-9);                // -(-3.0)
    CHECK(std::fabs(a.sample(-4.0, 41.5) - (-(19 * 1.5 - 3.0))) < 1e-9);  // dernier nœud
    a.close();
    // GeoTIFF en degrés (EPSG:4326), NoData
    tiffw::Img im;
    im.w = 6; im.h = 4; im.sx = 0.5; im.sy = 0.25; im.x0 = -3; im.y0 = 50;
    im.keys = {1024, 0, 1, 2, 2048, 0, 1, 4326};
    im.nodata = "-9999";
    for (int r = 0; r < im.h; ++r)
      for (int c = 0; c < im.w; ++c) im.v.push_back(r == 1 && c == 1 ? -9999.0 : 10 + r + 0.125 * c);
    const std::string tif = dir + "/cd.tif";
    CHECK(tiffw::write(tif, im));
    PdgridOptions t;
    t.var.clear();
    t.quantity = "CD/WGS84";
    CHECK(ConvertToPdgrid(tif, dir + "/b.pdgrid", t, go, err) == 1);
    GeoGridFile b;
    CHECK(b.open(dir + "/b.pdgrid", err) && b.quantity() == "CD/WGS84" && b.model() == "cd");
    for (int r = 0; r < im.h; ++r)
      for (int c = 0; c < im.w; ++c) {
        if (r == 1 && c == 1) continue;
        CHECK(std::fabs(b.sample(-3 + (c + 0.5) * 0.5, 50 - (r + 0.5) * 0.25) - (10 + r + 0.125 * c)) < 1e-9);
      }
    b.close();
    // GeoTIFF projeté : refusé avec un message
    tiffw::Img pj = im;
    pj.keys = {1024, 0, 1, 1, 3072, 0, 1, 32630};
    pj.x0 = 400000; pj.y0 = 5500000; pj.sx = 100; pj.sy = 100;
    CHECK(tiffw::write(dir + "/utm.tif", pj));
    err.clear();
    CHECK(ConvertToPdgrid(dir + "/utm.tif", dir + "/c.pdgrid", t, go, err) == -1 && !err.empty());
    // .gtx (NOAA / PROJ) : gros-boutiste, ligne sud d'abord, longitudes 0..360
    {
      std::vector<uint8_t> g;
      auto d64 = [&](double x) { uint64_t u; std::memcpy(&u, &x, 8); for (int k = 7; k >= 0; --k) g.push_back((uint8_t)(u >> (8 * k))); };
      auto i32 = [&](uint32_t u) { for (int k = 3; k >= 0; --k) g.push_back((uint8_t)(u >> (8 * k))); };
      d64(45); d64(355); d64(0.5); d64(0.5); i32(3); i32(4);
      for (int r = 0; r < 3; ++r)
        for (int c = 0; c < 4; ++c) {
          float x = (float)(r * 10 + c - 2.5);
          uint32_t u; std::memcpy(&u, &x, 4); i32(u);
        }
      FILE* f = std::fopen((dir + "/cd.gtx").c_str(), "wb");
      CHECK(f);
      std::fwrite(g.data(), 1, g.size(), f);
      std::fclose(f);
    }
    t.quantity = "CD/MSL";
    CHECK(ConvertToPdgrid(dir + "/cd.gtx", dir + "/d.pdgrid", t, go, err) == 1);
    GeoGridFile d;
    CHECK(d.open(dir + "/d.pdgrid", err));
    for (int r = 0; r < 3; ++r)
      for (int c = 0; c < 4; ++c) CHECK(std::fabs(d.sample(-5 + c * 0.5, 45 + r * 0.5) - (r * 10 + c - 2.5)) < 1e-9);
    d.close();
    // reconnus dans le dossier « Pays »
    std::filesystem::remove(dir + "/cd.gtx");  // un .gtx quelconque n'est pas un MLLW VDatum
    const auto nat = ScanNationalDatums(dir);
    int msl = 0, ell = 0;
    for (const auto& x : nat) {
      msl += x.kind == NationalDatum::CdBelowMsl;
      ell += x.kind == NationalDatum::CdEllipsoid;
    }
    CHECK(msl == 2 && ell == 1);
    std::filesystem::remove_all(dir);
  }
  {  // unité des stations de marée d'OpenCPN : .tcd (libtcd v2) et HARMONIC texte
    CHECK(TideUnitToMetres("feet") == 0.3048 && TideUnitToMetres(" meters\r") == 1.0 && std::isnan(TideUnitToMetres("knots")));
    // .tcd minimal : entête texte, somme, unités de niveau, un enregistrement
    std::vector<uint8_t> d;
    const char* head =
        "[VERSION] = test\n[MAJOR REV] = 2\n[HEADER SIZE] = 1024\n[RECORD SIZE BITS] = 16\n"
        "[RECORD TYPE BITS] = 4\n[LATITUDE BITS] = 25\n[LATITUDE SCALE] = 100000\n[LONGITUDE BITS] = 26\n"
        "[LONGITUDE SCALE] = 100000\n[TZFILE BITS] = 10\n[STATION BITS] = 18\n[COUNTRY BITS] = 9\n"
        "[RESTRICTION BITS] = 4\n[LEGALESE BITS] = 4\n[DATE BITS] = 27\n[DIRECTION UNIT BITS] = 2\n"
        "[DIRECTION BITS] = 9\n[LEVEL UNIT BITS] = 3\n[LEVEL UNIT TYPES] = 5\n[LEVEL UNIT SIZE] = 15\n"
        "[END OF ASCII HEADER DATA]\n";
    d.assign(head, head + std::strlen(head));
    d.resize(1024 + 4, 0);
    for (const char* u : {"Unknown", "feet", "meters", "knots", "knots^2"}) {
      std::vector<uint8_t> e(15, 0);
      std::memcpy(e.data(), u, std::strlen(u));
      d.insert(d.end(), e.begin(), e.end());
    }
    d.resize(d.size() + 37, 0xA5);  // autres tables (non lues)
    std::vector<int> bits;
    auto put = [&](long long v, int n) { for (int k = n - 1; k >= 0; --k) bits.push_back((int)((v >> k) & 1)); };
    auto str = [&](const char* t) { for (const char* c = t; ; ++c) { put((unsigned char)*c, 8); if (!*c) break; } };
    auto rec = [&](const char* name, double lat, double lon, int unit) {
      put(200, 16); put(1, 4);
      put((long long)std::llround(lat * 1e5) & ((1LL << 25) - 1), 25);
      put((long long)std::llround(lon * 1e5) & ((1LL << 26) - 1), 26);
      put(3, 10); str(name); put((1 << 18) - 1, 18); put(7, 9); str("source"); put(0, 4);
      str("comments"); str(""); put(0, 4); str("ctx"); str("8665530"); put(20210110, 27); str("");
      put(1, 2); put(361, 9); put(361, 9); put(unit, 3);
      put(0, 13);  // suite de l'enregistrement
    };
    put(0, 3);  // enregistrements non alignés sur l'octet
    rec("Port Test, Somewhere", 32.78083, -79.92361, 1);
    rec("Port Test, Somewhere", 48.3591, -4.7808, 2);  // homonyme ailleurs, en mètres
    while (bits.size() % 8) bits.push_back(0);
    for (size_t k = 0; k < bits.size(); k += 8) {
      int b = 0;
      for (int j = 0; j < 8; ++j) b = (b << 1) | bits[k + j];
      d.push_back((uint8_t)b);
    }
    const std::string tcd = Tmp("pd_units.tcd");
    FILE* f = std::fopen(tcd.c_str(), "wb");
    CHECK(f);
    std::fwrite(d.data(), 1, d.size(), f);
    std::fclose(f);
    CHECK(TcdStationToMetres(tcd, "Port Test, Somewhere", 32.78083, -79.92361) == 0.3048);
    CHECK(TcdStationToMetres(tcd, "Port Test, Somewhere", 48.3591, -4.7808) == 1.0);
    CHECK(std::isnan(TcdStationToMetres(tcd, "Port Test", 32.78083, -79.92361)));  // nom entier seulement
    CHECK(std::isnan(TcdStationToMetres(tcd, "Port Test, Somewhere", 10, 10)));
    RemoveFile(tcd);
    // HARMONIC texte (format XTide 1) et son index
    const std::string har = Tmp("pd_HARMONIC");
    f = std::fopen(har.c_str(), "wb");
    CHECK(f);
    std::fputs("# commentaire\r\nAberdeen, Washington\r\n-08:00 :America/Los_Angeles\r\n5.5600 feet\r\nM2 1 2\n"
               "Halifax, Nova Scotia\n-04:00 :America/Halifax\n1.0210 meters\n", f);
    std::fclose(f);
    f = std::fopen((har + ".IDX").c_str(), "wb");
    CHECK(f);
    std::fputs("TNAA:Can:NovS: -63.5833 44.6667 -4:0 Halifax, Nova Scotia\n"
               "tNAP:USA:WA: -123.9 46.9 -8:0 Westport, Washington\n&10 1 0.2 12 1 0.1 77 1 Aberdeen, Washington\n", f);
    std::fclose(f);
    CHECK(HarmonicStationToMetres(har, "Aberdeen, Washington") == 0.3048);
    CHECK(HarmonicStationToMetres(har, "Halifax, Nova Scotia") == 1.0);
    CHECK(HarmonicStationToMetres(har, "Westport, Washington") == 0.3048);  // secondaire : sa référence
    CHECK(std::isnan(HarmonicStationToMetres(har, "Brest")));
    RemoveFile(har);
    RemoveFile(har + ".IDX");
  }
  {  // catalogue : boutons « Convertir… »
    Catalog c = catalog::Load("");
    const CatalogItem* fes = c.find("fes2022b");
    const CatalogItem* mss = c.find("mss2025");
    CHECK(fes && fes->convert == "fes" && mss && mss->convert == "mss");
    const CatalogItem* cd = c.find("national-grid");
    CHECK(cd && cd->convert == "cd" && cd->use == "national");
    CHECK(c.find("raf20") && c.find("raf20")->convert.empty());
  }
  return 0;
}

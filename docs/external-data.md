# External data / Données externes

[🇬🇧 English](#english) · [🇫🇷 Français](#français)

PersonalDepth works without any of these files (OpenCPN tide stations, data
at chart datum shown as imported). They are needed for the gridded FES2022
tide and to link the vertical datums: the database stores depths below mean
sea level and shows them at chart datum, LAT, mean sea level or the ellipsoid.
They are **not included** in the plugin because of their licences.

**The quick way — *External data* window.** *Data* tab, *Vertical reference
surfaces* box (or *Tide* tab), **External data…**: the list shows each file,
its producer, licence and whether it is installed.
- **Open data** (BathyElli ZH and NM, RAF20, RAC23): **Download**, or
  **Everything missing (France)**. The plugin shows the licence, downloads the
  file from the producer's own website, checks its SHA-256 fingerprint,
  unpacks it (`.7z` archives of the Shom) into the reference folder, then
  offers to update the database surfaces. No Python, nothing else to install.
- **Data needing an account** (FES2022, MSS — AVISO): **Open the page** leads
  to the steps below; once converted, **Choose the file…** sets it in the
  plugin. Direct download of these two is waiting for AVISO's agreement.
- **NOAA VDatum**: opens NOAA's download page (agreement to accept);
  **Choose the folder…** sets the *Countries* folder.
- **Check the files** checks again the SHA-256 fingerprints of the installed
  files (damaged file, or another version).
- **Update the list** fetches the up-to-date catalogue (addresses, new data)
  from the plugin repository, without a new plugin version.
- Observed water levels of online tide gauges (REFMAR in France, NOAA CO-OPS
  in the United States): see section 6.

PersonalDepth fonctionne sans ces fichiers (stations de marée d'OpenCPN,
données au zéro des cartes affichées telles qu'importées). Ils servent à la
marée maillée FES2022 et à relier les références verticales : la base range
les profondeurs sous le niveau moyen et les affiche au zéro des cartes, au
LAT, au niveau moyen ou à l'ellipsoïde. Ils ne sont **pas fournis** avec le
plugin, en raison de leurs licences.

**Le plus simple — fenêtre *Données externes*.** Onglet *Données*, cadre
*Surfaces de référence verticale* (ou onglet *Marée*), **Données externes…** :
la liste donne chaque fichier, son producteur, sa licence et s'il est installé.
- **Données libres** (BathyElli ZH et NM, RAF20, RAC23) : **Télécharger**, ou
  **Tout ce qui manque (France)**. Le plugin affiche la licence, télécharge le
  fichier sur le site même du producteur, contrôle son empreinte SHA-256, le
  décompresse (archives `.7z` du Shom) dans le dossier des références, puis
  propose de mettre à jour les surfaces de la base. Ni Python ni autre
  logiciel à installer.
- **Données à compte** (FES2022, MSS — AVISO) : **Ouvrir la page** mène aux
  étapes ci-dessous ; une fois converti, **Choisir le fichier…** le règle dans
  le plugin. Le téléchargement direct de ces deux données attend l'accord
  d'AVISO.
- **NOAA VDatum** : ouvre la page de téléchargement de la NOAA (accord à
  accepter) ; **Choisir le dossier…** règle le dossier *Pays*.
- **Vérifier les fichiers** recontrôle les empreintes SHA-256 des fichiers
  installés (fichier abîmé, ou autre version).
- **Mettre à jour la liste** récupère le catalogue à jour (adresses, nouvelles
  données) sur le dépôt du plugin, sans nouvelle version du plugin.
- Hauteurs d'eau observées des marégraphes en ligne (REFMAR en France, NOAA
  CO-OPS aux États-Unis) : voir section 6.

| Data / Donnée | Producer / Producteur | Used for / Sert à | Files / Fichiers |
|---|---|---|---|
| FES2022b ocean tide | AVISO / CNES | Gridded tide, LAT, chart datum outside France / marée maillée, LAT, zéro des cartes hors France | `*.nc` → `FES2022b_ocean.pdtide` |
| MSS CNES_CLS 2025 | AVISO / CNES, CLS | Mean sea level above the ellipsoid outside France / niveau moyen au-dessus de l'ellipsoïde hors France | `mss_cnes_cls_2025.nc` → `MSS_CNES_CLS_2025.pdgrid` |
| BathyElli v2.1 ZH/ell | Shom | Chart datum (ZH) in France, IGN69 → ZH / zéro des cartes (ZH) en France, IGN69 → ZH | `*ZH*.glhi` |
| BathyElli v2.1 NM/ell | Shom | Mean sea level in France, tide Z0 / niveau moyen en France, Z0 de la marée | `*NM*.glhi` |
| RAF20 | IGN | IGN69 (mainland France / continent) | `RAF20.tac` |
| RAC23 | IGN | IGN78C (Corsica / Corse) | `RAC23.mnt` |
| OSMSHP (OpenStreetMap land polygons) | OpenStreetMap contributors, via OpenCPN | Coastline: no interpolation on land / trait de côte : pas d'interpolation sur la terre | `basemap_*.shp` |
| GSHHG | NOAA / SOEST, via OpenCPN | Coastline (coarser) / trait de côte (plus grossier) | `poly-*-1.dat` |
| VDatum | NOAA | Official chart datum (MLLW) in the United States / zéro des cartes officiel (MLLW) aux États-Unis | `mllw.gtx` of each region / de chaque région |

---

## English

### 1. Reference folder (BathyElli, RAF20, RAC23)

Put all these files in **one folder**, for example
`D:\Bathymetry\ReferenceElevation\`. PersonalDepth searches it, including
sub-folders, by file name:

```
ReferenceElevation\
  ...ZH....glhi      BathyElli v2.1 ZH/ell (one file per zone)
  ...NM....glhi      BathyElli v2.1 NM/ell (only for the FES2022 tide)
  RAF20.tac          IGN, mainland France
  RAC23.mnt          IGN, Corsica (optional)
```

**BathyElli v2.1** (Shom, *Licence Ouverte / Open Licence 2.0*)
- In the *External data* window: **Download** (`.7z` archives of the Shom,
  unpacked by the plugin).
- By hand: [data.gouv.fr — Surfaces BathyElli v2.1](https://www.data.gouv.fr/datasets/surfaces-bathyelli-v2-1-1),
  files **ZH / ell** and **NM / ell** (`.7z` archives containing the
  `.glhi`): together they link chart datum (ZH) to mean sea level in France,
  for the database and the FES2022 tide.
- Keep the original file names: PersonalDepth recognises them by `ZH` and
  `NM` in the name.

**RAF20** (IGN): geoid grid converting ellipsoidal heights to IGN69.
- In the *External data* window: **Download**.
- By hand: [IGN geodesy website — conversion grids](https://geodesie.ign.fr/grilles-de-conversion)
  (direct link `https://www.ign.fr/publications-de-l-ign/Geodesie/RAF20.tac`).
- Keep the `.tac` format. PROJ's GeoTIFF version `fr_ign_RAF20.tif` is not
  read by PersonalDepth.

**RAC23** (IGN): same for IGN78C in Corsica. File `RAC23.mnt`
(`https://www.ign.fr/publications-de-l-ign/Geodesie/RAC23.mnt`), also in the
*External data* window. Only needed for surveys in Corsica.

IGN grids: *Licence Ouverte / Open Licence 2.0*.

**In PersonalDepth**
- The *External data* window saves the files in the reference folder
  (*Folder* row of the window, the same as *France folder* of the import).
- *Data management… › Import…*, section "Vertical reference":
  - **IGN69 / IGN78C → CD**: choose the reference folder. It must contain
    `*ZH*.glhi` and `RAF20.tac`. `RAC23.mnt` is used automatically in
    Corsica.
- The folder is remembered for the next imports. Its `*NM*.glhi` and
  `*ZH*.glhi` files are also used by the database (chart datum and mean sea
  level in France, ellipsoidal heights).

### 2. FES2022 gridded tide

**Download** (AVISO, free registration; "can be used for any purpose"
according to AVISO, but redistribution is not allowed by PersonalDepth):
1. Register at [AVISO+](https://www.aviso.altimetry.fr/en/data/products/auxiliary-products/global-tide-fes.html)
   and select the product **FES (Finite Element Solution — Oceanic Tides Heights)**.
2. Download the folder **`fes2022b/ocean_tide_extrapolated`** (one NetCDF
   file per tidal constituent), via FTP or the
   [THREDDS catalog](https://tds.aviso.altimetry.fr/thredds/catalog/dataset-auxiliary-fes-tide-model/fes2022b/ocean_tide_extrapolated/catalog.html).
3. Also download **`Lambda2`** from the neighbouring **`fes2022b/ocean_tide`**
   folder (missing from the extrapolated set), and keep both folders side by
   side:
   ```
   fes2022b\
     ocean_tide\               (at least lambda2_fes2022.nc)
     ocean_tide_extrapolated\  (all constituents)
   ```

**Convert** to the compact `.pdtide` format (≈ 230 MB, worldwide, full
resolution), once:
```
pip install numpy netCDF4
python tools/pdtide_convert.py <...>\fes2022b\ocean_tide_extrapolated FES2022b_ocean.pdtide
```
The conversion takes a long time but can be resumed. Add
`--max-seconds=1800` to work in batches and run the same command again to
continue: finished constituents are kept in `FES2022b_ocean.pdtide.parts`.

**In PersonalDepth**, *Tide* section:
1. Source: **Gridded FES2022**.
2. *Atlas*: choose `FES2022b_ocean.pdtide`.
3. Chart datum (Z0), by priority:
   - an optional user grid (*Z0 grid*);
   - BathyElli NM/ell − ZH/ell: tick the box. The `*NM*.glhi` and
     `*ZH*.glhi` files must be in the reference folder chosen at import;
   - otherwise the lowest astronomical tide computed from FES2022 over
     19 years (approximation of chart datum).
4. Mode: current time or chosen date and time. *Tide curve…* plots the
   height versus time at up to 9 points spread over the data.

The atlas is also used by the database: lowest astronomical tide (LAT) for
sources imported at LAT (EMODnet) and for the display at LAT, and chart
datum outside France.

### 3. Mean sea surface (MSS) CNES_CLS 2025

Height of mean sea level above the WGS84 ellipsoid (1993–2012 average,
1/60° grid, built by CNES and CLS from satellite altimetry). PersonalDepth
uses it outside France, where BathyElli NM does not exist, for GNSS surveys
(ellipsoidal heights) and for the display at the ellipsoid.

**Download** (AVISO, same account as FES2022; AVISO licence, redistribution
not allowed by PersonalDepth):
1. In the [AVISO+](https://www.aviso.altimetry.fr/en/data/products/auxiliary-products/mss/mss-global-cnes-cls-2025.html)
   registration form, select the product **MSS_CNES_CLS (Mean Sea Surface)**.
2. Download `mss_cnes_cls_2025.nc` (≈ 1.7 GB), by FTP (`/auxiliary/mss/mss_cnes_cls_2025`)
   or THREDDS. The [handbook](https://www.aviso.altimetry.fr/fileadmin/documents/data/tools/hdbk_MSS-CNES-CLS_2025.pdf)
   describes the product.

**Convert** to the compact `.pdgrid` format (≈ 124 MB, no loss at the
millimetre, about one minute), once:
```
pip install numpy netCDF4
python tools/pdgrid_convert.py mss_cnes_cls_2025.nc MSS_CNES_CLS_2025.pdgrid
```
Another mean sea surface (for example a later CNES_CLS or the Hybrid MSS) can
be converted the same way; a grid above the TOPEX/Poseidon ellipsoid is
converted with `--tp2wgs84`.

**In PersonalDepth**: *Data* tab, *Vertical reference surfaces* box, **MSS**: choose
`MSS_CNES_CLS_2025.pdgrid`. After changing it, **Update surfaces** corrects
the sources concerned. Cite: *CNES_CLS 2025 Mean Sea Surface (Version 2025),
CNES, DOI 10.24400/527896/a01-2024.011*.

### 4. Coastline (OSMSHP, GSHHG, own shorelines)

Land polygons used so that the interpolated surface does not cover islands
and the coast. Both are downloaded by OpenCPN itself: *Options › Charts ›
Chart Downloader*, catalogues **OSMSHP** (OpenStreetMap land polygons,
`basemap_crude/low/medium/high/full`, recommended) and **GSHHG**
(`poly-c/l/i/h/f-1.dat`, coarser). Note the folder where they are saved.

- OSMSHP: © OpenStreetMap contributors, ODbL. The `full` quality (≈ 1.8 GB)
  follows the coast to a few metres.
- GSHHG: LGPL; last version 2017; even `f` can be offset by about a hundred
  metres, and small islands are missing.

**In PersonalDepth**: *Data* tab, *Coastline* box, **Folder**: choose that
folder (or its `OSMSHP` / `GSHHG` subfolder). OSMSHP is preferred, in the
best quality present.

#### Own shorelines (KML, shapefile, GeoPackage)

The basemaps miss small lakes, reservoirs and rivers (classed as land) and
may be outdated. The *Own* row of the same box takes KML / KMZ, shapefile
and GeoPackage files, in addition to the basemap or alone:

- **Google Earth**: draw the outline of the lake (*Add › Polygon*), its
  islands, or the shore as a path, then *Save place as…* `.kml` or `.kmz`.
- **OpenStreetMap**, current data from a regional extract (e.g.
  [Geofabrik](https://download.geofabrik.de/)): reading `.osm.pbf` inside
  the plugin would need gigabytes of memory (impossible on a Raspberry Pi or
  in 32 bits), so the extract is converted once on a PC with GDAL (QGIS
  installs it) and, for large extracts, osmium:

  ```
  osmium tags-filter region-latest.osm.pbf nwr/natural=water nwr/landuse=reservoir -o water.osm.pbf
  ogr2ogr -f GPKG water.gpkg water.osm.pbf multipolygons -where "natural='water' OR landuse='reservoir'" -spat 103.5 51.4 106.0 53.0
  ```

  `-spat lon_min lat_min lon_max lat_max` keeps the area of the surveys only
  (smaller file, read faster). The polygons trace water: the plugin takes
  the side of the soundings as water side, so lakes and their islands are
  recognised without setting anything.
- Any GIS layer (QGIS, ArcGIS): shapefile or GeoPackage, polygons or lines,
  in WGS84 or in a projected system known to the plugin (`.prj` / layer
  system).
- **France**: the *Limite terre-mer* of the Shom and IGN (highest
  astronomical tide line, much more accurate than OSM) is used as it is:
  `Limite_terre-mer_France_metropolitaine_polygones.shp` (land polygons,
  Lambert 93). Large shapefiles are read on demand, by 1° square: the
  170 MB national file opens in a fraction of a second with a few MB of
  memory, also on a Raspberry Pi.

OSM data: © OpenStreetMap contributors, ODbL.

### 5. Official chart datum of other countries

Outside France, the plugin uses the FES2022 lowest astronomical tide (LAT) as
chart datum. This is right wherever LAT **is** the official chart datum
(Netherlands, Germany, Belgium, Australia… and close to it in the United
Kingdom), but some countries define their own: the **United States** chart
depths below MLLW (mean lower low water), **Canada** below LLWLT (lower low
water, large tide). There, LAT FES can differ from the charts by several
decimetres.

**In PersonalDepth**: *Data* tab, *Vertical reference surfaces* box, **Countries**:
choose one folder and put the official files in it (sub-folders allowed).
The plugin recognises them and uses them where they exist; elsewhere it
keeps LAT FES. The box says in plain words which one applies to the chart on
screen ("✓ official chart datum" or "≈ approximate datum here"). Changing the
folder recomputes the surfaces of the database.

| Country | Chart datum | File | Access | In the plugin |
|---|---|---|---|---|
| United States | MLLW | NOAA VDatum regional grids | Free | Read directly (`mllw.gtx`) |
| Netherlands | LAT (NLLAT2018) | NLLAT2018 | Free, "not for navigation" | After conversion (below); LAT FES is already close |
| United Kingdom | CD ≈ LAT (VORF) | VORF | On request to the UKHO, licence | After conversion |
| Canada | LLWLT (HyVSEPs) | CHS HyVSEPs | On request to the CHS | After conversion |

**United States (NOAA VDatum)**

1. Go to [vdatum.noaa.gov](https://vdatum.noaa.gov/), *Download*, accept the
   agreement and download the transformation grids of the regions you sail
   in (one archive per coastal region).
2. Unzip them into the *Countries* folder. Each region folder holds a
   `mllw.gtx` (MLLW relative to local mean sea level): it is the only file
   the plugin reads. Neighbouring regions may overlap: they are averaged.

**Other countries** — a file giving the chart datum relative to mean sea
level, or above the ellipsoid, is converted once to `.pdgrid` (GDAL, then
`tools/pdgrid_convert.py`):

```
gdal_translate -of netCDF chart_datum_grid.tif cd.nc
python tools/pdgrid_convert.py cd.nc CD_country.pdgrid --var=Band1 --quantity=CD/WGS84 --name="VORF CD"
```

- `--quantity=CD/MSL`: values = chart datum − mean sea level (negative);
- `--quantity=CD/WGS84`: chart datum height above the ellipsoid; mean sea
  level then comes from BathyElli or from the MSS (section 3).

Any `.pdgrid` of quantity `CD/MSL` or `CD/WGS84` in the *Countries* folder is
used; its name (`--name`) is shown in the box and under the cursor.

### 6. Observed water levels of online tide gauges (REFMAR, NOAA)

The Shom publishes the measurements of about 180 tide gauges on the French
coasts (REFMAR network: RONIM and partner gauges), above chart datum, in UTC,
under *Licence Ouverte / Open Licence 2.0*. They correct the soundings of a
survey for the real water level (tide and surge), better than a prediction.

**In PersonalDepth**: *Data management… › Import…*, section *Observed water
levels*, **Tide gauge…**:
1. the gauges are listed by distance from the centre of the chart (search
   box to find one by name);
2. choose the period of the soundings (start and end dates, inclusive, UTC);
3. **Download**: one file per month, saved in `REFMAR\<id>_<NAME>\` inside
   the reference folder and read directly (best source of each day: validated
   10-min data, then raw delayed, raw real-time, validated hourly). Periods
   already downloaded are kept; the last days are downloaded again (recent
   raw data are completed and validated later by the Shom).

*Tide* tab, source **Online tide gauge: observed height**: the measured
water height of the nearest gauge (or a chosen one) is added to the depths,
now (fetched every 5 minutes) or at a chosen past date.

**United States — NOAA CO-OPS.** The same works with the NOAA tide gauges
(about 240 tidal stations, public domain, 6-minute water levels above MLLW,
the US chart datum, in UTC): choose the *NOAA CO-OPS* network in the tide
gauge window; in *nearest* mode, the nearest gauge of all networks is used.
Files are saved in `NOAA\<id>_<NAME>\`. NOAA files downloaded by hand are
not read as they are (their datum and units are not written in the file).

Files downloaded by hand from [data.shom.fr](https://data.shom.fr) (text
format `DD/MM/YYYY hh:mm:ss;value;source`) are also recognised as they are.
Cite "REFMAR" (DOI 10.17183/REFMAR) if you share a result.

---

## Français

### 1. Dossier des références (BathyElli, RAF20, RAC23)

Rangez tous ces fichiers dans **un même dossier**, par exemple
`D:\Bathymetry\ReferenceElevation\`. PersonalDepth les y cherche, sous-dossiers
compris, d'après leur nom :

```
ReferenceElevation\
  ...ZH....glhi      BathyElli v2.1 ZH/ell (un fichier par zone)
  ...NM....glhi      BathyElli v2.1 NM/ell (seulement pour la marée FES2022)
  RAF20.tac          IGN, France continentale
  RAC23.mnt          IGN, Corse (facultatif)
```

**BathyElli v2.1** (Shom, *Licence Ouverte 2.0*)
- Dans la fenêtre *Données externes* : **Télécharger** (archives `.7z` du
  Shom, décompressées par le plugin).
- À la main : [data.gouv.fr — Surfaces BathyElli v2.1](https://www.data.gouv.fr/datasets/surfaces-bathyelli-v2-1-1),
  fichiers **ZH / ell** et **NM / ell** (archives `.7z` contenant les
  `.glhi`) : ensemble, ils relient le zéro hydrographique au niveau moyen en
  France, pour la base et pour la marée FES2022.
- Garder les noms d'origine : PersonalDepth les reconnaît à `ZH` et `NM` dans
  le nom.

**RAF20** (IGN) : grille de géoïde, hauteurs ellipsoïdales → IGN69.
- Dans la fenêtre *Données externes* : **Télécharger**.
- À la main : [site de géodésie de l'IGN — grilles de conversion](https://geodesie.ign.fr/grilles-de-conversion)
  (lien direct `https://www.ign.fr/publications-de-l-ign/Geodesie/RAF20.tac`).
- Garder le format `.tac`. La version GeoTIFF de PROJ `fr_ign_RAF20.tif` n'est
  pas lue par PersonalDepth.

**RAC23** (IGN) : idem pour l'IGN78C en Corse. Fichier `RAC23.mnt`
(`https://www.ign.fr/publications-de-l-ign/Geodesie/RAC23.mnt`), aussi dans
la fenêtre *Données externes*. Utile seulement pour des levés en Corse.

Grilles de l'IGN : *Licence Ouverte 2.0*.

**Dans PersonalDepth**
- La fenêtre *Données externes* range les fichiers dans le dossier des
  références (ligne *Dossier* de la fenêtre, le même que *Dossier France* de
  l'import).
- *Gestion des données… › Importer…*, section « Référence verticale » :
  - **IGN69 / IGN78C → ZH** : choisir le dossier des références. Il doit
    contenir `*ZH*.glhi` et `RAF20.tac`. `RAC23.mnt` est utilisé
    automatiquement en Corse.
- Le dossier est mémorisé pour les imports suivants. Ses fichiers
  `*NM*.glhi` et `*ZH*.glhi` servent aussi à la base (zéro hydrographique et
  niveau moyen en France, hauteurs ellipsoïdales).

### 2. Marée maillée FES2022

**Téléchargement** (AVISO, inscription gratuite ; utilisable « pour tout
usage » selon AVISO, mais PersonalDepth ne le redistribue pas) :
1. S'inscrire sur [AVISO+](https://www.aviso.altimetry.fr/en/data/products/auxiliary-products/global-tide-fes.html)
   en choisissant le produit **FES (Finite Element Solution — Oceanic Tides Heights)**.
2. Télécharger le dossier **`fes2022b/ocean_tide_extrapolated`** (un fichier
   NetCDF par onde), par FTP ou par le
   [catalogue THREDDS](https://tds.aviso.altimetry.fr/thredds/catalog/dataset-auxiliary-fes-tide-model/fes2022b/ocean_tide_extrapolated/catalog.html).
3. Télécharger aussi **`Lambda2`** dans le dossier voisin
   **`fes2022b/ocean_tide`** (absente du jeu extrapolé), en gardant les deux
   dossiers côte à côte :
   ```
   fes2022b\
     ocean_tide\               (au moins lambda2_fes2022.nc)
     ocean_tide_extrapolated\  (toutes les ondes)
   ```

**Conversion** au format compact `.pdtide` (≈ 230 Mo, monde entier, pleine
résolution), une seule fois :
```
pip install numpy netCDF4
python tools/pdtide_convert.py <...>\fes2022b\ocean_tide_extrapolated FES2022b_ocean.pdtide
```
La conversion est longue mais reprend là où elle s'est arrêtée : ajouter
`--max-seconds=1800` pour travailler par tranches et relancer la même
commande. Les ondes terminées sont gardées dans `FES2022b_ocean.pdtide.parts`.

**Dans PersonalDepth**, cadre *Marée* :
1. Source : **FES2022 maillé**.
2. *Atlas* : choisir `FES2022b_ocean.pdtide`.
3. Référence Z0 (niveau moyen au-dessus du ZH), par priorité :
   - une grille fournie (*Grille Z0*, facultative) ;
   - BathyElli NM/ell − ZH/ell : cocher la case. Les fichiers `*NM*.glhi` et
     `*ZH*.glhi` doivent être dans le dossier des références choisi à
     l'import ;
   - sinon la plus basse mer astronomique calculée par FES2022 sur 19 ans
     (approximation du ZH).
4. Mode : heure actuelle, ou date et heure choisies. *Courbe de marée…* trace
   la hauteur en fonction du temps en 9 points au plus, répartis sur les données.

L'atlas sert aussi à la base : plus basse mer astronomique (LAT) pour les
sources importées au LAT (EMODnet) et pour l'affichage au LAT, et zéro des
cartes hors de France.

### 3. Surface moyenne océanique (MSS) CNES_CLS 2025

Hauteur du niveau moyen au-dessus de l'ellipsoïde WGS84 (moyenne 1993–2012,
grille au 1/60°, calculée par le CNES et CLS par altimétrie satellitaire).
PersonalDepth s'en sert hors de France, là où BathyElli NM n'existe pas, pour
les levés GNSS (hauteurs ellipsoïdales) et l'affichage à l'ellipsoïde.

**Téléchargement** (AVISO, même compte que FES2022 ; licence AVISO,
PersonalDepth ne le redistribue pas) :
1. Dans le formulaire d'inscription [AVISO+](https://www.aviso.altimetry.fr/en/data/products/auxiliary-products/mss/mss-global-cnes-cls-2025.html),
   choisir le produit **MSS_CNES_CLS (Mean Sea Surface)**.
2. Télécharger `mss_cnes_cls_2025.nc` (≈ 1,7 Go), par FTP
   (`/auxiliary/mss/mss_cnes_cls_2025`) ou THREDDS. Le
   [manuel](https://www.aviso.altimetry.fr/fileadmin/documents/data/tools/hdbk_MSS-CNES-CLS_2025.pdf)
   décrit le produit.

**Conversion** au format compact `.pdgrid` (≈ 124 Mo, sans perte au
millimètre, une minute environ), une seule fois :
```
pip install numpy netCDF4
python tools/pdgrid_convert.py mss_cnes_cls_2025.nc MSS_CNES_CLS_2025.pdgrid
```
Une autre surface moyenne (une version CNES_CLS ultérieure, la Hybrid MSS…)
se convertit de la même façon ; une grille au-dessus de l'ellipsoïde
TOPEX/Poseidon se convertit avec `--tp2wgs84`.

**Dans PersonalDepth** : onglet *Données*, cadre *Surfaces de référence verticale*, **MSS** :
choisir `MSS_CNES_CLS_2025.pdgrid`. Après un changement, **Mettre à jour les
surfaces** corrige les sources concernées. Citer : *CNES_CLS 2025 Mean Sea
Surface (Version 2025), CNES, DOI 10.24400/527896/a01-2024.011*.

### 4. Trait de côte (OSMSHP, GSHHG, traits personnels)

Polygones de terre servant à ce que la surface interpolée ne couvre pas les
îles et la côte. Les deux se téléchargent dans OpenCPN : *Options › Cartes ›
Téléchargeur de cartes*, catalogues **OSMSHP** (polygones de terre
d'OpenStreetMap, `basemap_crude/low/medium/high/full`, conseillé) et
**GSHHG** (`poly-c/l/i/h/f-1.dat`, plus grossier). Noter le dossier
d'enregistrement.

- OSMSHP : © contributeurs d'OpenStreetMap, ODbL. La qualité `full` (≈ 1,8 Go)
  suit la côte à quelques mètres.
- GSHHG : LGPL ; dernière version de 2017 ; même en `f`, le trait peut être
  décalé d'une centaine de mètres, et les petites îles manquent.

**Dans PersonalDepth** : onglet *Données*, cadre *Trait de côte*, **Dossier** :
choisir ce dossier (ou son sous-dossier `OSMSHP` / `GSHHG`). OSMSHP est
préféré, dans la meilleure qualité présente.

#### Traits de côte personnels (KML, shapefile, GeoPackage)

Les fonds ignorent les petits lacs, retenues et rivières (rangés en terre) et
peuvent être anciens. La ligne *Personnels* du même cadre prend des fichiers
KML / KMZ, shapefile et GeoPackage, en plus du fond ou seuls :

- **Google Earth** : dessiner le contour du lac (*Ajouter › Polygone*), ses
  îles, ou le rivage en trajet, puis *Enregistrer le lieu sous…* `.kml` ou
  `.kmz`.
- **OpenStreetMap**, données à jour d'un extrait régional (ex.
  [Geofabrik](https://download.geofabrik.de/)) : lire un `.osm.pbf` dans le
  plugin demanderait des gigaoctets de mémoire (impossible sur Raspberry Pi
  ou en 32 bits) ; l'extrait est converti une fois sur PC avec GDAL (installé
  avec QGIS) et, pour les gros extraits, osmium :

  ```
  osmium tags-filter region-latest.osm.pbf nwr/natural=water nwr/landuse=reservoir -o eau.osm.pbf
  ogr2ogr -f GPKG eau.gpkg eau.osm.pbf multipolygons -where "natural='water' OR landuse='reservoir'" -spat 103.5 51.4 106.0 53.0
  ```

  `-spat lon_min lat_min lon_max lat_max` ne garde que la zone des levés
  (fichier plus petit, lu plus vite). Les polygones tracent l'eau : le
  plugin prend le côté des sondes comme côté eau, lacs et îles sont reconnus
  sans réglage.
- Toute couche SIG (QGIS, ArcGIS) : shapefile ou GeoPackage, polygones ou
  lignes, en WGS84 ou dans un système projeté connu du plugin (`.prj` /
  système de la couche).
- **France** : la *Limite terre-mer* du Shom et de l'IGN (laisse des plus
  hautes mers astronomiques, bien plus précise qu'OSM) s'utilise telle
  quelle : `Limite_terre-mer_France_metropolitaine_polygones.shp`
  (polygones de terre, Lambert 93). Les gros shapefiles sont lus à la
  demande, par carré de 1° : le fichier national de 170 Mo s'ouvre en une
  fraction de seconde avec quelques Mo de mémoire, aussi sur Raspberry Pi.

Données OSM : © contributeurs d'OpenStreetMap, ODbL.

### 5. Zéro des cartes officiel des autres pays

Hors de France, le plugin prend comme zéro des cartes la plus basse mer
astronomique (LAT) de la marée FES2022. C'est juste partout où le LAT **est**
le zéro officiel (Pays-Bas, Allemagne, Belgique, Australie… et presque au
Royaume-Uni), mais certains pays ont le leur : les **États-Unis** sondent
sous le MLLW (moyenne des plus basses mers quotidiennes), le **Canada** sous
le LLWLT (plus basse mer, grande marée). Là, le LAT FES peut s'écarter des
cartes de plusieurs décimètres.

**Dans PersonalDepth** : onglet *Données*, cadre *Surfaces de référence verticale*, **Pays** :
choisir un dossier et y déposer les fichiers officiels (sous-dossiers
acceptés). Le plugin les reconnaît et les utilise là où ils existent ;
ailleurs il garde le LAT FES. Le cadre dit en clair lequel s'applique à la
carte affichée (« ✓ zéro officiel du pays » ou « ≈ zéro approché ici »).
Changer de dossier recalcule les surfaces de la base.

| Pays | Zéro des cartes | Fichier | Accès | Dans le plugin |
|---|---|---|---|---|
| États-Unis | MLLW | Grilles régionales NOAA VDatum | Gratuit | Lu directement (`mllw.gtx`) |
| Pays-Bas | LAT (NLLAT2018) | NLLAT2018 | Gratuit, « pas pour la navigation » | Après conversion (ci-dessous) ; le LAT FES en est déjà proche |
| Royaume-Uni | CD ≈ LAT (VORF) | VORF | Sur demande à l'UKHO, licence | Après conversion |
| Canada | LLWLT (HyVSEPs) | HyVSEPs du SHC | Sur demande au SHC | Après conversion |

**États-Unis (NOAA VDatum)**

1. Aller sur [vdatum.noaa.gov](https://vdatum.noaa.gov/), *Download*,
   accepter les conditions et télécharger les grilles de transformation des
   régions où l'on navigue (une archive par région côtière).
2. Les décompresser dans le dossier *Pays*. Chaque dossier de région contient
   un `mllw.gtx` (MLLW par rapport au niveau moyen local) : c'est le seul
   fichier que lit le plugin. Les régions voisines peuvent se chevaucher :
   elles sont moyennées.

**Autres pays** — un fichier donnant le zéro des cartes par rapport au niveau
moyen, ou au-dessus de l'ellipsoïde, est converti une fois en `.pdgrid` (GDAL,
puis `tools/pdgrid_convert.py`) :

```
gdal_translate -of netCDF grille_zero_des_cartes.tif cd.nc
python tools/pdgrid_convert.py cd.nc CD_pays.pdgrid --var=Band1 --quantity=CD/WGS84 --name="VORF CD"
```

- `--quantity=CD/MSL` : valeurs = zéro des cartes − niveau moyen (négatives) ;
- `--quantity=CD/WGS84` : hauteur du zéro des cartes au-dessus de
  l'ellipsoïde ; le niveau moyen vient alors de BathyElli ou de la MSS
  (section 3).

Tout `.pdgrid` de grandeur `CD/MSL` ou `CD/WGS84` du dossier *Pays* est
utilisé ; son nom (`--name`) s'affiche dans le cadre et sous le curseur.

### 6. Hauteurs d'eau observées des marégraphes en ligne (REFMAR, NOAA)

Le Shom diffuse les mesures d'environ 180 marégraphes des côtes françaises
(réseau REFMAR : marégraphes RONIM et partenaires), au zéro hydrographique,
en UTC, sous *Licence Ouverte 2.0*. Elles corrigent les sondes d'un levé de
la hauteur d'eau réelle (marée et surcote), mieux qu'une prédiction.

**Dans PersonalDepth** : *Gestion des données… › Importer…*, section
*Hauteurs d'eau observées*, **Marégraphe…** :
1. les marégraphes sont classés par distance au centre de la carte (case de
   recherche pour en trouver un par son nom) ;
2. choisir la période des sondes (dates de début et de fin incluses, UTC) ;
3. **Télécharger** : un fichier par mois, rangé dans `REFMAR\<id>_<NOM>\` du
   dossier des références et lu directement (meilleure source de chaque jour :
   validées 10 min, puis brutes temps différé, brutes temps réel, validées
   horaires). Les périodes déjà téléchargées sont gardées ; les derniers jours sont
   repris (les données brutes récentes sont complétées et validées plus tard
   par le Shom).

Onglet *Marée*, source **Marégraphe en ligne : hauteur observée** : la
hauteur d'eau mesurée au marégraphe le plus proche (ou à celui choisi)
s'ajoute aux profondeurs, maintenant (reprise toutes les 5 minutes) ou à une
date passée choisie.

**États-Unis — NOAA CO-OPS.** Il en est de même avec les marégraphes de la
NOAA (environ 240 stations à marée, domaine public, hauteurs d'eau toutes les
6 minutes au-dessus du MLLW, le zéro des cartes américain, en UTC) : choisir
le réseau *NOAA CO-OPS* dans la fenêtre des marégraphes ; en mode *le plus
proche*, c'est le marégraphe le plus proche, tous réseaux confondus. Fichiers
rangés dans `NOAA\<id>_<NOM>\`. Les fichiers NOAA téléchargés à la main ne
sont pas lus tels quels (ni leur référence ni leur unité n'y sont écrites).

Les fichiers téléchargés à la main sur [data.shom.fr](https://data.shom.fr)
(format texte `JJ/MM/AAAA hh:mm:ss;valeur;source`) sont aussi reconnus tels
quels. Citer « REFMAR » (DOI 10.17183/REFMAR) si vous diffusez un résultat.

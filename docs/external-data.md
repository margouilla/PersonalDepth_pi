# External data / Données externes

[🇬🇧 English](#english) · [🇫🇷 Français](#français)

PersonalDepth works without any of these files (OpenCPN tide stations, data
at chart datum shown as imported). They are needed for the gridded FES2022
tide and to link the vertical datums: the database stores depths below mean
sea level and shows them at chart datum, LAT, mean sea level or the ellipsoid.
They are **not included** in the plugin because of their licences.

PersonalDepth fonctionne sans ces fichiers (stations de marée d'OpenCPN,
données au zéro des cartes affichées telles qu'importées). Ils servent à la
marée maillée FES2022 et à relier les références verticales : la base range
les profondeurs sous le niveau moyen et les affiche au zéro des cartes, au
LAT, au niveau moyen ou à l'ellipsoïde. Ils ne sont **pas fournis** avec le
plugin, en raison de leurs licences.

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
- Download page: [data.gouv.fr — Surfaces BathyElli v2.1](https://www.data.gouv.fr/datasets/surfaces-bathyelli-v2-1-1)
- Download the **ZH / ell** and **NM / ell** files (`.glhi`): together they
  link chart datum (ZH) to mean sea level in France, for the database and
  the FES2022 tide.
- Keep the original file names: PersonalDepth recognises them by `ZH` and
  `NM` in the name.

**RAF20** (IGN): geoid grid converting ellipsoidal heights to IGN69.
- Direct link (as published by IGN):
  `https://geodesie.ign.fr/contenu/fichiers/documentation/grilles/metropole/RAF20.tac`
- Otherwise: [IGN geodesy website](https://geodesie.ign.fr), "grilles"
  (conversion grids) page.
- Keep the `.tac` format. PROJ's GeoTIFF version `fr_ign_RAF20.tif` is not
  read by PersonalDepth.

**RAC23** (IGN): same for IGN78C in Corsica. File `RAC23.mnt`, on the same
IGN geodesy website. Only needed for surveys in Corsica.

**In PersonalDepth**
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

**In PersonalDepth**: *Database* tab, *Vertical datum* box, **MSS**: choose
`MSS_CNES_CLS_2025.pdgrid`. After changing it, **Update surfaces** corrects
the sources concerned. Cite: *CNES_CLS 2025 Mean Sea Surface (Version 2025),
CNES, DOI 10.24400/527896/a01-2024.011*.

### 4. Coastline (OSMSHP, GSHHG)

Land polygons used so that the interpolated surface does not cover islands
and the coast. Both are downloaded by OpenCPN itself: *Options › Charts ›
Chart Downloader*, catalogues **OSMSHP** (OpenStreetMap land polygons,
`basemap_crude/low/medium/high/full`, recommended) and **GSHHG**
(`poly-c/l/i/h/f-1.dat`, coarser). Note the folder where they are saved.

- OSMSHP: © OpenStreetMap contributors, ODbL. The `full` quality (≈ 1.8 GB)
  follows the coast to a few metres.
- GSHHG: LGPL; last version 2017; even `f` can be offset by about a hundred
  metres, and small islands are missing.

**In PersonalDepth**: *Database* tab, *Coastline* box, **Folder**: choose that
folder (or its `OSMSHP` / `GSHHG` subfolder). OSMSHP is preferred, in the
best quality present.

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
- Page de téléchargement : [data.gouv.fr — Surfaces BathyElli v2.1](https://www.data.gouv.fr/datasets/surfaces-bathyelli-v2-1-1)
- Télécharger les fichiers **ZH / ell** et **NM / ell** (`.glhi`) : ensemble,
  ils relient le zéro hydrographique au niveau moyen en France, pour la base
  et pour la marée FES2022.
- Garder les noms d'origine : PersonalDepth les reconnaît à `ZH` et `NM` dans
  le nom.

**RAF20** (IGN) : grille de géoïde, hauteurs ellipsoïdales → IGN69.
- Lien direct (publié par l'IGN) :
  `https://geodesie.ign.fr/contenu/fichiers/documentation/grilles/metropole/RAF20.tac`
- Sinon : [site de géodésie de l'IGN](https://geodesie.ign.fr), page des
  grilles de conversion.
- Garder le format `.tac`. La version GeoTIFF de PROJ `fr_ign_RAF20.tif` n'est
  pas lue par PersonalDepth.

**RAC23** (IGN) : idem pour l'IGN78C en Corse. Fichier `RAC23.mnt`, sur le
même site de géodésie de l'IGN. Utile seulement pour des levés en Corse.

**Dans PersonalDepth**
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

**Dans PersonalDepth** : onglet *Base*, cadre *Référence verticale*, **MSS** :
choisir `MSS_CNES_CLS_2025.pdgrid`. Après un changement, **Mettre à jour les
surfaces** corrige les sources concernées. Citer : *CNES_CLS 2025 Mean Sea
Surface (Version 2025), CNES, DOI 10.24400/527896/a01-2024.011*.

### 4. Trait de côte (OSMSHP, GSHHG)

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

**Dans PersonalDepth** : onglet *Base*, cadre *Trait de côte*, **Dossier** :
choisir ce dossier (ou son sous-dossier `OSMSHP` / `GSHHG`). OSMSHP est
préféré, dans la meilleure qualité présente.

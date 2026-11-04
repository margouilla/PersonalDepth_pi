# External data / Données externes

[🇬🇧 English](#english) · [🇫🇷 Français](#français)

PersonalDepth works without any of these files (OpenCPN tide stations, data
already referenced to chart datum). They are only needed for the gridded
FES2022 tide and for French vertical datum conversions. They are **not
included** in the plugin because of their licences.

PersonalDepth fonctionne sans ces fichiers (stations de marée d'OpenCPN,
données déjà au zéro hydrographique). Ils ne servent qu'à la marée maillée
FES2022 et aux conversions de référence verticale en France. Ils ne sont
**pas fournis** avec le plugin, en raison de leurs licences.

| Data / Donnée | Producer / Producteur | Used for / Sert à | Files / Fichiers |
|---|---|---|---|
| FES2022b ocean tide | AVISO / CNES | Gridded tide / marée maillée | `*.nc` → `FES2022b_ocean.pdtide` |
| BathyElli v2.1 ZH/ell | Shom | IGN69 → ZH, ellipsoidal → ZH | `*ZH*.glhi` |
| BathyElli v2.1 NM/ell | Shom | Tide reference Z0 / Z0 de la marée | `*NM*.glhi` |
| RAF20 | IGN | IGN69 (mainland France / continent) | `RAF20.tac` |
| RAC23 | IGN | IGN78C (Corsica / Corse) | `RAC23.mnt` |

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
- Download the **ZH / ell** files (`.glhi`), and also **NM / ell** if you
  want the FES2022 tide referenced to chart datum by BathyElli.
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
- *Gestion des données… › Importer…*, section "Référence verticale des Z":
  - **IGN69 / IGN78C → ZH**: choose the reference folder. It must contain
    `*ZH*.glhi` and `RAF20.tac`. `RAC23.mnt` is used automatically in
    Corsica.
  - **Ellipsoidal heights → ZH** (GNSS survey without geoid): only the
    `*ZH*.glhi` files are needed.
- The folder is remembered for the next imports.

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

**In PersonalDepth**, *Marée* (tide) section:
1. Source: **FES2022 maillé** (gridded).
2. *Atlas*: choose `FES2022b_ocean.pdtide`.
3. Chart datum (Z0), by priority:
   - an optional user grid (*Grille Z0*);
   - BathyElli NM/ell − ZH/ell: tick the box. The `*NM*.glhi` and
     `*ZH*.glhi` files must be in the reference folder chosen at import;
   - otherwise the lowest astronomical tide computed from FES2022 over
     19 years (approximation of chart datum).
4. Mode: current time or chosen date and time.

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
- Télécharger les fichiers **ZH / ell** (`.glhi`), et aussi **NM / ell** pour
  référencer la marée FES2022 au zéro hydrographique par BathyElli.
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
- *Gestion des données… › Importer…*, cadre « Référence verticale des Z » :
  - **IGN69 / IGN78C → ZH** : choisir le dossier des références. Il doit
    contenir `*ZH*.glhi` et `RAF20.tac`. `RAC23.mnt` est utilisé
    automatiquement en Corse.
  - **Hauteurs ellipsoïdales → ZH** (levé GNSS sans géoïde) : seuls les
    fichiers `*ZH*.glhi` sont nécessaires.
- Le dossier est mémorisé pour les imports suivants.

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
4. Mode : heure actuelle, ou date et heure choisies.

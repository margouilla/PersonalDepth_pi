# PersonalDepth_pi

**Personal high-resolution bathymetry for OpenCPN — bathymétrie personnelle haute résolution pour OpenCPN**

[🇬🇧 English](#english) · [🇫🇷 Français](#français)

> ⚠️ Beta software — not for navigation. / Logiciel en test — ne pas utiliser pour la navigation.

---

## English

PersonalDepth is an OpenCPN plugin (OpenCPN ≥ 5.14) inspired by
TimeZero's PBG module. Large personal surveys (XYZ point clouds, grids, TIN)
are imported **once** into a local database. The colour ramp, depth
thresholds, safety depth and hillshading can then be changed **instantly**,
without regenerating any file. The soundings displayed are the **exact
values** of the survey.

### Key idea: keep the data separate from the colours

```
file ──(thread)──► quantised cloud ──► tiled SQLite database ──► merged surface (by priority)
XYZ/ASC/TIN          12 B/sounding      Web Mercator tiles 256²       │
                                         ~0.9 B per sounding           ▼
palette / thresholds / safety ──► 16,384-entry LUT ──► GPU shader colours each pixel
```

Changing a colour or a threshold only rebuilds the lookup table; the depth
grids are never recomputed.

### Features

| | |
|---|---|
| **Formats** | XYZ text (any separator, headers skipped), ESRI ASCII `.asc`, LandXML TIN; GeoTIFF/BAG/NetCDF when built with GDAL. User NODATA values and Z bounds at import |
| **Survey database** | SQLite files split into geographic cells (0.25° by default, re-splittable); per-pixel shoalest sounding kept to the millimetre; overlapping surveys merged pixel by pixel by priority (best resolution then newest, shoalest, newest, or manual rank — no averaging); classes, multi-selection, clean-up by depth range; import can be stopped safely (SQLite transaction rolled back); already-imported files detected (import only new ones or replace) |
| **Compression** | JPEG-LS (MED) predictor + zig-zag + deflate, unit detection; ~0.9 byte per sounding at 50 cm resolution; tile encoding spread over all CPU cores |
| **Coordinates** | 300 built-in systems (WGS84, all UTM zones, Lambert-93, CC42–CC50, NTF Lambert, French overseas, many European/American/Oceanian systems, all NAD83 State Plane zones in m / US ft, polar, Web Mercator), EPSG code or PROJ string, `.prj` file; custom text formats (column choice, header, separators, date and time); projections coded in the plugin, checked against proj4js to < 1 cm |
| **Vertical datum** | Conversion to chart datum (ZH) on import: IGN69 / IGN78C (Corsica) → ZH with Shom BathyElli v2.1 + IGN RAF20 / RAC23, ellipsoidal heights → ZH, or any reference surface + geoid / difference grid |
| **Tide** | OpenCPN tide stations, or gridded **FES2022** tide (worldwide atlas converted by `tools/pdtide_convert.py`, Schureman nodal corrections matching pyfes within ~1 cm, inferred minor constituents, chart datum from BathyElli or lowest astronomical tide over 19 years). Displayed depth = charted depth + tide height, pixel by pixel |
| **Palette** | Continuous or stepped, editable table, presets, GMT `.cpt` import/export, automatic mode (N rounded thresholds over all data or the current view, optional fixed min/max), safety depth overlay |
| **Grid or point cloud** | Each survey is analysed at import: regular grids (even projected with rounding noise, or written in degrees) keep their cell; point clouds (sounding lines, scattered or chart soundings, multibeam, lidar) are **interpolated** by triangulation (TIN) up to a maximum distance worked out from the survey. Measured cells always win; interpolated areas can be hidden or lightened. Survey type in simple words (single-beam, multibeam, lidar, chart soundings, lead line) |
| **Observed water levels** | Time-stamped soundings corrected with a staff gauge or tide gauge series (text files, one file or a folder), with unit, convention, time zone and a curve window |
| **Large files** | Text files over 200 MB are read and imported in blocks (memory around 300 MB whatever the file size) |
| **Palette banner** | On-chart banner, draggable (position can be locked), horizontal or vertical: min / max depth (click to set), **A**/**M** automatic or manual, **View**/**All** source, colour bar (click for presets and your own palettes), gear to open the panel, current tide height |
| **Units** | Survey files can be imported in metres or feet; the database always stores metres. Soundings, isobaths, legend, palette thresholds, safety depth and tide heights follow OpenCPN's units (*Options › Display › Units*: metres or feet — fathoms are shown in feet) |
| **Survey metadata** | Survey date set at import (or read from the file name) and editable afterwards; vertical reference, coordinate system and Z unit of an imported source can be changed (*Edit…*: the original files are re-read with the new settings, which are stored per source) |
| **Tide curve** | Separate window plotting height versus time for every tide point used (OpenCPN station, or up to 9 FES2022 points spread over the data), legend with checkboxes, automatic bounds (one tide cycle around the chosen time) or user-defined, marker at the current or chosen time |
| **Language** | French, English or Russian, following OpenCPN's language (*Options › User › General options › Language*: French → French, Russian → Russian, any other language → English, unless a `PersonalDepth_<code>.po` translation file exists; see the user guide) |
| **Display** | Hillshading (adjustable, vertical exaggeration), GLSL smoothing, isobaths (marching squares, cached per zoom level, labels, safety contour), decluttered exact soundings, cursor read-out with source survey |

### Installation (testers)

Requirements: **OpenCPN 5.14 or newer**, OpenGL enabled. Pick the file for
your system in [Releases](https://github.com/margouilla/PersonalDepth_pi/releases):

| System | File `PersonalDepth-<version>_…` |
|---|---|
| Windows (OpenCPN 32-bit) | `msvc-wx32-10.tar.gz` |
| macOS 11 or newer, Intel and Apple Silicon | `darwin-wx32-10-universal.tar.gz` |
| Debian 12, Ubuntu 24.04 (OpenCPN PPA) — 64-bit PC | `debian-x86_64-12-x86_64.tar.gz` |
| Debian 13 — 64-bit PC | `debian-x86_64-13-x86_64.tar.gz` |
| Raspberry Pi OS 64-bit bookworm, Debian 12 ARM 64-bit | `debian-arm64-12-arm64.tar.gz` |
| Raspberry Pi OS 64-bit trixie, Debian 13 ARM 64-bit | `debian-arm64-13-arm64.tar.gz` |
| Raspberry Pi OS 32-bit bookworm, Debian 12 armhf | `debian-armhf-12-armhf.tar.gz` |

The Linux and macOS packages are new (beta.4) and **not tested on real
systems yet**: feedback welcome. Flatpak OpenCPN (Flathub) is not supported
yet.

1. Download the file for your system.
2. OpenCPN: *Options › Plugins › Import plugin…*, select the file.
3. Enable **PersonalDepth**, then click its toolbar icon.

**Illustrated user guide: [docs/user-guide.md](docs/user-guide.md)** (with what to check for testers).
What's new in each version: [CHANGELOG.md](CHANGELOG.md).

The user interface is in **English or French**, following the language
chosen in OpenCPN.

### Sample data

Ready-to-use database files for the Iroise Sea (Litto3D® Finistère 2014, 1 m,
depths below chart datum) to try the rendering without importing anything:
[data-litto3d-finistere-samples](https://github.com/margouilla/PersonalDepth_pi/releases/tag/data-litto3d-finistere-samples)
(Licence Ouverte 2.0 — see the attached `README_DATA.md`).

### Quick start

1. In the PersonalDepth panel, choose a **database folder**.
2. *Data management…* › *Import…*: pick the files, columns, Z sign and unit,
   survey date, coordinate system and vertical reference. *Edit…* changes
   these settings later for the selected sources.
3. Adjust palette, thresholds, safety depth and hillshading: changes are
   applied immediately.
4. Optional: *Tide* section for the current (or chosen) water height;
   *Tide curve…* opens the time / height chart.

### External data (not included, because of their licences)

- **FES2022** tide atlas (AVISO): to be downloaded and converted with
  `tools/pdtide_convert.py` (Python + netCDF4).
- **BathyElli v2.1** (Shom), **RAF20 / RAC23** (IGN): needed only for the
  French vertical datum conversions.

The plugin also works without them: OpenCPN tide stations, and data already
referenced to chart datum.
**Where to get them and how to use them: [docs/external-data.md](docs/external-data.md).**

### Building from source

Core library and tests (any platform):
```
cmake -S . -B build -DPD_BUILD_PLUGIN=OFF
cmake --build build && ctest --test-dir build
```

Plugin: CMake ≥ 3.16, C++17 compiler, **the same wxWidgets version as
OpenCPN** (3.2), OpenGL, GDAL optional. The OpenCPN API header is fetched
from `opencpn-libs/api-21`.

Windows (validated: OpenCPN 5.14 32-bit, Visual Studio 2026, wxWidgets
3.2.6 prebuilt): `powershell -ExecutionPolicy Bypass -File build.ps1`
(`-Clean`, `-NoInstall`, `-Package` builds the importable `.tar.gz` in
`dist\`).

Linux / macOS:
```
sudo apt install build-essential cmake libwxgtk3.2-dev libgl-dev libgdal-dev
cmake -S . -B build && cmake --build build -j
cmake --build build --target pd-package   # dist/PersonalDepth-<version>_<target>.tar.gz
```
The package target defaults to the build system (`/etc/os-release`); set it
with `-DPD_TARGET=… -DPD_TARGET_VERSION=… -DPD_TARGET_ARCH=…` and the
pre-release suffix with `-DPD_PRE=beta.N`.

Continuous integration (`.github/workflows/build.yml`, GitHub Actions):
every push builds and tests the Linux (Debian 12/13, x86_64, arm64, armhf)
and macOS (universal) packages; a `v*` tag also creates a draft release with
all of them. The Windows package is still built locally with `build.ps1`.

Translations: every English text is in `src/core/LangTable.inc`, one line
`{"French text", "English text"}` per text, looked up at run time — a
missing line simply leaves the French text.

### Known limitations

- Validated on Windows, OpenCPN 5.14 32-bit, OpenGL mode; Linux, Raspberry Pi
  and macOS builds still to be tested; the non-GL renderer is written but
  little tested.
- Raspberry Pi: large imports are slow and limited by memory (32-bit systems
  in particular).
- Fixed-function OpenGL: not compatible with Android / GLES.
- Mercator screen projection assumed; antimeridian not handled.
- Older datums (NTF, ED50, OSGB36…) use mean Helmert parameters (~1–2 m).

### Roadmap

Other user interface languages, Flatpak build and OpenCPN plugin catalog
(Cloudsmith), live echo-sounder input, export of the merged
surface.

---

## Français

PersonalDepth est un plugin OpenCPN (OpenCPN ≥ 5.14) inspiré du
module PBG de TimeZero. De gros levés personnels (nuages XYZ, grilles, TIN)
sont importés **une seule fois** dans une base locale ; la palette, les seuils
de profondeur, la zone dangereuse et l'ombrage se règlent ensuite **en
direct**, sans jamais régénérer de fichier. Les sondes affichées sont les
**valeurs exactes** du levé.

### Principe : séparer la donnée de la couleur

```
fichier ──(thread)──► nuage quantifié ──► base SQLite en tuiles ──► surface fusionnée (priorités)
XYZ/ASC/TIN           12 o/sonde           tuiles Web Mercator 256²      │
                                            ~0,9 o par sonde              ▼
palette / seuils / sécurité ──► LUT 16 384 entrées ──► shader GPU, couleur pixel par pixel
```

Changer une couleur ou un seuil ne reconstruit que la table de couleurs ; les
grilles de profondeur ne sont jamais recalculées.

### Fonctionnalités

| Fonction | Détail |
|---|---|
| Formats | XYZ texte (espace, tabulation, `,` ou `;`, entêtes ignorés), ESRI ASCII `.asc`, LandXML TIN (`<P>`/`<F>`), GeoTIFF/BAG/NetCDF si compilé avec GDAL. Valeurs NODATA supplémentaires et bornes Z1 – Z2 à l'import |
| Base de bathymétrie | Fichiers SQLite découpés en cellules géographiques (0,25° par défaut : `PD_N4815W00430.pdb` ; 1°, 0,5° ou 0,1° au choix, redécoupage possible ; anciennes bases en 1° lisibles), tuiles Web Mercator 256 × 256 (niveau natif d'après la résolution). Chaque pixel garde la plus faible sonde de la source (au mm) ; la surface fusionnée garde la valeur de la source prioritaire (pas de moyenne) : meilleure résolution puis plus récente (défaut), sonde la plus faible, plus récente, rang manuel. Classes, sélection multiple, nettoyage par bornes de profondeur, seules les tuiles touchées sont refusionnées. Import interruptible (transaction annulée pour le fichier en cours), fichiers déjà importés détectés (seulement les nouveaux, ou remplacement) |
| Compression | Prédicteur JPEG-LS (MED) + zigzag + deflate, détection de l'unité ; ~0,9 octet par sonde pour un levé à 50 cm ; encodage réparti sur tous les cœurs |
| Coordonnées | 300 systèmes intégrés (WGS84, UTM tous fuseaux, Lambert-93, CC42–CC50, Lambert NTF, outre-mer, Europe — RD, BNG, ITM, LV95, Gauss-Krüger…, Amériques dont toutes les zones NAD83 State Plane en m / pieds US, Océanie, polaires, Web Mercator), code EPSG ou chaîne PROJ, `.prj` voisin ; formats texte personnalisés (choix des colonnes, entête, séparateurs, date et heure). Projections codées dans le plugin, validées contre proj4js à < 1 cm. Conversion des grilles projetées : exacte un nœud sur 32, interpolée entre les deux (< 1 mm) |
| Référence verticale | Conversion vers le ZH à l'import : IGN69 / IGN78C (Corse) → ZH par BathyElli v2.1 (Shom) + RAF20 / RAC23 (IGN), hauteurs ellipsoïdales → ZH, ou surface de référence + géoïde / grille d'écart au choix |
| Marée | Stations de marée d'OpenCPN (hauteur unique, « maintenant » ou date choisie, curseur ±12 h), ou **FES2022 maillé** : atlas mondial converti (`tools/pdtide_convert.py`, 34 ondes), prédiction Schureman identique à pyfes (~1 cm), ondes mineures déduites ; Z0 = grille fournie, sinon BathyElli NM/ell − ZH/ell, sinon plus basse mer astronomique FES sur 19 ans. Profondeur affichée = sonde ZH + hauteur d'eau, pixel par pixel (shader), aussi pour les isobathes et les sondes |
| Unités | Fichiers de levé importés en mètres ou en pieds ; la base est toujours en mètres. Sondes, isobathes, légende, seuils de la palette, zone dangereuse et hauteurs de marée suivent les unités d'OpenCPN (*Options › Affichage › Unités* : mètres ou pieds — les brasses sont affichées en pieds) |
| Palette | Dégradé continu ou paliers ; seuils et couleurs éditables ; 5 préréglages ; import/export GMT `.cpt`. Mode automatique : N seuils (valeurs rondes ou répartis) entre le min et le max de toutes les données ou de la zone affichée, bornes min/max imposables |
| Grille ou nuage de points | Chaque levé est analysé à l'import : les grilles régulières (même projetées avec un bruit d'arrondi, ou écrites en degrés) gardent leur maille ; les nuages (lignes de sonde, sondes éparses ou de carte, multifaisceaux, lidar) sont **interpolés** par triangulation (TIN) jusqu'à une distance maximale déduite du levé. Les cases mesurées l'emportent toujours ; les zones interpolées peuvent être masquées ou éclaircies. Type de levé en termes simples (monofaisceau, multifaisceaux, lidar, sondes de carte, plomb) |
| Hauteurs d'eau observées | Sondes horodatées corrigées par une série d'échelle ou de marégraphe (fichiers texte, un fichier ou un dossier), avec unité, convention, fuseau horaire et fenêtre de courbe |
| Gros fichiers | Fichiers texte de plus de 200 Mo lus et importés par blocs (mémoire vers 300 Mo quelle que soit la taille) |
| Bandeau de palette | Bandeau sur la carte, déplaçable (position verrouillable), horizontal ou vertical : profondeurs min / max (clic pour les saisir), **A**/**M** automatique ou manuel, **Vue**/**Tout**, rampe de couleurs (clic : préréglages et vos palettes), roue pour ouvrir le panneau, hauteur de marée du moment |
| Sécurité | Zone « prof. < X m » en surcouche (tirant d'eau + pied de pilote), isobathe de sécurité rouge |
| Relief | Ombrage (Horn) réglable en direct, exagération verticale, adoucissement de la seule surface d'ombrage |
| Isobathes | Marching squares sur la grille, en cache par niveau de zoom : intervalle régulier ou seuils de la palette, isobathe maîtresse, valeurs sur les lignes, lissage. Aucune ligne inventée dans les trous du levé |
| Sondes chiffrées | Plus faible sonde par case d'écran, décimale en indice, valeurs découvrantes soulignées, troncature hydrographique, désencombrement ; lecture curseur avec la source du levé |
| Métadonnées des levés | Date du levé imposée à l'import (sinon lue dans le nom du fichier) et modifiable ensuite ; référence verticale, système de coordonnées et unité de Z d'une source importée modifiables (*Modifier…* : les fichiers d'origine sont relus avec les nouveaux réglages, enregistrés par source) |
| Courbe de marée | Fenêtre indépendante : hauteur d'eau en fonction du temps pour chaque point de marée utilisé (station OpenCPN, ou jusqu'à 9 points FES2022 répartis sur les données), légende à cocher, bornes automatiques (un cycle de marée autour de l'instant choisi) ou saisies, marqueur de l'instant (direct ou date choisie) |
| Langue | Français, anglais ou russe, selon la langue d'OpenCPN (*Options › Utilisateur › Options générales › Langue* : français → français, russe → russe, toute autre langue → anglais, sauf fichier de traduction `PersonalDepth_<code>.po` ; voir le guide) |
| Rendu | OpenGL (tuiles texturées, shader GLSL) et mode non-GL (wxDC) |

### Installation (testeurs)

Configuration requise : **OpenCPN 5.14 ou plus récent**, OpenGL activé.
Choisir le fichier de son système dans
[Releases](https://github.com/margouilla/PersonalDepth_pi/releases) :

| Système | Fichier `PersonalDepth-<version>_…` |
|---|---|
| Windows (OpenCPN 32 bits) | `msvc-wx32-10.tar.gz` |
| macOS 11 ou plus récent, Intel et Apple Silicon | `darwin-wx32-10-universal.tar.gz` |
| Debian 12, Ubuntu 24.04 (PPA OpenCPN) — PC 64 bits | `debian-x86_64-12-x86_64.tar.gz` |
| Debian 13 — PC 64 bits | `debian-x86_64-13-x86_64.tar.gz` |
| Raspberry Pi OS 64 bits bookworm, Debian 12 ARM 64 bits | `debian-arm64-12-arm64.tar.gz` |
| Raspberry Pi OS 64 bits trixie, Debian 13 ARM 64 bits | `debian-arm64-13-arm64.tar.gz` |
| Raspberry Pi OS 32 bits bookworm, Debian 12 armhf | `debian-armhf-12-armhf.tar.gz` |

Les paquets Linux et macOS sont nouveaux (beta.4) et **pas encore testés sur
de vrais systèmes** : retours bienvenus. OpenCPN en Flatpak (Flathub) n'est
pas encore pris en charge.

1. Télécharger le fichier de son système.
2. OpenCPN : *Options › Plugins › Importer un plugin…*, choisir le fichier.
3. Activer **PersonalDepth**, puis cliquer sur son icône dans la barre d'outils.

**Mode d'emploi illustré : [docs/guide-utilisateur.md](docs/guide-utilisateur.md)** (avec les points à vérifier pour les testeurs).
Nouveautés de chaque version : [CHANGELOG.md](CHANGELOG.md#français).

L'interface est en **français ou en anglais**, selon la langue choisie dans
OpenCPN.

### Données d'exemple

Fichiers de base prêts à l'emploi pour la mer d'Iroise (Litto3D® Finistère 2014,
1 m, profondeurs au zéro hydrographique), pour essayer le rendu sans rien
importer :
[data-litto3d-finistere-samples](https://github.com/margouilla/PersonalDepth_pi/releases/tag/data-litto3d-finistere-samples)
(Licence Ouverte 2.0 — voir le `README_DATA.md` joint).

### Utilisation

1. Panneau PersonalDepth, cadre *Base de bathymétrie* : choisir le dossier de
   la base. La liste montre les fichiers `.pdb` avec leur taille et leur nombre
   de sources ; la case de chaque fichier affiche ou masque sa zone (*Tout* /
   *Aucun*, *Centrer*). *Arrêter* interrompt l'opération en cours.
2. *Gestion des données…* :
   - *Importer…* : fichiers (ou dossier entier), colonnes, sens et unité de Z
     (mètres ou pieds), NODATA, bornes Z1 – Z2, date du levé, système de
     coordonnées, référence verticale et transformation vers le ZH, classe ;
   - *Modifier…* (ou double-clic) : date du levé, classe, et réglages de
     lecture des sources sélectionnées (unité, système, référence verticale :
     relecture des fichiers d'origine) ;
   - liste des sources par classe (date du levé, résolution, sondes, rang,
     Z min / max, référence verticale, date d'import) et détail de la
     sélection ;
   - priorité dans les recouvrements, rang (▲ ▼), classe, *Retirer*,
     *Nettoyer…*, *Centrer*, *Redécouper…*.
3. Palette, seuils, zone dangereuse, opacité, ombrage, sondes : tout
   s'applique immédiatement.
4. Section *Marée* : « Hauteur d'eau actuelle » ou « Date et heure
   choisies », stations OpenCPN ou FES2022 maillé ; *Courbe de marée…* ouvre
   le graphique temps / hauteur.

### Données externes (non fournies, en raison de leurs licences)

- Atlas de marée **FES2022** (AVISO) : à télécharger puis convertir avec
  `tools/pdtide_convert.py` (Python + netCDF4).
- **BathyElli v2.1** (Shom), **RAF20 / RAC23** (IGN) : utiles seulement pour
  les conversions de référence verticale en France.

Le plugin fonctionne sans elles : stations de marée d'OpenCPN, et données déjà
au zéro hydrographique.
**Où les récupérer et comment les utiliser : [docs/external-data.md](docs/external-data.md#français).**

### Compilation

Cœur et tests seuls (toute plateforme) :
```
cmake -S . -B build -DPD_BUILD_PLUGIN=OFF
cmake --build build && ctest --test-dir build
```

Plugin : CMake ≥ 3.16, compilateur C++17, **la même version de wxWidgets
qu'OpenCPN** (3.2), OpenGL, GDAL facultatif. L'en-tête d'API est récupéré
depuis `opencpn-libs/api-21` (ou `-DOCPN_API_DIR=…`).

Windows — procédure validée (OpenCPN 5.14 32 bits, Visual Studio 2026,
wxWidgets 3.2.6 précompilé : `wxWidgets-3.2.6-headers.7z`,
`wxMSW-3.2.6_vc14x_ReleaseDLL.7z`, `wxMSW-3.2.6_vc14x_Dev.7z` extraits dans
`..\deps\wxWidgets-3.2.6`) :
```
powershell -ExecutionPolicy Bypass -File build.ps1            # compile, teste, installe
powershell -ExecutionPolicy Bypass -File build.ps1 -Package   # + dist\*.tar.gz à importer
```
Options : `-Clean` (après une modification de `CMakeLists.txt`), `-NoInstall`
(OpenCPN ouvert), `-Pre beta.N` (suffixe de version du paquet).

Remarques Windows :
- OpenCPN 5.14 livre une `msvcp140.dll` de 2017 : la compilation définit
  `_DISABLE_CONSTEXPR_MUTEX_CONSTRUCTOR`, sans quoi un `std::mutex` compilé
  avec Visual Studio 2022 17.10+ plante au premier verrouillage.
- Installation manuelle dans `%LOCALAPPDATA%\opencpn\plugins\` (pas dans
  `Program Files (x86)\OpenCPN\plugins`) ; journal :
  `C:\ProgramData\opencpn\opencpn.log`.
- `small`/`near`/`far` sont des macros de `windows.h`.

Linux / macOS :
```
sudo apt install build-essential cmake libwxgtk3.2-dev libgl-dev libgdal-dev
cmake -S . -B build && cmake --build build -j
cmake --build build --target pd-package   # dist/PersonalDepth-<version>_<cible>.tar.gz
```
La cible du paquet est par défaut le système de compilation
(`/etc/os-release`) ; la préciser avec `-DPD_TARGET=… -DPD_TARGET_VERSION=…
-DPD_TARGET_ARCH=…`, et le suffixe de pré-version avec `-DPD_PRE=beta.N`.

Compilation automatique (`.github/workflows/build.yml`, GitHub Actions) :
chaque push compile et teste les paquets Linux (Debian 12/13, x86_64, arm64,
armhf) et macOS (universel) ; un tag `v*` crée en plus un brouillon de release
avec tous les paquets. Le paquet Windows reste compilé localement par
`build.ps1`.

Traductions : tous les textes anglais sont dans `src/core/LangTable.inc`, une
ligne `{"texte français", "English text"}` par texte, cherchée à l'exécution —
une ligne absente laisse simplement le texte français.

### Arborescence

```
src/core/          cœur C++17 sans wxWidgets (testable seul)
  PointCloud       lecteurs XYZ/ASC/LandXML/GDAL, nuage quantifié, cache .pdpc
  BathyGrid        maillage, TIN, pyramide, ombrage
  BathyDb          base SQLite : import, fusion, priorités, vue, redécoupage
  TileCodec        compression des tuiles de profondeur
  WebMerc          tuiles Web Mercator
  ColorRamp        rampe, préréglages, CPT, LUT, palette automatique
  Contours         isobathes (marching squares, Chaikin)
  Soundings        index spatial, sélection des sondes
  Projection       systèmes de coordonnées, conversions vers WGS84
  VerticalRef      grilles IGN .tac/.mnt, .asc, .gtx, BathyElli ; conversion vers le ZH
  TideFES          atlas .pdtide, arguments astronomiques, plus basse mer
  TideField        hauteur d'eau (uniforme ou maillée)
  BathyLayer       chargement asynchrone
  Inflate          décompression zlib sans dépendance
  Triangulate      enveloppe de delaunator
src/PersonalDepth_pi.*   classe plugin, configuration, tâches de fond
src/BathyRenderer.*      rendu GL / DC, sondes, bandeau de palette
src/BathyDialog.*        panneau principal
src/DataDialog.*         fenêtre « Gestion des données »
src/ImportDialog.*       fenêtre d'import / de modification des sources
src/TideChart.*          fenêtre « Courbe de marée »
src/core/Lang.*          langue de l'interface ; LangTable.inc : table français -> anglais
tools/pdtide_convert.py  conversion FES2022 NetCDF -> .pdtide
tools/ci/                compilation Linux en conteneur (GitHub Actions)
cmake/                   paquet OpenCPN (PdPackage), correctif wxWidgets macOS
packaging/               modèle de metadata.xml
.github/workflows/       compilation automatique Linux / Raspberry Pi / macOS
tests/test_core.cpp      tests du cœur
third_party/             SQLite (domaine public), miniz (MIT), delaunator (MIT)
```

### Limites connues

- Validé sous Windows avec OpenCPN 5.14 (32 bits), en mode OpenGL ; versions
  Linux, Raspberry Pi et macOS à tester ; le rendu non-OpenGL (wxDC) est
  écrit mais peu testé.
- Raspberry Pi : gros imports lents et limités par la mémoire (surtout en
  32 bits).
- Rendu GL en pipeline fixe : pas compatible Android / GLES.
- Projection écran supposée Mercator ; antiméridien non géré.
- Datums anciens (NTF, ED50, OSGB36, DHDN, BD72…) : paramètres de Helmert
  moyens, ~1–2 m d'écart. LAEA (EPSG:3035) non pris en charge.
- Bandeau de palette et lecture curseur en ASCII en mode OpenGL.
- Les noms de classe et libellés de référence verticale enregistrés dans la
  base restent dans la langue où ils ont été créés.

### Pistes suivantes

Autres langues d'interface, paquet Flatpak et catalogue de plugins OpenCPN
(Cloudsmith), intégration d'un sondeur en direct, export de
la surface fusionnée.

---

## Author / Auteur

**Marcaurelio Franzetti** — [github.com/margouilla](https://github.com/margouilla) —
marcaurelio.franzetti@gmail.com

Bug reports and feedback / retours et signalements :
[Issues](https://github.com/margouilla/PersonalDepth_pi/issues)

## Licence

GNU General Public License version 2 or (at your option) any later version —
see [`LICENSE`](LICENSE). / Licence GNU GPL version 2 ou (à votre choix) toute
version ultérieure.

Third-party code / code tiers : SQLite (public domain / domaine public),
miniz (MIT), `delaunator.hpp` (MIT, © 2018 Volodymyr Bilonenko).

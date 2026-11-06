# Changelog / Journal des modifications

[🇬🇧 English](#english) · [🇫🇷 Français](#français)

## English

### 0.1.0-beta.6 (2026-11-06)

- **Palette banner on the chart** replaces the legend: minimum depth,
  **A** / **M** button (automatic or manual palette), maximum depth, colour
  bar with the safety depth marked, and a gear that opens the panel. In
  automatic mode, a **View** / **All** button chooses between the displayed
  area and all data. Click a depth to type a palette bound, click the colour
  bar to pick a preset. Placed bottom centre (or top centre, bottom left),
  clear of the scale bar and of OpenCPN's edge panning. The cursor readout
  and the banner can be switched separately.
- **Custom text format** at import (*Columns › Custom format*, *Format…*):
  header lines, separator, decimal mark, choice of the X, Y, Z, date and
  time columns, coloured preview, automatic detection, formats saved in
  `.pdfmt` files. Optional date / time patterns (`yyyy/M/d h:m:s.s`,
  `MMM d, y`…) and degree-minute-second coordinate patterns (`D M S.S X`,
  `DDMM.MX`), with a help window. The survey date is taken from the date
  column.
- **File paths with any characters** (Cyrillic, accents…) on Windows.
- **NAD83 State Plane**: all US zones (metres, US survey feet, feet) in the
  list and by EPSG code; ESRI State Plane `.prj` files recognised.
- **Automatic** coordinate system: `.prj` file, otherwise degrees.
- **OpenCPN ENC (S-52)** palette, using OpenCPN's shallow / safety / deep
  contours.

### 0.1.0-beta.5

- **Sounding lines** (single-beam echo sounder, GPS tracks): the storage
  resolution is now estimated from the spacing of consecutive soundings
  (a few metres) instead of the covered area (often 50–100 m). Re-import the
  sources (*Edit…*) to benefit from it.
- **Fixed resolution** at import (*Edit…* for existing sources).
- Import window: scroll bar on small screens.
- English: spaces restored in the cursor readout and messages; file sizes
  in KB / MB / GB; FES2022 Z0 line translated.

### 0.1.0-beta.4

- **New platforms**: packages for macOS 11+ (Intel and Apple Silicon),
  Debian 12 / 13 and Ubuntu 24.04 (64-bit PC), Raspberry Pi OS 64-bit
  (bookworm, trixie) and 32-bit (bookworm), built automatically by GitHub
  Actions. Not yet tested on real systems: feedback welcome.
- macOS: GLSL shaders enabled (live palette, gridded tide).
- Default folder of the vertical references outside Windows: OpenCPN's
  private data folder.
- Illustrated user guide (English, French) with checks for testers, and
  GitHub forms to report a problem or send a test report.

### 0.1.0-beta.3

- **Language**: the user interface is now available in English as well as French.
  It follows OpenCPN's language (*Options › User › General options › Language*):
  French → French, any other language → English. Changing the language in
  OpenCPN rebuilds the plugin windows immediately.
- **Units**: survey files can be imported in metres or feet (the database
  always stores metres). Soundings, isobaths, legend, palette thresholds, safety
  depth and tide heights follow OpenCPN's units (*Options › Display › Units*:
  metres or feet; fathoms are shown in feet).
- **Survey metadata**: the survey date can be set at import and edited in the
  *Data management* list. *Edit…* also changes the Z unit, coordinate system
  and vertical reference of the selected sources: the original files are
  re-read with the new settings. Import settings are now stored per source.
- **Tide curve**: new *Tide curve…* window (height versus time) for every tide
  point used (OpenCPN station, or up to 9 FES2022 points spread over the data),
  legend with checkboxes, automatic bounds (one tide cycle around the chosen
  time) or user-defined, marker at the current or chosen time.

### 0.1.0-beta.2 / 0.1.0-beta.1

First public test versions (Windows, OpenCPN 5.14 32-bit). beta.2 fixes a
crash of OpenCPN when opening the plugin panel.

## Français

### 0.1.0-beta.6 (2026-11-06)

- **Bandeau de palette sur la carte** à la place de la légende : profondeur
  minimale, bouton **A** / **M** (palette automatique ou manuelle),
  profondeur maximale, rampe de couleurs avec la profondeur de sécurité
  repérée, et une roue qui ouvre le panneau. En automatique, un bouton
  **Vue** / **Tout** choisit entre la zone affichée et toutes les données.
  Clic sur une profondeur : saisie d'une borne de la palette ; clic sur les
  couleurs : choix d'un préréglage. Placé en bas au centre (ou en haut au
  centre, en bas à gauche), à l'écart de l'échelle et du défilement de bord
  d'OpenCPN. Lecture au curseur et bandeau s'activent séparément.
- **Format texte personnalisé** à l'import (*Colonnes › Format
  personnalisé*, *Format…*) : lignes d'entête, séparateur, séparateur
  décimal, choix des colonnes X, Y, Z, date et heure, aperçu en couleur,
  détection automatique, formats enregistrés en fichiers `.pdfmt`. Gabarits
  facultatifs de date / heure (`yyyy/M/d h:m:s.s`, `MMM d, y`…) et de
  coordonnées en degrés-minutes-secondes (`D M S.S X`, `DDMM.MX`), avec une
  fenêtre d'aide. La date du levé est prise dans la colonne de date.
- **Chemins de fichiers avec tous caractères** (cyrillique, accents…) sous
  Windows.
- **NAD83 State Plane** : toutes les zones des États-Unis (mètres, pieds US,
  pieds) dans la liste et par code EPSG ; fichiers `.prj` ESRI State Plane
  reconnus.
- Système de coordonnées **automatique** : fichier `.prj`, sinon degrés.
- Palette **ENC OpenCPN (S-52)**, avec les isobathes peu profonde / de
  sécurité / profonde d'OpenCPN.

### 0.1.0-beta.5

- **Lignes de sonde** (sondeur monofaisceau, traces GPS) : la résolution de
  stockage est maintenant estimée d'après l'espacement des sondes
  consécutives (quelques mètres) et non plus la surface couverte (souvent
  50 à 100 m). Réimporter les sources (*Modifier…*) pour en profiter.
- **Résolution imposée** à l'import (*Modifier…* pour les sources existantes).
- Fenêtre d'import : barre de défilement sur les petits écrans.
- Anglais : espaces rétablies dans la lecture curseur et les messages,
  tailles de fichiers en KB / MB / GB, ligne Z0 de FES2022 traduite.

### 0.1.0-beta.4

- **Nouvelles plateformes** : paquets pour macOS 11+ (Intel et Apple
  Silicon), Debian 12 / 13 et Ubuntu 24.04 (PC 64 bits), Raspberry Pi OS
  64 bits (bookworm, trixie) et 32 bits (bookworm), compilés automatiquement
  par GitHub Actions. Pas encore testés sur de vrais systèmes : retours
  bienvenus.
- macOS : shaders GLSL activés (palette en direct, marée maillée).
- Dossier par défaut des références verticales hors Windows : dossier de
  données privé d'OpenCPN.
- Mode d'emploi illustré (anglais, français) avec les points à vérifier par
  les testeurs, et formulaires GitHub pour signaler un problème ou envoyer un
  retour de test.

### 0.1.0-beta.3

- **Langue** : l'interface est maintenant disponible en anglais en plus du
  français. Elle suit la langue d'OpenCPN (*Options › Utilisateur › Options
  générales › Langue*) : français → français, toute autre langue → anglais. Un
  changement de langue dans OpenCPN reconstruit aussitôt les fenêtres du plugin.
- **Unités** : les fichiers de levé peuvent être importés en mètres ou en pieds
  (la base reste en mètres). Sondes, isobathes, légende, seuils de la palette,
  zone dangereuse et hauteurs de marée suivent les unités d'OpenCPN
  (*Options › Affichage › Unités* : mètres ou pieds ; les brasses sont
  affichées en pieds).
- **Métadonnées des levés** : date du levé imposable à l'import et modifiable
  dans la liste de *Gestion des données*. *Modifier…* change aussi l'unité de Z,
  le système de coordonnées et la référence verticale des sources
  sélectionnées : les fichiers d'origine sont relus avec les nouveaux réglages.
  Les réglages d'import sont désormais enregistrés par source.
- **Courbe de marée** : nouvelle fenêtre *Courbe de marée…* (hauteur en
  fonction du temps) pour chaque point de marée utilisé (station OpenCPN, ou
  jusqu'à 9 points FES2022 répartis sur les données), légende à cocher, bornes
  automatiques (un cycle de marée autour de l'instant choisi) ou saisies,
  marqueur de l'instant (direct ou date choisie).

### 0.1.0-beta.2 / 0.1.0-beta.1

Premières versions de test publiques (Windows, OpenCPN 5.14 32 bits). La beta.2
corrige un plantage d'OpenCPN à l'ouverture du panneau du plugin.

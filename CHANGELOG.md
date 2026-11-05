# Changelog / Journal des modifications

[🇬🇧 English](#english) · [🇫🇷 Français](#français)

## English

### 0.1.0-beta.5 (in preparation)

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

### 0.1.0-beta.5 (en préparation)

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

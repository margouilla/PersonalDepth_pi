# Changelog / Journal des modifications

[🇬🇧 English](#english) · [🇫🇷 Français](#français)

## English

### 0.1.0-beta.10 (unreleased)

- **Import window reorganised**: files and everyday settings always shown;
  coordinates, vertical reference, observed water levels and advanced
  options (offset, NODATA, Z bounds, fixed resolution, cache) in collapsible
  sections whose title sums up the current setting; rows that do not apply
  are hidden. No more scrolling on usual screens. Suggested by Baikal.
- *Survey type* and *Nature* merged into one *Data type* list (grid,
  single-beam, dense cloud — multibeam and lidar, which were handled the
  same —, chart soundings, lead line, other point cloud).
- Custom text format: the position of the first sounding is written out,
  with a warning when the latitude is polar (latitude / longitude columns
  probably swapped, as in Garmin Quickdraw exports).
- Continuous integration: the Windows package (OpenCPN 32 bits) is now built
  and tested by GitHub Actions like the others and attached to the release;
  tests also run on Raspberry Pi armhf (light version under emulation); a
  hang now fails the test after 20 minutes. New triangulation test: two real
  cases that froze the old version, and 20,000 random grids, part of them
  checked for the exact Delaunay property.

### 0.1.0-beta.9 (2026-11-12)

- **Interpolation simplified**: the surface between soundings is computed
  once, at the sounding step, and drawn the same at every scale. Before, line
  surveys were interpolated on a coarser grid and recombined at each zoom:
  surface changing with the zoom, streaks along the lines, large blocks at
  the edges, and new holes when the max distance was raised (130 → 150 m).
  Line surveys imported with an earlier version should be re-imported.
- Fixed: the triangulation could loop forever (bug in the C++ port of the
  delaunator library, hull walk), freezing the import; the *Stop* button then
  had no effect and OpenCPN had to be killed. Seen with Garmin Quickdraw
  data and line surveys.
- Natural neighbour: no more straight cut at tile edges (wider margin of
  soundings around each tile, working grid aligned on the tiles).
- Automatic max distance for line surveys: covers the large gaps between
  lines (95 % of them), not only the usual gap.
- IDW takes the nearest soundings in each quarter around the point: no more
  steps between two lines.
  Reports and test data by Baikal.
- Main window in **tabs** (Database, Tide, Palette, Contours and soundings,
  Display); the *Tide curve* button no longer hides under the tide text.
- User guides illustrated with screenshots and animations.
- **Translation files**: the Russian interface now comes from
  `data/lang/PersonalDepth_ru.po` (gettext, English → Russian, French source
  as a comment), editable without rebuilding; a corrected copy in the plugin's
  user folder (`lang` subfolder) takes priority and survives updates. Any
  other language works the same way with a `PersonalDepth_<code>.po` file.
  Entries whose `%d`, `%s`… differ from the English text are ignored.
- Removed the *White outline on values* option: contour values are always
  drawn as solid figures in the line colour.

### 0.1.0-beta.8 (2026-11-10)

- Fixed: soundings and colours of the database were shifted north or south of
  their true position, by up to half a cell (visible along a sounding track,
  especially with large cells). The view rows are now aligned on the database
  cells. Reported by Rasbats (issue #3).
- Fixed: OpenCPN crashed when the plugin was updated or uninstalled (its
  windows were deleted after the plugin was unloaded). The fix is in the code
  that unloads the plugin, so it applies from the next update: updating from
  beta.7 may still crash once (restart OpenCPN, the new version is installed).
- Dense single-beam line surveys (soundings a few decimetres apart along the
  track) are recognised as line surveys again: the gap between lines is now
  measured on a thinned sample. Before, D was only a few metres, so nothing
  was interpolated between lines, and the import was slow with a huge
  database (simulated 10-million-sounding survey: 23 s and 50 MB instead of
  4 min and 490 MB). Line surveys are stored at no finer than 1/100 of the
  line spacing.
- **Max distance** for the interpolation can be set by hand in the import
  window. The analysis of each survey (nature, spacing, D used, interpolated
  cells) is shown in *Data management*.
- Coordinates in **degrees-minutes-seconds with symbols and hemisphere
  letter** (`54° 00' 28.44" N`) are read without a pattern, and recognised by
  the automatic detection (N/S column → Y, E/W column → X).
- **Water levels for soundings without time**: the level of the survey day
  (survey date, or a date YYYYMMDD in the file name) is applied to the whole
  file.
- Option to draw the depth contour values **without the white outline**.
  Suggestions and reports by Baikal.
- **Russian** interface, chosen like French and English from OpenCPN's
  language.
- On-chart texts (banner, cursor readout, soundings) are drawn in full
  Unicode with OpenGL: accents, Cyrillic file names (before: "?????").
- **Single palette folder**: the original palettes are written there as
  `.cpt` files and can be edited or deleted like the user's own; *Restore
  original palettes* writes them back. Only *OpenCPN ENC (S-52)* stays apart.
- **Interpolation methods**: TIN (linear), natural neighbour (discrete
  Sibson) or IDW (inverse distance), chosen at import. The filled area is the
  same for all (limited by D); only the values change. By default: natural
  neighbour for a line survey, TIN otherwise.
- Faster import of large files: soundings are sorted by tile before being
  stored, and tiles leaving memory are written in groups (simulated survey
  of 10 million soundings: 21 s → 16 s, identical result).

### 0.1.0-beta.7 (2026-11-09)

- **Observed water levels** at import: each time-stamped sounding is
  corrected with the level of a staff gauge or tide gauge at its time (text
  files `date time, level`, one file or a folder), with the same format
  window as the soundings (columns, unit m / ft, convention, time zone) and
  a curve window.
- **Time zone** of the dates in the custom text format (soundings and water
  levels), converted to UTC.
- **Banner**: drag it anywhere on the chart; position can be locked;
  **horizontal or vertical** with a button on the banner (the vertical banner
  shows the intermediate thresholds).
- **My palettes**: palettes saved as `.cpt` files in a palette folder, offered
  in the preset list and in the banner menu (GMT palettes copied there too);
  `.cpt` files written by PersonalDepth keep the mode and danger zone.
- **Covered area** of the selected sources in *Data management*.
- **Grid or cloud** recognised at import: regular grids (even in projected
  coordinates with rounding noise, or written in degrees) keep their own
  cell; scattered soundings, survey lines and chart soundings are analysed
  (spacing between neighbours, gaps between lines). The nature can be forced
  and a **survey type** given in simple words (single-beam echo sounder,
  multibeam, lidar, chart soundings, lead line).
- **Interpolation at import** of point clouds (sounding lines, scattered
  soundings): triangulation (TIN) of the soundings, limited to a maximum
  distance worked out from the survey (gaps wider than that stay empty).
  Measured cells always win over interpolated ones, from any source.
  *Display* panel: show or hide the interpolated areas, and show them
  lighter or not. Soundings are never taken from interpolated cells; the
  cursor shows "(interpolated)".
- **Very large text files** (over 200 MB, tens of millions of soundings) are
  read and imported in blocks: memory stays around 300 MB instead of growing
  with the file (fixes "Insufficient memory" with the 32-bit Windows build).
- **Tide on the banner** shortened to the height and a short date and time
  (`04/11/26 12:32` in French, `04 Nov 26 12:32` in English), inside the
  banner in both orientations.
- Fixed: after choosing a palette from the banner, the chart followed the
  mouse as if the button were still pressed (Windows).
- Databases written by this version (interpolated areas) need beta.7 or
  newer; databases from earlier versions are read as before (re-import a
  survey to get its interpolation).

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

### 0.1.0-beta.10 (à venir)

- **Fenêtre d'import réorganisée** : fichiers et réglages courants toujours
  visibles ; coordonnées, référence verticale, hauteurs d'eau observées et
  options avancées (décalage, NODATA, bornes de Z, résolution imposée, cache)
  dans des sections repliables dont le titre résume le réglage ; lignes
  inutiles masquées. Plus de défilement sur un écran courant. Suggestion de
  Baikal.
- *Type de levé* et *Nature* réunis en une seule liste *Type de données*
  (grille, monofaisceau, nuage dense — multifaisceau et lidar, traités de la
  même façon —, sondes de carte, plomb, autre nuage de points).
- Format texte personnalisé : position de la première sonde écrite en
  clair, avec un avertissement si la latitude est polaire (colonnes latitude
  / longitude sans doute inversées, comme dans les exports Garmin
  Quickdraw).
- Intégration continue : le paquet Windows (OpenCPN 32 bits) est désormais
  compilé et testé par les Actions GitHub comme les autres, et joint à la
  release ; les tests tournent aussi sur Raspberry Pi armhf (version allégée
  sous émulation) ; un blocage fait échouer le test au bout de 20 minutes.
  Nouveau test de la triangulation : deux cas réels qui bloquaient
  l'ancienne version, et 20 000 grilles aléatoires dont une partie vérifiée
  pour la propriété de Delaunay exacte.

### 0.1.0-beta.9 (2026-11-12)

- **Interpolation simplifiée** : la surface entre les sondes est calculée
  une seule fois, au pas des sondes, et s'affiche à l'identique à toutes les
  échelles. Avant, les levés en lignes étaient interpolés sur une grille plus
  grossière puis recombinés à chaque zoom : surface qui changeait avec le
  zoom, stries le long des lignes, grands pavés en bordure, et nouveaux trous
  en augmentant la distance max (130 → 150 m). Les levés en lignes importés
  avec une version précédente sont à réimporter.
- Corrigé : la triangulation pouvait tourner sans fin (défaut du portage C++
  de la bibliothèque delaunator, parcours de l'enveloppe), figeant l'import ;
  le bouton *Arrêter* restait alors sans effet et il fallait tuer OpenCPN.
  Vu sur des données Garmin Quickdraw et des levés en lignes.
- Voisin naturel : plus de coupure droite au bord des tuiles (marge de sondes
  plus large autour de chaque tuile, grille de travail calée sur les tuiles).
- Distance max automatique des levés en lignes : couvre les grands écarts
  entre lignes (95 % d'entre eux), pas seulement l'écart habituel.
- IDW prend les sondes les plus proches dans chaque quart autour du point :
  plus de marches entre deux lignes.
  Signalements et données de test de Baikal.
- Fenêtre principale en **onglets** (Base, Marée, Palette, Isobathes et
  sondes, Affichage) ; le bouton *Courbe de marée* ne passe plus sous le
  texte de la marée.
- Guides illustrés de captures d'écran et d'animations.
- **Fichiers de traduction** : l'interface russe vient maintenant de
  `data/lang/PersonalDepth_ru.po` (gettext, anglais → russe, texte français en
  commentaire), modifiable sans recompiler ; une copie corrigée dans le
  dossier utilisateur du plugin (sous-dossier `lang`) l'emporte et survit aux
  mises à jour. Toute autre langue fonctionne de même avec un fichier
  `PersonalDepth_<code>.po`. Les entrées dont les `%d`, `%s`… diffèrent de
  l'anglais sont ignorées.
- Option *Contour blanc des valeurs* retirée : les valeurs des isobathes sont
  toujours en chiffres pleins, à la couleur des lignes.

### 0.1.0-beta.8 (2026-11-10)

- Corrigé : les sondes et les couleurs de la base étaient décalées vers le
  nord ou le sud de leur vraie position, jusqu'à une demi-cellule (visible le
  long d'une route de sonde, surtout avec de grandes cellules). Les lignes de
  la vue sont maintenant calées sur les cellules de la base. Signalé par
  Rasbats (issue #3).
- Corrigé : OpenCPN plantait à la mise à jour ou à la désinstallation du
  plugin (fenêtres supprimées après le déchargement du plugin). La correction
  est dans le code d'arrêt du plugin : elle vaut à partir de la mise à jour
  suivante ; passer de la beta.7 à la beta.8 peut encore planter une fois
  (relancer OpenCPN : la nouvelle version est installée).
- Les levés monofaisceaux en lignes très denses (sondes à quelques décimètres
  le long de la route) sont de nouveau reconnus comme levés en lignes :
  l'écart entre lignes est mesuré sur un échantillon éclairci. Avant, D ne
  faisait que quelques mètres : rien n'était interpolé entre les lignes, et
  l'import était lent avec une base énorme (levé simulé de 10 millions de
  sondes : 23 s et 50 Mo au lieu de 4 min et 490 Mo). Les levés en lignes
  sont stockés au plus fin à 1/100 de l'écart entre lignes.
- **Distance max** d'interpolation réglable à la main dans la fenêtre
  d'import. L'analyse de chaque levé (nature, espacement, D utilisée, cases
  interpolées) s'affiche dans *Gestion des données*.
- Coordonnées en **degrés-minutes-secondes avec symboles et lettre
  d'hémisphère** (`54° 00' 28.44" N`) lues sans gabarit, et reconnues par la
  détection automatique (colonne N/S → Y, colonne E/W → X).
- **Hauteurs d'eau pour des sondes sans heure** : la hauteur du jour du levé
  (date du levé, ou date AAAAMMJJ dans le nom du fichier) est appliquée à
  tout le fichier.
- Option pour dessiner les valeurs des isobathes **sans contour blanc**.
  Suggestions et signalements de Baikal.
- Interface en **russe**, choisie comme le français et l'anglais d'après la
  langue d'OpenCPN.
- Textes sur la carte (bandeau, lecture au curseur, sondes) dessinés en
  Unicode complet en OpenGL : accents, noms de fichiers en cyrillique (avant :
  « ????? »).
- **Dossier de palettes unique** : les palettes d'origine y sont écrites en
  `.cpt` et se modifient ou se suppriment comme celles de l'utilisateur ;
  *Restaurer les palettes d'origine* les réécrit. Seule *ENC OpenCPN (S-52)*
  reste à part.
- **Méthodes d'interpolation** : TIN (linéaire), voisin naturel (Sibson
  discret) ou IDW (inverse de la distance), choisies à l'import. La zone
  remplie est la même pour toutes (limitée par D) ; seules les valeurs
  changent. Par défaut : voisin naturel pour un levé en lignes, TIN sinon.
- Import des gros fichiers plus rapide : sondes triées par tuile avant d'être
  rangées, et tuiles sorties de la mémoire écrites par groupes (levé simulé de
  10 millions de sondes : 21 s → 16 s, résultat identique).

### 0.1.0-beta.7 (2026-11-09)

- **Hauteurs d'eau observées** à l'import : chaque sonde horodatée est
  corrigée de la hauteur d'une échelle ou d'un marégraphe à son heure
  (fichiers texte `date heure, hauteur`, un fichier ou un dossier), avec la
  même fenêtre de format que les sondes (colonnes, unité m / ft, convention,
  fuseau horaire) et une fenêtre de courbe.
- **Fuseau horaire** des dates dans le format texte personnalisé (sondes et
  hauteurs d'eau), ramenées en UTC.
- **Bandeau** : déplaçable à la souris n'importe où sur la carte ; position
  verrouillable ; **horizontal ou vertical** par un bouton sur le bandeau (le
  bandeau vertical montre les seuils intermédiaires).
- **Mes palettes** : palettes enregistrées en fichiers `.cpt` dans un dossier
  de palettes, proposées dans la liste des préréglages et dans le menu du
  bandeau (y compris les palettes GMT qui y sont copiées) ; les `.cpt` écrits
  par PersonalDepth gardent le mode et la zone dangereuse.
- **Surface couverte** des sources sélectionnées dans *Gestion des données*.
- **Grille ou nuage** reconnu à l'import : les grilles régulières (même en
  coordonnées projetées avec un bruit d'arrondi, ou écrites en degrés)
  gardent leur maille ; les sondes éparses, profils et sondes de carte sont
  analysés (espacement entre voisins, écart entre profils). La nature peut
  être forcée et un **type de levé** indiqué en termes simples (sondeur
  monofaisceau, multifaisceau, lidar, sondes de carte, plomb).
- **Interpolation à l'import** des nuages (profils, sondes éparses) :
  triangulation (TIN) des sondes, limitée à une distance maximale déduite du
  levé (les trous plus larges restent vides). Les cellules mesurées
  l'emportent toujours sur les interpolées, quelle que soit la source.
  Panneau *Affichage* : afficher ou masquer les zones interpolées, les
  montrer plus claires ou non. Les sondes ne sont jamais prises dans les
  cellules interpolées ; le curseur indique « (interpolé) ».
- **Très gros fichiers texte** (plus de 200 Mo, dizaines de millions de
  sondes) lus et importés par blocs : la mémoire reste vers 300 Mo au lieu de
  croître avec le fichier (corrige « Mémoire insuffisante » avec la version
  Windows 32 bits).
- **Marée sur le bandeau** réduite à la hauteur et à une date et heure
  courtes (`04/11/26 12:32` en français, `04 Nov 26 12:32` en anglais), dans
  le bandeau quelle que soit son orientation.
- Corrigé : après le choix d'une palette depuis le bandeau, la carte suivait
  la souris comme si le bouton restait enfoncé (Windows).
- Les bases écrites par cette version (zones interpolées) demandent la
  beta.7 ou plus récente ; les bases des versions précédentes sont lues comme
  avant (réimporter un levé pour obtenir son interpolation).

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

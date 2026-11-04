# PersonalDepth_pi — bathymétrie personnelle dynamique pour OpenCPN

Plugin OpenCPN (API 1.18, OpenCPN ≥ 5.8) qui reprend le principe du module PBG
de TimeZero : on charge une fois de gros levés (nuages XYZ, grilles, TIN), puis
on règle **en direct** la palette, les seuils de profondeur, la zone dangereuse
et l'ombrage, sans jamais régénérer de fichier. Les sondes affichées sont les
**valeurs exactes** du levé.

## Principe : séparer la donnée de la couleur

```
fichier ──(thread)──► nuage quantifié ──► grille « plus faible sonde » ──► pyramide
 XYZ/ASC/TIN/TIFF     12 o/sonde, cache     (+ TIN optionnel)               + ombrage
                         │                                                      │
                         └──► index spatial ──► sondes chiffrées exactes        │
                                                                                ▼
 palette / seuils / sécurité ──► LUT 16 384 entrées ──► recoloration des seules tuiles visibles
```

Changer une couleur ou un seuil ne touche que la LUT : les tuiles visibles sont
recolorées à la volée au rendu suivant (≈ 11 ms pour 2 M de cellules, mesuré).
La grille, l'index et le cache ne sont recalculés que si l'on change le
maillage (taille de cellule, mode TIN), comme un PBG qu'on reconstruirait.

## Fonctionnalités

| Fonction | Détail |
|---|---|
| Formats | XYZ texte (espace, tabulation, `,` ou `;`, entêtes ignorés), ESRI ASCII `.asc`, LandXML TIN (`<P>`/`<F>`), GeoTIFF/BAG/NetCDF si compilé avec GDAL |
| Volumes | 12 octets par sonde ; 14 M de sondes lues en 1,3 s, cache binaire `.pdpc` relu ~4× plus vite |
| Coordonnées | 69 systèmes intégrés (WGS84, UTM tous fuseaux, Lambert-93, CC42–CC50, Lambert NTF, outre-mer, Europe — RD, BNG, ITM, LV95, Gauss-Krüger…, Amériques, Océanie, polaires, Web Mercator), code EPSG ou chaîne PROJ libre, lecture du `.prj` voisin. Projections codées dans le plugin (Krüger ordre 6, LCC, stéréographiques, oblique suisse, Mercator, Helmert), validées contre proj4js à < 1 cm. Message d'aide si le système est oublié (« ressemble à du Lambert-93 ») |
| Référence verticale | Données au ZH (défaut) ; **IGN69 → ZH à la volée** (ex. Litto3D) par BathyElli v2.1 « ZH/ell » (Atlantique + Méditerranée, `.glhi`) combinée à RAF20 (`.tac`) : C = h_ZH − N_IGN69, profondeur_ZH = profondeur_IGN69 + C. Autres régions : surfaces ZH/ell + grille de géoïde au choix (`.tac`, `.asc`, `.gtx`, points XYZ), ou grille d'écart directe (option « inverser »). Correction appliquée au chargement, le cache reste brut ; points hors couverture signalés |
| Marée | Stations de marée d'OpenCPN (API 1.21) : station la plus proche du levé, hauteur d'eau « maintenant » (mise à jour chaque minute) ou à une date/heure choisie, curseur ±12 h par pas de 15 min. Profondeur affichée = sonde ZH + hauteur d'eau : palette, isobathes, sondes chiffrées, lecture curseur et légende suivent, sans recalcul des grilles. Avertissement si la station est à plus de 30 km |
| Base de bathymétrie | Sources importées une fois dans une base SQLite découpée automatiquement en cellules géographiques de 0,25° par défaut (`PD_N4815W00430.pdb`, coin sud-ouest en degrés et minutes ; 1°, 0,5° ou 0,1° au choix, redécoupage d'une base existante possible ; les anciennes bases en 1° `PD_N48W005.pdb` restent lisibles), en tuiles Web Mercator 256 × 256 (niveau natif choisi d'après la résolution : 0,5 m → niveau 18, 1 m → 17). Chaque pixel garde la plus faible sonde de la source (valeur exacte, au mm) ; la surface fusionnée garde, pixel par pixel, la valeur de la source prioritaire (pas de moyenne) et son numéro. Règles : meilleure résolution puis plus récente (défaut), sonde la plus faible, plus récente, rang manuel. Sources rangées par classe (déduite du nom de fichier : LITTO3D, PFE…, modifiable) dans un arbre à sélection multiple : cases pour afficher/masquer une source ou une classe, retrait et rang par blocs ; seules les tuiles touchées sont refusionnées. Affichage : une seule surface, une seule famille d'isobathes, source indiquée au curseur. Compression : prédicteur JPEG-LS + zigzag + deflate, ~0,9 octet par sonde pour un levé à 50 cm (levé PFE : 215 Mo de XYZ → 25 Mo de base avec 4 dalles Litto3D) |
| Marée FES2022 | Atlas mondial converti (`tools/pdtide_convert.py`, 231,6 Mo, 34 ondes au mm) ; prédiction Schureman identique à pyfes (écarts 0,3–3 cm RMS sans ondes mineures déduites) ; Z0 = grille fournie, sinon BathyElli NM/ell − ZH/ell, sinon plus basse mer FES sur 19 ans ; marée variable dans l'espace appliquée pixel par pixel (shader), aux isobathes et aux sondes |
| Convention Z | Automatique (majorité négative ⇒ altitudes), profondeurs positives, altitudes ; décalage vertical (changement de zéro) |
| Maillage | Plus faible sonde (défaut, sûr pour la navigation), moyenne, ou plus faible sonde + TIN de Delaunay qui comble les trous entre profils sans franchir les grands vides. Les XYZ déjà maillés sont détectés et gardent leur pas exact (« grille native », une cellule par sonde) ; pour les nuages irréguliers, les trous isolés d'une cellule sont comblés |
| Isobathes | Calculées à la volée sur la grille (marching squares), en cache par niveau de zoom : intervalle régulier ou seuils de la palette, isobathe maîtresse, isobathe de sécurité rouge liée à la zone dangereuse, valeurs sur les lignes, lissage graphique. Aucune ligne inventée dans les trous du levé |
| Palette | Dégradé continu ou paliers ; seuils et couleurs éditables dans un tableau ; 5 préréglages ; import/export palettes GMT `.cpt`. Mode automatique : N seuils (valeurs rondes ou répartis) entre le min et le max de toutes les données ou de la zone affichée (profondeurs du moment), bornes min/max imposables (ex. min 0 m : palette sur la seule bathymétrie), couleurs reprises du préréglage choisi |
| Sécurité | Zone « prof. < X m » mélangée en surcouche (tirant d'eau + pied de pilote) |
| Relief | Ombrage (Horn) avec intensité réglable en direct, exagération verticale, « Adoucir le relief » (lissage de la surface d'ombrage seulement, les profondeurs restent exactes) |
| Lissage | Profondeur et ombrage interpolés entre cellules par shader GLSL, palette appliquée pixel par pixel (paliers nets) ; repli automatique sans shader. Levés maillés en projection : grille d'affichage interpolée depuis les nœuds d'origine |
| Sondes chiffrées | Plus faible sonde par case d'écran, décimale en indice, valeurs découvrantes soulignées, troncature hydrographique, désencombrement (aucun chevauchement, priorité aux valeurs d'isobathes puis à la plus faible sonde) ; en dézoom, valeurs issues de la grille (grisées) |
| Lecture curseur | Sonde réelle la plus proche et sa distance, dans une étiquette qui suit le pointeur (passe d'interface, non mise en cache) et dans le panneau ; masquée pendant le glisser de la carte |
| Rendu | OpenGL (tuiles 512² texturées, maillées pour suivre la projection) et mode non-GL (wxDC, image mise en cache) ; dessiné sous les navires/routes si l'hôte le permet |
| Persistance | Couches, palette et réglages sauvés dans la config OpenCPN, couches rechargées en arrière-plan au démarrage |

## Arborescence

```
src/core/         cœur C++17 sans wxWidgets (testable seul)
  PointCloud      lecteurs XYZ/ASC/LandXML/GDAL, nuage quantifié, cache .pdpc
  BathyGrid       maillage, TIN, pyramide, ombrage
  ColorRamp       rampe, préréglages, CPT, LUT, colorisation
  Soundings       index spatial, projection écran, sélection des sondes
  BathyLayer      chargement asynchrone d'une couche
  Contours        isobathes (marching squares, Chaikin)
  Projection      systèmes de coordonnées, conversions vers WGS84
  VerticalRef     grilles .tac/.asc/.gtx/points, correction IGN69 → ZH
  Triangulate     enveloppe de delaunator (une seule unité de compilation)
  TideFES         atlas .pdtide, arguments astronomiques, plus basse mer
  TideField       hauteur d'eau (uniforme ou maillée) appliquée à l'affichage
  Inflate         décompression zlib sans dépendance
  WebMerc         tuiles Web Mercator
  TileCodec       compression des tuiles de profondeur
  BathyDb         base SQLite : import, fusion, priorités, vue
tools/pdtide_convert.py  conversion FES2022 NetCDF -> .pdtide
third_party/sqlite (domaine public), third_party/miniz (MIT)
src/PersonalDepth_pi.*   classe plugin, config, curseur
src/BathyRenderer.*      rendu GL / DC, sondes, légende
src/BathyDialog.*        panneau de contrôle
tests/test_core.cpp      tests du cœur
third_party/delaunator.hpp (MIT)
```

## Compilation

### Cœur et tests seuls (toute plateforme)
```
cmake -S . -B build -DPD_BUILD_PLUGIN=OFF
cmake --build build && ctest --test-dir build
```

### Plugin
Prérequis : CMake ≥ 3.16, compilateur C++17, **la même version de wxWidgets
qu'OpenCPN** (3.2 pour OpenCPN 5.8/5.10), OpenGL, GDAL facultatif.
L'en-tête d'API est récupéré automatiquement depuis `opencpn-libs/api-21`
(API marée 1.21 ; ou pointer `-DOCPN_API_DIR=/chemin/opencpn-libs/api-21`).

Linux :
```
sudo apt install build-essential cmake libwxgtk3.2-dev libgl-dev libgdal-dev
cmake -S . -B build && cmake --build build -j
sudo cmake --install build      # lib/opencpn + share/opencpn/plugins/PersonalDepth_pi
```
Windows (Visual Studio, wxWidgets 3.2 compilé comme OpenCPN) :
```
cmake -S . -B build -A Win32 -DwxWidgets_ROOT_DIR=C:/wxWidgets-3.2
cmake --build build --config Release
```
puis copier `PersonalDepth_pi.dll` dans `C:\Program Files (x86)\OpenCPN\plugins\`.
Pour OpenCPN 64 bits, utiliser `-A x64`.

macOS : même principe ; le lien utilise `-undefined dynamic_lookup`.

### Windows — procédure validée (OpenCPN 5.14 32 bits, VS Community 2026)
1. wxWidgets **3.2.6** précompilé (assets de la release v3.2.6 sur GitHub) :
   `wxWidgets-3.2.6-headers.7z`, `wxMSW-3.2.6_vc14x_ReleaseDLL.7z`,
   `wxMSW-3.2.6_vc14x_Dev.7z`, extraits dans un même dossier.
2. PowerShell : `Set-ExecutionPolicy -Scope Process Bypass`, puis
   `& "C:\Program Files\Microsoft Visual Studio\18\Community\Common7\Tools\Launch-VsDevShell.ps1" -Arch x86 -HostArch amd64`
3. `cmake -S . -B build -G "Visual Studio 18 2026" -A Win32 -DPD_WITH_GDAL=OFF "-DwxWidgets_ROOT_DIR=$WX" "-DwxWidgets_LIB_DIR=$WX\lib\vc14x_dll"`
   puis `cmake --build build --config Release` et `ctest --test-dir build -C Release`.
4. **Installer dans `%LOCALAPPDATA%\opencpn\plugins\`**, pas dans
   `Program Files (x86)\OpenCPN\plugins` (réservé aux plugins système, les
   autres y sont ignorés sans message).
5. **Runtime C++** : OpenCPN embarque `msvcp140.dll`/`vcruntime140.dll` 14.12
   (VS 2017), trop anciens pour une DLL compilée avec VS 2022/2026. Les renommer
   en `.old` dans le dossier d'OpenCPN pour utiliser ceux de `SysWOW64`
   (à refaire après une mise à jour d'OpenCPN).
6. **Activation** : un plugin non catalogué et désactivé est retiré de la liste
   d'OpenCPN 5.14. OpenCPN fermé, ajouter à `C:\ProgramData\opencpn\opencpn.ini` :
   ```
   [PlugIns/PersonalDepth_pi.dll]
   bEnabled=1
   ```
7. Pièges : `small`/`near`/`far` sont des macros de `windows.h` (ne pas les
   utiliser comme noms de variables) ; diagnostics dans
   `C:\ProgramData\opencpn\opencpn.log`.
8. Rendu OpenGL d'OpenCPN ≥ 5.8 : l'hôte dessine en GLSL et peut laisser un
   programme, un VBO ou une autre unité de texture actifs ; le plugin
   sauvegarde puis impose programme 0, unité 0, pas de VBO et sa propre
   projection en pixels (`GLHostState`). La passe `OVERLAY_CHARTS` est mise
   en cache avec les cartes : tout élément qui doit suivre la souris se
   dessine dans `OVERLAY_OVER_UI`.
9. Recompilation : `powershell -ExecutionPolicy Bypass -File build.ps1`
   (`-Clean` après une modification de `CMakeLists.txt`, `-NoInstall` si
   OpenCPN est ouvert). Le script copie aussi BathyElli (`*ZH*.glhi`) et RAF20
   (`RAF20*.tac`) depuis `..\ReferenceElevation` vers
   `%LOCALAPPDATA%\opencpn\plugins\PersonalDepth_pi\references`.

À faire pour la diffusion : paquet tarball + métadonnées pour *Plugins › Importer*
(supprime les étapes 5 et 6), ou lien statique du runtime (`/MT`).

Pour une diffusion par le catalogue OpenCPN (tarballs, métadonnées, CI),
reprendre ces sources dans le gabarit `shipdriver_pi`.

## Utilisation
1. Activer le plugin dans Options › Plugins, puis cliquer l'icône de la barre.
2. Panneau principal, cadre *Base de bathymétrie* : choisir le dossier de la
   base. La liste montre les fichiers `.pdb` (cellules géographiques) avec
   leur taille et leur nombre de sources ; la case de chaque fichier affiche
   ou masque sa zone (*Tout* / *Aucun*, *Centrer*). *Arrêter* interrompt
   l'opération en cours sur la base.
3. *Gestion des données…* ouvre la fenêtre de gestion :
   - *Importer…* : fichiers (ou dossier entier), colonnes, sens de Z, NODATA
     supplémentaires, bornes Z1 – Z2 (profondeur ZH finale), système de
     coordonnées, référence verticale et transformation vers le ZH, classe.
     Si des fichiers sont déjà dans la base (même chemin ou même nom), choix
     entre « seulement les nouveaux » et « tout réimporter » (l'ancienne
     version de chaque fichier est alors remplacée).
   - liste des sources par classe avec date du levé, résolution, sondes, rang,
     Z min / max, référence verticale, date d'import ; détail de la sélection
     (fichier d'origine, système, emprise, fichiers `.pdb` concernés) ;
   - priorité dans les recouvrements, rang (▲ ▼), classe, *Retirer*,
     *Nettoyer…* (sondes hors des bornes Z1 – Z2), *Centrer* ;
   - *Redécouper…* : taille des cellules des fichiers `.pdb`.
   L'arrêt d'un import garde les fichiers déjà terminés et annule proprement
   le fichier en cours (transaction SQLite annulée).
4. Modifier palette, seuils, zone dangereuse, opacité, ombrage, sondes :
   tout s'applique immédiatement.
5. Marée : section *Marée*, « Maintenant » ou « Date choisie », stations
   OpenCPN ou FES2022 maillé.

## Limites connues (v0.1)
- Validé sous Windows avec OpenCPN 5.14 (32 bits), en mode OpenGL. Le rendu
  non-OpenGL (wxDC) est écrit mais peu testé.
- Datums anciens (NTF, ED50, OSGB36, DHDN, BD72…) : paramètres de Helmert
  moyens, ~1–2 m d'écart avec les grilles officielles. LAEA (EPSG:3035) non
  pris en charge.
- Rendu GL en pipeline fixe (glBegin) : OK sur ordinateurs, pas sur Android/GLES.
- Projection écran supposée Mercator (projection par défaut d'OpenCPN) ;
  antiméridien non géré.
- Précision des seuils de couleur : (zmax − zmin) / 32 768, soit ~6 mm pour
  0–200 m. Les valeurs chiffrées, elles, sont exactes (float32).
- La légende et la lecture curseur sont en ASCII en mode OpenGL.
- Marée : une seule station (la plus proche du centre des couches) pour toute
  la vue ; pas encore de modèle harmonique maillé (FES2022). Corse : grille
  RAC non fournie (utiliser le mode « surfaces » avec une autre grille).

## Pistes suivantes
Paquet pour *Plugins › Importer*, marée FES2022 extraite par zone
(+ BathyElli NM/ell), grille RAC Corse, projection LAEA, export S-57/mbtiles,
fusion de levés avec priorité par date.

## Auteur
Marcaurelio Franzetti — [github.com/margouilla](https://github.com/margouilla) —
marcaurelio.franzetti@gmail.com

Retours de test et signalements : onglet *Issues* du dépôt
https://github.com/margouilla/PersonalDepth_pi

Licence : GNU GPL version 2 ou (à votre choix) toute version ultérieure — voir `LICENSE`.
SQLite : domaine public ; miniz : MIT.
`delaunator.hpp` : MIT, © 2018 Volodymyr Bilonenko.

# Changelog / Journal des modifications

[🇬🇧 English](#english) · [🇫🇷 Français](#français)

## English

### 0.1.0-beta.14 (2026-11-20)

- `.prj` files without a known EPSG code (ESRI WKT exported by QGIS, WKT2)
  are now read parameter by parameter: Mercator (1SP, 2SP, ESRI, Web
  Mercator), transverse Mercator (Gauss-Krüger), Lambert conformal conic and
  stereographic projections, ellipsoid, TOWGS84 (common datums such as
  Pulkovo 1942, ED50, OSGB36, NAD27, NTF when absent). Applies to own
  shorelines (shapefile) and to survey files with a `.prj`. Before, a
  "World Mercator" shoreline was not recognised. Reported by Baikal.
- Own shorelines drawn as **closed polylines** (first point = last point:
  lake and island outlines exported as lines by QGIS) are now taken as
  polygons, so they mask the land like polygons do. Before, only polygon
  shapefiles masked the land.
- A database cell file (`.pdb`, `.pdi`) whose last source is removed is now
  deleted, instead of staying in the list with 0 sources. Empty files left
  by earlier versions are deleted when the database is opened.
- Fixed: the summary of the automatic palette (*Display* tab) stayed in the
  previous language or unit after changing them in OpenCPN.
- **Palette banner**: the contours / soundings line is replaced by a single
  rings icon opening a menu: show contours, palette thresholds or interval
  (round values or typed), show soundings, settings. Major contour, decimals
  and text size stay in the *Display* tab. Suggested by Baikal.
- **Toolbar icon**: new *Icon* setting (*Display* tab): it opens the panel
  (as before) or shows / hides the whole plugin. The panel also opens from
  the chart's right-click menu (*PersonalDepth…*). Suggested by Baikal.
- **Imagery import settings**: background, pixel, equalisation and sonar
  offset are grouped under *Import settings*, with **Re-import selection**
  to apply them to images already in the database (same date and class).

### 0.1.0-beta.13 (2026-11-19)

- **Seabed imagery** (reflectivity, backscatter, side-scan sonar mosaics):
  *Imagery* box of the *Data* tab. GeoTIFF images (dB, 8/16-bit greyscale, palette) are
  imported into an imagery database (`PD_*.pdi` files, in the same folder as
  the bathymetry). This includes the mosaics distributed free of charge by
  Ifremer, Infomar or EMODnet. The original values are kept and read at the
  cursor (e.g. `Imagery -18.4 dB`). Each image is stretched for display
  between its 2 % and 98 % values, measured at import. The uniform
  background around a survey (white 255 or black 0) is detected and
  discarded automatically. Rendering in greyscale, copper or sepia, with
  black and white points, gamma and inversion, changed instantly without
  re-reading the files. *Bathymetry* and *Imagery* switches are separate.
  The two layers overlay, the user chooses which one is on top, and each
  has its own opacity. Contours and soundings always stay above. A 50
  million pixel mosaic imports in about 20 s. The display settings of both
  layers are together in the *Display* tab. The palette banner gets a grey
  imagery bar (black / white bounds in % of the stretch, **A** / **M**
  automatic contrast over the **View** or **All** images, palette and
  inversion on click).
- **Side-scan sonar of recreational sounders** (Lowrance / Simrad / B&G
  `.sl2` / `.sl3` logs, StructureScan / SideImaging channel) imported from the
  *Data* tab (*Imagery* box) as a georeferenced mosaic. Slant range corrected to ground
  range with the depth of each ping. Positions interpolated between GPS fixes,
  heading taken from the smoothed track. Interference from other sounders
  (short streaks on a single ping) removed. Optional sliding equalisation:
  same mean brightness from the centre to the edge of the swath, despite gain
  or range changes, and noise beyond the acoustic range cut. Overlapping lines
  are averaged and weighted by signal strength. Height above the bottom read
  on the image itself (first bottom echo), track smoothed, ping-to-ping
  gain stripes corrected, and each ping resampled on the ground (sharper
  image near the boat: sand ripples become visible). Transducer position
  relative to the GPS antenna: estimated on lines run in opposite directions
  (patch test: the same seabed patch seen both ways is shifted by twice the
  offset), or entered by hand. Mosaic pixel: range / 400,
  between 0.1 and 1 m, widened for very large surveys. An 8 km, 1.1 GB log
  imports in about 40 s.
- **Echo sounder tracks: transducer / GPS antenna offset** (import window,
  *Advanced* section). The logs and GPX files never say whether positions are
  corrected; the offset is estimated on lines run in opposite directions,
  first on the depths (patch test: over a slope that varies along the lines,
  both directions only give the same depth once the offset is applied; the
  value found also includes the lag of the sounder's depth), otherwise on the
  side-scan of the log; or entered by hand. On Baikal's lake survey: about
  6.5 m, found the same on both halves of the survey. Shown in the source
  analysis. When estimated, the *forward* / *starboard* fields are greyed out
  and show the offset obtained (same for the side-scan in the *Imagery* box);
  unticked, they hold the measured values, also used when the estimate fails.
- **Panel reorganised**: the *Palette* and *Contours and soundings* tabs are
  merged into *Display*, which now holds everything about how the data look,
  without scrolling: layers and vertical reference of the depths shown (chart
  datum, LAT, mean sea level, ellipsoid) at the top; palette and thresholds,
  contours and soundings on the left; relief and smoothing, imagery, banner
  and cursor on the right. Short labels, full text in tooltips.
- **Data tab**: the *Database* and *Imagery* tabs are merged (three tabs
  left: Data, Tide, Display). At the top, the database folder (`.pdb` and
  `.pdi`) with a single *Stop* button and status line; on the left, the
  bathymetry files and the coastline; on the right, the imagery and the
  vertical reference surfaces. The panel opens at the size that fits every
  tab without scrolling.
- **Contours and soundings in the palette banner**: a bottom line shows or
  hides the contours and the soundings with a click, and changes the contour
  interval (round values, palette thresholds or a typed value), the major
  contour, the decimals and the text size of the soundings. With the depth
  colours hidden, the line stays alone.
- Depth contours and soundings, when ticked, always stay on top of the
  depth colours and of the imagery, and remain shown when the *Bathymetry*
  colours are switched off (e.g. contours over a reflectivity mosaic).
  The *Database* tab keeps the reference surfaces (MSS, other countries'
  files, update, migration); the datum of the original data is still set at
  import.
- **GeoTIFF grids without GDAL**: built-in reader, so GeoTIFF bathymetry
  (`.tif .tiff`) now imports on every platform, including the Windows
  package. Stripped or tiled files, uncompressed, LZW, Deflate or PackBits,
  integer or floating-point values, BigTIFF (over 4 GB), read block by block
  without loading the whole file. The coordinate system is read from the
  file: geographic, EPSG projected systems, or user-defined Mercator,
  transverse Mercator and Lambert projections; otherwise from a `.tfw` file.
  NoData from the file. Builds with GDAL fall back to it for other variants.
- Grids in degrees (GeoTIFF, ASC, text) are stored at their finer step: the
  east–west step at mid and high latitudes (e.g. 4.0 m instead of 6.4 m at
  51° N for a "5 m" grid). Before, the coarser north–south step was used and
  east–west detail was lost. The database pixels between two grid nodes are
  filled by linear interpolation between the nodes (along rows, then
  columns): no empty rows or columns, and contours without steps. These
  filled pixels are shown like the grid but never taken as soundings.
  Re-import such grids to benefit. Databases holding such grids can no longer
  be fully read by earlier versions.

### 0.1.0-beta.12 (2026-11-17)

- **Echo sounder tracks**: import of GPX files (Garmin and others: track,
  route and waypoints carrying a depth extension, `gpxx:Depth`,
  `gpxtpx:depth`…) and of Lowrance / Simrad / B&G sonar logs `.sl2` / `.sl3`
  (depth under the transducer of each ping; sonar image skipped; primary
  channel, else DownScan, else secondary; pings at the same GPS fix reduced
  to their median). Soundings keep their time: observed water levels apply;
  the import *Z offset* adds the transducer depth. A 1 GB log is read in a
  few seconds on a PC (reading the file dominates, also on a Raspberry Pi).
- **Own shorelines** (*Database* tab, *Coastline* box, *Own*): KML / KMZ
  (Google Earth polygons and lines), generic shapefiles (polygons and
  polylines, 2D/Z/M, `.prj` system) and GeoPackage (`.gpkg`, all feature
  tables, table system), in addition to the OSMSHP / GSHHG basemap. The
  water side is that of the soundings: a polygon around the soundings
  outlines water (a lake and its islands), a polygon without soundings
  outlines land (islands, coast). Large shapefiles are read on demand by
  1° square (the 170 MB *Limite terre-mer* of France: 0.3 s, a few MB).
- **Shoreline as the 0 m contour** (*unreferenced* sources: lakes,
  reservoirs): at import the interpolated surface slopes down to 0 at the
  shoreline instead of stopping at the last sounding line. The shoreline is
  used by the interpolation only: no sounding is added, and the
  interpolation distance D remains the limit.
- **Vertical reference surfaces kept in each `.pdb`**: every database file
  keeps a copy of the surfaces over its area. A `.pdb` copied alone to
  another folder, or shared without `PersonalDepth_datum.sqlite`, brings its
  surfaces back when the database is opened and is shown at the same chart
  datum. Existing databases get their copies at the first opening (quick
  afterwards: nothing is rewritten while the surfaces do not change).
- **Official chart datum of other countries** (*Database* tab, *Vertical
  datum* box, *Countries*): one folder for the official files, recognised
  automatically and used where they exist, LAT FES elsewhere. NOAA VDatum
  regional grids (`mllw.gtx`, MLLW of the United States) are read directly;
  other countries (VORF, HyVSEPs, NLLAT2018…) through `.pdgrid` files of
  quantity `CD/MSL` or `CD/WGS84` (`tools/pdgrid_convert.py --quantity`). The
  box says in plain words whether the chart datum shown is official (✓) or
  approximate (≈ LAT FES). The default display is renamed *Official chart
  datum of the country, like nautical charts*.
- Fixed: with the chart rotated (course up or head up, or rotation keys),
  for some headings the bathymetry switched to a very coarse level (blurred,
  no contours): the screen corners were taken for an antimeridian crossing,
  which falsified the display scale. Reported by Baikal.
- Chart rotated (course up, head up): only the cells of the rotated screen
  and its margin are computed, no longer the whole rectangle around it
  (up to twice as many cells), so the view no longer drops to a coarser
  level after a rebuild; the margin shrinks (down to 10 %) when needed.
- Zooming is more responsive: a database view still being computed is
  stopped as soon as the zoom or position makes it useless, instead of
  chaining outdated views.
- Smoother panning and zooming with depth contours: contours are projected
  once when computed, then placed on screen by the graphics card (a single
  transformation per frame) instead of being reprojected point by point at
  every frame; their labels are placed the same way.
- Zoomed in beyond the resolution of the data, the view keeps a full margin
  around the screen: fewer rebuilds when dragging the chart.
- No more jolt when a new database view arrives (OpenGL): its textures and
  contours are prepared a few milliseconds per frame while the current view
  stays on screen, then it replaces it. While the chart is dragged, soundings
  and contour values are shifted with it and selected again when it stops.
- Diagnostics: `PerfLog=1` in the `[PlugIns/PersonalDepth]` section of
  `opencpn.ini` writes the display timings of the plugin to the OpenCPN log
  every 3 seconds of activity (frames per second, time per frame, textures,
  contours, view rebuilds).
- *Show interpolated areas lighter*: small gaps between soundings of a dense
  survey (up to 2 cells from a measured cell) are no longer lightened, which
  gave a mesh look at large scales.
- Fixed: small lakes, reservoirs and rivers are classed as land by OSMSHP
  and GSHHG, so the land mask erased the whole interpolation of their
  surveys. When most measured soundings of a tile (or of the survey) fall on
  "land", the basemap does not know this water body and is not applied.

### 0.1.0-beta.11 (2026-11-16)

- Fixed: tide station and FES2022 tide area chosen from **all** the visible
  sources of the database. With surveys on both sides of the Atlantic, the
  "centre" fell in mid-ocean (Azores station shown at Le Conquet). The tide
  now uses the sources shown on the chart, or the chart itself, and follows
  the chart when it moves to another area.

### 0.1.0-beta.10 (2026-11-13)

- **Common vertical datum**: the database now stores depths below **mean sea
  level** and links every datum to it with surfaces computed once and kept in
  the database folder (`PersonalDepth_datum.sqlite`): chart datum (Shom ZH
  from BathyElli v2.1 in France, FES2022 lowest astronomical tide elsewhere),
  LAT (FES2022b, 19 years), WGS84 ellipsoid (BathyElli NM in France, mean
  sea surface CNES_CLS 2025 elsewhere).
  - Import: new vertical references *mean sea level* (GEBCO), *LAT*
    (EMODnet), *ellipsoidal heights* worldwide, *not tied* (lakes,
    reservoirs: never converted).
  - Display (*Database* tab, *Vertical datum*): depths at local chart datum
    (default: same values as the charts), LAT, mean sea level or ellipsoid;
    the cursor shows the datum. With the tide: actual water depth.
  - *Update surfaces* recomputes the surfaces after a reference file is
    added or changed and corrects the sources concerned; without reference
    files, chart datum is "undefined" (zero offset) and values are shown as
    imported.
  - **Databases created by earlier versions** (depths at chart datum):
    migration offered when opened, after a copy of the `.pdb` files to
    `backup_before_MSL`; the display at chart datum is unchanged. Import is
    blocked until migration. A migrated database must not be opened with an
    earlier version.
  - New tool `tools/pdgrid_convert.py`: converts the AVISO mean sea surface
    (`mss_cnes_cls_2025.nc`, 1.7 GB) to a compact `.pdgrid` file (124 MB)
    read by tiles (see `docs/external-data.md`).
  - *Clean* bounds are compared at local chart datum.
- **Coastline**: interpolated surfaces no longer cover islands and the coast.
  The land polygons of OpenCPN's basemaps are read directly, OSMSHP
  (OpenStreetMap, `basemap_*.shp`, recommended) or GSHHG (`poly-*-1.dat`),
  best quality present (*Database* tab, *Coastline*). Interpolated pixels on
  land are removed at import; *Apply to sources* does it for sources already
  imported. Measured soundings are always kept. Suggested by ian5142.
- **Import window reorganised**: files and everyday settings always shown;
  coordinates, vertical reference, observed water levels and advanced
  options (offset, NODATA, Z bounds, cache) in collapsible
  sections whose title sums up the current setting; rows that do not apply
  are hidden. No more scrolling on usual screens. Suggested by Baikal.
  *Fixed resolution* sits next to the interpolation settings.
- *Survey type* and *Nature* merged into one *Data type* list (grid,
  single-beam, dense cloud — multibeam and lidar, which were handled the
  same —, chart soundings, lead line, other point cloud).
- Custom text format: the position of the first sounding is written out,
  with a warning when the latitude is polar (latitude / longitude columns
  probably swapped, as in Garmin Quickdraw exports).
- Fixed: OpenCPN crashed on Windows when a date column held dates before
  1970 (e.g. NONNA-10 contributor dates such as 19451231), as soon as the
  preview showed the date; the survey date is now also taken from such
  files. Reported by ian5142.
- Fixed: raising the max distance on a dense point cloud made the stored
  surface coarser (pixel = sounding spacing instead of the fine
  interpolation step), giving a blocky look. Reported by ian5142.
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

### 0.1.0-beta.14 (2026-11-20)

- Fichiers `.prj` sans code EPSG connu (WKT ESRI exportés par QGIS, WKT2)
  lus paramètre par paramètre : projections Mercator (1SP, 2SP, ESRI, Web
  Mercator), Mercator transverse (Gauss-Krüger), Lambert conique conforme et
  stéréographiques, ellipsoïde, TOWGS84 (datums courants comme Pulkovo 1942,
  ED50, OSGB36, NAD27, NTF s'il manque). S'applique aux traits de côte
  personnels (shapefile) et aux levés accompagnés d'un `.prj`. Avant, un
  trait de côte en « World Mercator » n'était pas reconnu. Signalé par Baikal.
- Traits de côte personnels tracés en **polylignes refermées** (premier point
  = dernier : contours de lac et d'îles exportés en lignes par QGIS)
  désormais pris comme polygones : ils masquent la terre comme des polygones.
  Avant, seuls les shapefiles de polygones masquaient la terre.
- Un fichier de cellule (`.pdb`, `.pdi`) dont la dernière source est retirée
  est maintenant supprimé, au lieu de rester dans la liste avec 0 source. Les
  fichiers vides laissés par les versions précédentes sont supprimés à
  l'ouverture de la base.
- Corrigé : le résumé de la palette automatique (onglet *Affichage*) restait
  dans la langue ou l'unité précédente après leur changement dans OpenCPN.
- **Bandeau de palette** : la ligne isobathes / sondes est remplacée par une
  icône (anneaux) qui ouvre un menu : isobathes, seuils de la palette ou
  intervalle (valeurs rondes ou saisie), sondes, réglages. Isobathe
  maîtresse, décimales et taille du texte restent dans l'onglet *Affichage*.
  Suggestion de Baikal.
- **Icône de la barre d'outils** : nouveau réglage *Icône* (onglet
  *Affichage*) : elle ouvre le panneau (comme avant) ou affiche / masque tout
  le plugin. Le panneau s'ouvre aussi par le clic droit sur la carte
  (*PersonalDepth…*). Suggestion de Baikal.
- **Réglages d'import de l'imagerie** : fond, pixel, égalisation et décalage
  du sonar réunis sous *Réglages d'import*, avec **Réimporter la sélection**
  pour les appliquer aux images déjà dans la base (même date, même classe).

### 0.1.0-beta.13 (2026-11-19)

- **Imagerie du fond** (réflectivité, rétrodiffusion, mosaïques de sonar
  latéral) : cadre *Imagerie* de l'onglet *Données*. Les images GeoTIFF (dB, niveaux de
  gris 8/16 bits, palette) s'importent dans une base d'imagerie (fichiers
  `PD_*.pdi`, dans le même dossier que la bathymétrie). C'est le cas des
  mosaïques diffusées gratuitement par l'Ifremer, Infomar ou EMODnet. Les
  valeurs d'origine sont conservées et lues au curseur (ex. `Imagerie
  -18.4 dB`). Chaque image est étirée pour l'affichage entre ses valeurs à
  2 % et 98 %, mesurées à l'import. Le fond uniforme autour d'un levé (blanc
  255 ou noir 0) est détecté et écarté automatiquement. Rendu en niveaux de
  gris, cuivre ou sépia, avec points noir et blanc, gamma et inversion,
  modifié instantanément sans relire les fichiers. Les interrupteurs
  *Bathymétrie* et *Imagerie* sont séparés. Les deux couches se
  superposent, l'utilisateur choisit celle du dessus et chacune a sa propre
  opacité. Isobathes et sondes restent toujours au-dessus. Une mosaïque de
  50 millions de pixels s'importe en 20 s environ. Les réglages d'affichage
  des deux couches sont réunis dans l'onglet *Affichage*. Le bandeau de
  palette reçoit une rampe grise pour l'imagerie (bornes noir / blanc en %
  de l'étirement, **A** / **M** contraste automatique sur la **Vue** ou
  **Tout**, palette et inversion au clic).
- **Sonar latéral des sondeurs de plaisance** (journaux Lowrance / Simrad /
  B&G `.sl2` / `.sl3`, canal StructureScan / SideImaging) importé depuis
  l'onglet *Données* (cadre *Imagerie*) en mosaïque géoréférencée. Distance oblique ramenée à
  la distance au sol avec la profondeur de chaque ping. Positions interpolées
  entre les points GPS, cap tiré de la route lissée. Parasites des autres
  sondeurs (traits brefs sur un seul ping) retirés. Égalisation glissante
  facultative : même luminosité moyenne du centre au bord de la fauchée,
  malgré les changements de gain ou de portée, et bruit coupé au-delà de la
  portée acoustique. Les lignes qui se recouvrent sont moyennées, pondérées
  par la force du signal. Hauteur au-dessus du fond lue sur l'image même
  (premier écho du fond), route lissée, rayures de gain d'un ping à l'autre
  corrigées, et chaque ping rééchantillonné au sol (image plus nette près du
  bateau : les rides de sable apparaissent). Position de la sonde par rapport
  à l'antenne GPS : estimée sur les lignes parcourues en sens opposés
  (principe du « patch test » : une même tache du fond vue des deux sens est
  décalée de deux fois l'écart), ou saisie. Pixel de la mosaïque : portée / 400, entre 0,1 et
  1 m, élargi pour les levés très étendus. Un journal de 1,1 Go (8 km de
  route) s'importe en 40 s environ.
- **Traces de sondeur : décalage sonde / antenne GPS** (fenêtre d'import,
  section *Avancé*). Journaux et GPX n'indiquent jamais si les positions sont
  corrigées ; le décalage est estimé sur les lignes parcourues en sens
  opposés, d'abord sur les profondeurs (patch test : sur une pente qui varie
  le long des lignes, les deux sens ne donnent la même profondeur qu'une fois
  le décalage appliqué ; la valeur trouvée comprend aussi le retard de la
  profondeur du sondeur), sinon sur le sonar latéral du journal ; ou saisi.
  Levé du lac de Baikal : environ 6,5 m, retrouvé à l'identique sur les deux
  moitiés du levé. Indiqué dans l'analyse de la source. Estimé, les champs
  *avant* / *tribord* sont grisés et montrent le décalage obtenu (de même pour
  le sonar latéral dans le cadre *Imagerie*) ; case décochée, ils portent les
  valeurs mesurées, appliquées aussi quand l'estimation échoue.
- **Panneau réorganisé** : les onglets *Palette* et *Isobathes et sondes*
  rejoignent *Affichage*, qui réunit tout ce qui règle l'aspect des données,
  sans défilement : en haut, couches et référence verticale des profondeurs
  affichées (zéro des cartes, LAT, niveau moyen, ellipsoïde) ; à gauche,
  palette et seuils, isobathes et sondes ; à droite, relief et lissage,
  imagerie, bandeau et curseur. Libellés courts, texte complet en bulle
  d'aide.
- **Onglet Données** : les onglets *Base* et *Imagerie* sont réunis (trois
  onglets : Données, Marée, Affichage). En haut, le dossier de la base
  (`.pdb` et `.pdi`) avec un seul bouton *Arrêter* et une seule ligne d'état ;
  à gauche, les fichiers de bathymétrie et le trait de côte ; à droite,
  l'imagerie et les surfaces de référence verticale. Le panneau s'ouvre à la
  taille qui fait tenir chaque onglet sans défilement.
- **Isobathes et sondes dans le bandeau de palette** : une ligne en bas
  affiche ou masque d'un clic les isobathes et les sondes, et change
  l'intervalle des isobathes (valeurs rondes, seuils de la palette ou valeur
  saisie), l'isobathe maîtresse, les décimales et la taille du texte des
  sondes. Couleurs de profondeur masquées, la ligne reste seule.
- Isobathes et sondes, si elles sont cochées, restent toujours au-dessus des
  couleurs de profondeur et de l'imagerie, et restent affichées quand les
  couleurs *Bathymétrie* sont masquées (ex. isobathes sur une mosaïque de
  réflectivité).
  L'onglet *Base* garde les surfaces de référence (MSS, fichiers des autres
  pays, mise à jour, migration) ; la référence des données d'origine reste
  définie à l'import.
- **Grilles GeoTIFF sans GDAL** : lecteur intégré ; la bathymétrie en
  GeoTIFF (`.tif .tiff`) s'importe maintenant sur toutes les plateformes, y
  compris le paquet Windows. Fichiers en bandes ou en tuiles, sans
  compression, LZW, Deflate ou PackBits, valeurs entières ou réelles, BigTIFF
  (plus de 4 Go), lus bloc par bloc sans charger tout le fichier. Le système
  de coordonnées est lu dans le fichier : géographique, systèmes projetés
  EPSG, ou projections Mercator, Mercator transverse et Lambert définies par
  l'utilisateur ; à défaut, dans un fichier `.tfw`. NoData du fichier. Les
  versions compilées avec GDAL s'en servent pour les autres variantes.
- Grilles en degrés (GeoTIFF, ASC, texte) rangées à leur pas le plus fin :
  le pas est-ouest aux latitudes moyennes et hautes (ex. 4,0 m au lieu de
  6,4 m à 51° N pour une grille « 5 m »). Avant, le pas nord-sud, plus
  grossier, était retenu et le détail est-ouest perdu. Les pixels de la base
  situés entre deux nœuds de la grille sont comblés par interpolation
  linéaire entre les nœuds (le long des lignes, puis des colonnes) : ni ligne
  ni colonne vide, et des isobathes sans marches. Ces pixels comblés
  s'affichent comme la grille mais ne sont jamais pris comme sondes
  chiffrées. Réimporter ces grilles pour en profiter. Une base qui contient
  de telles grilles n'est plus entièrement lisible par les versions
  antérieures.

### 0.1.0-beta.12 (2026-11-17)

- **Traces de sondeur** : import des fichiers GPX (Garmin et autres : traces,
  routes et waypoints portant une profondeur en extension, `gpxx:Depth`,
  `gpxtpx:depth`…) et des journaux sonar Lowrance / Simrad / B&G `.sl2` /
  `.sl3` (profondeur sous le transducteur de chaque ping ; image sonar
  sautée ; canal primaire, sinon DownScan, sinon secondaire ; pings d'une
  même position GPS réduits à leur médiane). Les sondes gardent leur heure :
  les hauteurs d'eau observées s'appliquent ; le *décalage Z* de l'import
  ajoute l'enfoncement du transducteur. Un journal de 1 Go se lit en
  quelques secondes sur PC (la lecture du fichier domine, aussi sur
  Raspberry Pi).
- **Traits de côte personnels** (onglet *Base*, cadre *Trait de côte*,
  *Personnels*) : KML / KMZ (polygones et lignes de Google Earth),
  shapefiles quelconques (polygones et polylignes, 2D/Z/M, système du
  `.prj`) et GeoPackage (`.gpkg`, toutes les tables d'entités, système de la
  table), en plus du fond OSMSHP / GSHHG. Le côté eau est celui des sondes :
  un polygone qui entoure les sondes trace l'eau (un lac et ses îles), un
  polygone sans sondes trace la terre (îles, côte). Les gros shapefiles sont
  lus à la demande par carré de 1° (*Limite terre-mer* de la France,
  170 Mo : 0,3 s, quelques Mo).
- **Rivage = isobathe 0** (sources *non rattachées* : lacs, retenues) : à
  l'import, la surface interpolée descend vers 0 au rivage au lieu de
  s'arrêter à la dernière ligne de sondes. Le rivage ne sert qu'à
  l'interpolation : aucune sonde n'est ajoutée, et la distance D
  d'interpolation reste la limite.
- **Surfaces de référence verticale gardées dans chaque `.pdb`** : chaque
  fichier de base garde une copie des surfaces de son emprise. Un `.pdb`
  copié seul dans un autre dossier, ou partagé sans
  `PersonalDepth_datum.sqlite`, rapporte ses surfaces à l'ouverture de la
  base et s'affiche au même zéro des cartes. Les bases existantes reçoivent
  leurs copies à la première ouverture (rapide ensuite : rien n'est réécrit
  tant que les surfaces ne changent pas).
- **Zéro des cartes officiel des autres pays** (onglet *Base*, cadre
  *Référence verticale*, *Pays*) : un dossier pour les fichiers officiels,
  reconnus tout seuls et utilisés là où ils existent, LAT FES ailleurs. Les
  grilles régionales NOAA VDatum (`mllw.gtx`, MLLW des États-Unis) sont lues
  directement ; les autres pays (VORF, HyVSEPs, NLLAT2018…) par des fichiers
  `.pdgrid` de grandeur `CD/MSL` ou `CD/WGS84` (`tools/pdgrid_convert.py
  --quantity`). Le cadre dit en clair si le zéro affiché est officiel (✓) ou
  approché (≈ LAT FES). L'affichage par défaut s'appelle maintenant *Zéro des
  cartes officiel du pays, comme les cartes marines*.
- Corrigé : carte tournée (route ou cap en haut, ou touches de rotation) :
  pour certains caps, la bathymétrie passait à un niveau très grossier (floue,
  sans isobathes) ; les coins de l'écran étaient pris pour un franchissement
  de l'antiméridien, ce qui faussait l'échelle d'affichage. Signalé par Baikal.
- Carte tournée (route ou cap en haut) : seules les cellules de l'écran
  tourné et de sa marge sont calculées, et non plus tout le rectangle qui
  l'englobe (jusqu'à deux fois plus de cellules) ; la vue ne descend plus à
  un niveau plus grossier à son rechargement ; la marge se réduit (jusqu'à
  10 %) si nécessaire.
- Zoom plus réactif : une vue de la base en cours de calcul est arrêtée dès
  que le zoom ou le déplacement la rend inutile, au lieu d'enchaîner des
  vues périmées.
- Déplacement et zoom plus fluides avec les isobathes : elles sont projetées
  une fois à leur calcul, puis placées à l'écran par la carte graphique (une
  seule transformation par image) au lieu d'être reprojetées point par point
  à chaque image ; leurs valeurs sont placées de la même façon.
- Zoom au-delà de la résolution des données : la vue garde une marge pleine
  autour de l'écran, d'où moins de rechargements en faisant glisser la carte.
- Plus d'à-coup à l'arrivée d'une nouvelle vue de la base (OpenGL) : ses
  textures et ses isobathes sont préparées quelques millisecondes par image
  pendant que la vue actuelle reste affichée, puis elle la remplace. Pendant
  qu'on fait glisser la carte, les sondes et les valeurs d'isobathes suivent
  le mouvement et sont choisies à nouveau à l'arrêt.
- Diagnostic : `PerfLog=1` dans la section `[PlugIns/PersonalDepth]` de
  `opencpn.ini` écrit les temps d'affichage du plugin dans le journal
  d'OpenCPN toutes les 3 s d'activité (images par seconde, temps par image,
  textures, isobathes, recalculs de la vue).
- *Zones interpolées plus claires* : les petits trous entre les sondes d'un
  levé dense (jusqu'à 2 cellules d'une cellule mesurée) ne sont plus
  éclaircis ; aux grandes échelles cela donnait un aspect de grillage.
- Corrigé : petits lacs, retenues et rivières sont rangés en terre par OSMSHP
  et GSHHG ; le masque terre effaçait toute l'interpolation de leurs levés.
  Quand la plupart des sondes mesurées d'une tuile (ou du levé) tombent sur
  la « terre », le fond de carte ne connaît pas ce plan d'eau et n'est pas
  appliqué.

### 0.1.0-beta.11 (2026-11-16)

- Corrigé : station de marée et zone de la marée FES2022 choisies d'après
  **toutes** les sources visibles de la base. Avec des levés des deux côtés
  de l'Atlantique, le « centre » tombait en plein océan (station des Açores
  affichée au Conquet). La marée utilise maintenant les sources affichées à
  l'écran, ou la carte elle-même, et suit la carte quand elle change de zone.

### 0.1.0-beta.10 (2026-11-13)

- **Référence verticale commune** : la base range désormais les profondeurs
  sous le **niveau moyen** et y relie chaque référence par des surfaces
  calculées une fois et gardées dans le dossier de la base
  (`PersonalDepth_datum.sqlite`) : zéro des cartes (ZH du Shom par BathyElli
  v2.1 en France, plus basse mer astronomique FES2022 ailleurs), LAT
  (FES2022b, 19 ans), ellipsoïde WGS84 (BathyElli NM en France, surface
  moyenne océanique CNES_CLS 2025 ailleurs).
  - Import : nouvelles références *niveau moyen* (GEBCO), *LAT* (EMODnet),
    *hauteurs ellipsoïdales* partout, *non rattachée* (lacs, retenues :
    jamais converties).
  - Affichage (onglet *Base*, *Référence verticale*) : profondeurs au zéro
    des cartes local (par défaut : mêmes valeurs que les cartes), au LAT, au
    niveau moyen ou à l'ellipsoïde ; le curseur indique la référence. Avec la
    marée : hauteur d'eau réelle.
  - *Mettre à jour les surfaces* recalcule les surfaces après l'ajout ou le
    changement d'un fichier de référence et corrige les sources concernées ;
    sans fichiers de référence, le zéro des cartes est « non défini » (écart
    nul) et les valeurs s'affichent telles qu'importées.
  - **Bases créées par les versions antérieures** (profondeurs au zéro des
    cartes) : migration proposée à l'ouverture, après copie des fichiers
    `.pdb` dans `backup_before_MSL` ; l'affichage au zéro des cartes est
    inchangé. L'import est bloqué tant que la base n'est pas migrée. Une base
    migrée ne doit plus être ouverte avec une version antérieure.
  - Nouvel outil `tools/pdgrid_convert.py` : convertit la surface moyenne
    océanique d'AVISO (`mss_cnes_cls_2025.nc`, 1,7 Go) en un fichier `.pdgrid`
    compact (124 Mo), lu par tuiles (voir `docs/external-data.md`).
  - Les bornes de *Nettoyer* sont comparées au zéro des cartes local.
- **Trait de côte** : les surfaces interpolées ne couvrent plus les îles ni la
  côte. Les polygones de terre des fonds de carte d'OpenCPN sont lus
  directement, OSMSHP (OpenStreetMap, `basemap_*.shp`, conseillé) ou GSHHG
  (`poly-*-1.dat`), dans la meilleure qualité présente (onglet *Base*, *Trait
  de côte*). Les pixels interpolés sur la terre sont retirés à l'import ;
  *Appliquer aux sources* le fait pour les sources déjà importées. Les sondes
  mesurées sont toujours gardées. Suggéré par ian5142.
- **Fenêtre d'import réorganisée** : fichiers et réglages courants toujours
  visibles ; coordonnées, référence verticale, hauteurs d'eau observées et
  options avancées (décalage, NODATA, bornes de Z, cache)
  dans des sections repliables dont le titre résume le réglage ; lignes
  inutiles masquées. Plus de défilement sur un écran courant. Suggestion de
  Baikal. La *Résolution imposée* est placée avec les réglages d'interpolation.
- *Type de levé* et *Nature* réunis en une seule liste *Type de données*
  (grille, monofaisceau, nuage dense — multifaisceau et lidar, traités de la
  même façon —, sondes de carte, plomb, autre nuage de points).
- Format texte personnalisé : position de la première sonde écrite en
  clair, avec un avertissement si la latitude est polaire (colonnes latitude
  / longitude sans doute inversées, comme dans les exports Garmin
  Quickdraw).
- Corrigé : OpenCPN plantait sous Windows quand une colonne date contenait
  des dates antérieures à 1970 (ex. dates des contributeurs NONNA-10 comme
  19451231), dès que l'aperçu affichait la date ; la date du levé est
  maintenant aussi tirée de ces fichiers. Signalé par ian5142.
- Corrigé : augmenter la distance max sur un nuage dense rendait la surface
  stockée plus grossière (pixel = espacement des sondes au lieu du pas fin de
  l'interpolation), d'où un aspect en blocs. Signalé par ian5142.
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

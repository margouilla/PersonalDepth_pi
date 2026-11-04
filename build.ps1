# PersonalDepth_pi - compilation + test + installation
# Usage : powershell -ExecutionPolicy Bypass -File build.ps1   [-Clean] [-NoInstall] [-Package] [-Pre beta.1]
#   -Package : cree aussi dist\PersonalDepth-<version>_msvc-wx32-10.tar.gz, a installer
#              dans OpenCPN par Options > Plugins > Importer (format du gestionnaire de plugins)
param([switch]$Clean, [switch]$NoInstall, [switch]$Package, [string]$Pre = "beta.1")

# --- identite du plugin (metadata.xml du paquet)
$Author = "Marcaurelio Franzetti (margouilla)"
$Repo   = "https://github.com/margouilla/PersonalDepth_pi"
$ErrorActionPreference = "Stop"
$root = $PSScriptRoot
$wx   = Join-Path (Split-Path $root -Parent) "deps\wxWidgets-3.2.6"
$vs   = "C:\Program Files\Microsoft Visual Studio\18\Community\Common7\Tools\Launch-VsDevShell.ps1"
Set-Location $root

# 1. Environnement Visual Studio (32 bits, comme OpenCPN)
if (-not (Get-Command cl.exe -ErrorAction SilentlyContinue)) {
  & $vs -Arch x86 -HostArch amd64 -SkipAutomaticLocation | Out-Null
}
Set-Location $root

# 2. Configuration (seulement la premiere fois, ou avec -Clean)
if ($Clean -and (Test-Path build)) { Remove-Item build -Recurse -Force }
if (-not (Test-Path build\CMakeCache.txt)) {
  Write-Host "== Configuration ==" -ForegroundColor Cyan
  cmake -S . -B build -G "Visual Studio 18 2026" -A Win32 -DPD_WITH_GDAL=OFF -Wno-dev `
    "-DwxWidgets_ROOT_DIR=$wx" "-DwxWidgets_LIB_DIR=$wx\lib\vc14x_dll"
  if ($LASTEXITCODE) { Write-Host "ECHEC de la configuration" -ForegroundColor Red; exit 1 }
}

# 3. Compilation (sortie complete dans build.log)
Write-Host "== Compilation ==" -ForegroundColor Cyan
cmake --build build --config Release *> build.log
$errs = Select-String -Path build.log -Pattern ": error|fatal error| error LNK" | Select-Object -First 20
if ($errs -or $LASTEXITCODE) {
  $errs | ForEach-Object { Write-Host $_.Line -ForegroundColor Red }
  Write-Host "ECHEC de la compilation - details dans build.log" -ForegroundColor Red
  exit 1
}
Write-Host "Compilation OK" -ForegroundColor Green

# 4. Test du coeur
Write-Host "== Test ==" -ForegroundColor Cyan
ctest --test-dir build -C Release --output-on-failure
if ($LASTEXITCODE) { Write-Host "ECHEC du test" -ForegroundColor Red; exit 1 }

# 5. Paquet pour Options > Plugins > Importer
if ($Package) {
  Write-Host "== Paquet ==" -ForegroundColor Cyan
  $m = Select-String -Path CMakeLists.txt -Pattern 'project\(PersonalDepth_pi VERSION ([0-9.]+)' | Select-Object -First 1
  $ver = $m.Matches[0].Groups[1].Value
  if ($Pre) { $ver = "$ver-$Pre" }
  $name  = "PersonalDepth-" + $ver + "_msvc-wx32-10"
  $pkg   = Join-Path $root "build\package"
  $stage = Join-Path $pkg $name
  if (Test-Path $pkg) { Remove-Item $pkg -Recurse -Force }
  New-Item -ItemType Directory -Force (Join-Path $stage "plugins\PersonalDepth_pi\data") | Out-Null
  Copy-Item build\Release\PersonalDepth_pi.dll (Join-Path $stage "plugins")
  Copy-Item data\personaldepth.svg (Join-Path $stage "plugins\PersonalDepth_pi\data")
  Copy-Item LICENSE (Join-Path $stage "plugins\PersonalDepth_pi\data\license.txt")
  $authorXml = if ($Author) { "  <author>$Author</author>`n" } else { "" }
  $sourceXml = if ($Repo) { "  <source>$Repo</source>`n  <info-url>$Repo</info-url>`n" } else { "" }
  $xml = @"
<?xml version="1.0" encoding="UTF-8"?>
<plugin version="1">
  <name>PersonalDepth</name>
  <version>$ver</version>
  <release>1</release>
  <summary>Personal bathymetry: XYZ, grids and TIN with a live colour ramp</summary>
  <api-version>1.18</api-version>
  <open-source>yes</open-source>
$authorXml$sourceXml  <description>
Displays large personal bathymetric
surveys (XYZ, ESRI ASCII, LandXML) as
a shaded coloured surface with exact
soundings. Palette, depth thresholds
and safety contour change instantly.
Surveys are merged in a local database
by priority; tide from OpenCPN
stations or FES2022.
Beta test version, French interface.
  </description>
  <target>msvc-wx32</target>
  <target-version>10</target-version>
  <target-arch>x86_64</target-arch>
</plugin>
"@
  [IO.File]::WriteAllText((Join-Path $stage "metadata.xml"), $xml, (New-Object Text.UTF8Encoding($false)))
  $dist = Join-Path $root "dist"
  New-Item -ItemType Directory -Force $dist | Out-Null
  $tgz = Join-Path $dist ($name + ".tar.gz")
  if (Test-Path $tgz) { Remove-Item $tgz -Force }
  tar -czf $tgz -C $pkg $name
  if ($LASTEXITCODE) { Write-Host "ECHEC de la creation du paquet" -ForegroundColor Red; exit 1 }
  Write-Host "Paquet : $tgz" -ForegroundColor Green
  if (-not $Author) { Write-Host "(auteur non renseigne : variable `$Author en tete de build.ps1)" -ForegroundColor Yellow }
}

# 6. Installation dans le dossier plugins de l'utilisateur
if ($NoInstall) { exit 0 }
if (Get-Process opencpn -ErrorAction SilentlyContinue) {
  Write-Host "OpenCPN est ouvert : fermez-le puis relancez ce script pour installer." -ForegroundColor Yellow
  exit 0
}
$dest = Join-Path $env:LOCALAPPDATA "opencpn\plugins"
New-Item -ItemType Directory -Force $dest | Out-Null
Copy-Item build\Release\PersonalDepth_pi.dll $dest -Force
# References verticales (BathyElli v2.1 ZH + RAF20 + RAC23) livrees avec le plugin
$refSrc = Join-Path (Split-Path $root -Parent) "ReferenceElevation"
$refDst = Join-Path $dest "PersonalDepth_pi\references"
if (Test-Path $refSrc) {
  New-Item -ItemType Directory -Force $refDst | Out-Null
  Get-ChildItem $refSrc -Recurse -Include *ZH*.glhi, RAF20*.tac, RAC*.mnt | Copy-Item -Destination $refDst -Force
  Write-Host "References verticales copiees dans $refDst" -ForegroundColor Green
}
Write-Host "Installe dans $dest - vous pouvez lancer OpenCPN." -ForegroundColor Green

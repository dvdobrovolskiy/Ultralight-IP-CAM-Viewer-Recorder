# Builds plugins into dist\plugins (portable layout, next to the app exe).
# Every plugins\<name>\<name>.c becomes dist\plugins\<name>.dll. MSYS2 UCRT64 GCC, no CMake needed.
#   .\build_plugin_portable.ps1                 # all plugins
#   .\build_plugin_portable.ps1 -Plugin face_counter
param(
  [string]$Plugin = "",
  [string]$OutDir = "dist\plugins"
)
$ErrorActionPreference = "Stop"
$root = $PSScriptRoot
$cc = "C:\msys64\ucrt64\bin\cc.exe"
$cxx = "C:\msys64\ucrt64\bin\g++.exe"
if (-not (Test-Path $cc)) { throw "MSYS2 UCRT64 GCC not found at $cc. Install mingw-w64-ucrt-x86_64-toolchain." }
$env:Path = "C:\msys64\ucrt64\bin;" + $env:Path   # gcc needs its runtime DLLs on PATH
$out = Join-Path $root $OutDir
New-Item -ItemType Directory -Force -Path $out | Out-Null

$dirs = Get-ChildItem (Join-Path $root "plugins") -Directory
if ($Plugin) { $dirs = $dirs | Where-Object { $_.Name -eq $Plugin }; if (-not $dirs) { throw "No plugins\$Plugin" } }
$built = @()
foreach ($d in $dirs) {
  $name = $d.Name
  $src = Join-Path $d.FullName "$name.c"
  if (-not (Test-Path $src)) { continue }
  $dll = Join-Path $out "$name.dll"
  Write-Host "Building $name.dll ..." -ForegroundColor Cyan
  # Extra C++ sources (e.g. plugins\<name>\libfacedetection\*.cpp) are compiled with g++ and
  # linked in statically, so the DLL still has no runtime dependencies.
  $cpps = @(Get-ChildItem $d.FullName -Recurse -Filter *.cpp | ForEach-Object { $_.FullName })
  if ($cpps.Count -eq 0) {
    & $cc -shared -O2 -ffast-math -Wall -Wextra -static-libgcc -s -o $dll $src -lwinhttp -lversion
    if ($LASTEXITCODE -ne 0) { throw "Failed to build $name (close the app if $name.dll is loaded)" }
  } else {
    $obj = Join-Path $env:TEMP ("plugin_build_" + $name)
    New-Item -ItemType Directory -Force -Path $obj | Out-Null
    $objs = @()
    $o = Join-Path $obj "$name.o"
    & $cc -c -O2 -ffast-math -Wall -Wextra -o $o $src
    if ($LASTEXITCODE -ne 0) { throw "Failed to compile $src" }
    $objs += $o
    foreach ($cpp in $cpps) {
      $o = Join-Path $obj ([IO.Path]::GetFileNameWithoutExtension($cpp) + ".o")
      & $cxx -c -O3 -w -o $o $cpp
      if ($LASTEXITCODE -ne 0) { throw "Failed to compile $cpp" }
      $objs += $o
    }
    & $cxx -shared -static -s -o $dll @objs -lwinhttp -lversion
    if ($LASTEXITCODE -ne 0) { throw "Failed to link $name (close the app if $name.dll is loaded)" }
  }
  $built += $dll
  # Data files shipped next to the DLL (e.g. face_counter's sface_weights.bin) go into
  # dist\plugins\<name>\ - the plugin's data dir.
  # plugins\<name>\runtime\* are run-time files loaded by the plugin itself (face_counter:
  # onnxruntime.dll + the SCRFD detector model) and ship in the same data dir.
  $bins = @(Get-ChildItem $d.FullName -Filter *.bin)
  $rt = Join-Path $d.FullName "runtime"
  if (Test-Path $rt) { $bins += @(Get-ChildItem $rt -File) }
  if ($bins.Count -gt 0) {
    $ddir = Join-Path $out $name
    New-Item -ItemType Directory -Force -Path $ddir | Out-Null
    foreach ($bin in $bins) { Copy-Item $bin.FullName (Join-Path $ddir $bin.Name) -Force }
  }
}
if (-not $built) { throw "Nothing built" }
Write-Host "Plugins in $out :" -ForegroundColor Green
$built | ForEach-Object { Write-Host ("  {0}  ({1} KB)" -f (Split-Path -Leaf $_), [math]::Round((Get-Item $_).Length / 1KB)) }
Write-Host "Start the app, press F10, enable the plugin." -ForegroundColor DarkGray

param(
  [string]$BuildDir = "build"
)

$ErrorActionPreference = "Stop"

$root = $PSScriptRoot
$buildDir = Join-Path $root $BuildDir
$distDir = Join-Path $root "dist"

$exe = Join-Path $buildDir "Ultralight_IP_CAM_Viewer_Recorder.exe"
if (-not (Test-Path $exe)) {
  throw "Not built: $exe`nBuild first (see README.md)."
}

New-Item -ItemType Directory -Force -Path $distDir | Out-Null

# Clean dist
Get-ChildItem -Path $distDir -Force -ErrorAction SilentlyContinue | Remove-Item -Force -Recurse

Copy-Item -Force $exe (Join-Path $distDir "Ultralight_IP_CAM_Viewer_Recorder.exe")

# Copy runtime DLLs already staged by CMake into build\
Get-ChildItem -Path $buildDir -Filter *.dll -File | ForEach-Object {
  Copy-Item -Force $_.FullName (Join-Path $distDir $_.Name)
}

# Optional: also include license texts if you have them locally

$zip = Join-Path $root "Ultralight_IP_CAM_Viewer_Recorder-portable.zip"
if (Test-Path $zip) { Remove-Item -Force $zip }
Compress-Archive -Path (Join-Path $distDir "*") -DestinationPath $zip

Write-Host "Portable bundle: $distDir"
Write-Host "Zip: $zip"

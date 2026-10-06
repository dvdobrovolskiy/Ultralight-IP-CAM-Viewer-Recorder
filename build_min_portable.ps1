param(
  # Build directory (CMake/Ninja files). Default is inside dist/ so the configure output matches what you run.
  # Note: dist/ itself is NOT cleaned by this script.
  [string]$BuildDir = "dist\\build-min",
  [string]$FfmpegRoot = "$PSScriptRoot\\third_party\\ffmpeg-min\\install",
  [int]$Jobs = 0
)

$ErrorActionPreference = "Stop"

function Require-Path([string]$p, [string]$hint) {
  if (-not (Test-Path $p)) {
    throw "$hint`nMissing: $p"
  }
}

$root = $PSScriptRoot
$buildPath = Join-Path $root $BuildDir
Write-Host "BuildDir = $buildPath" -ForegroundColor Cyan

# Normalize FFmpeg root for CMake (absolute + forward slashes)
$ffRootAbs = (Resolve-Path $FfmpegRoot).Path
$ffRootCmake = $ffRootAbs -replace '\\', '/'

# MSYS2 UCRT64 tools (no bash needed)
$ucrtBin = "C:\\msys64\\ucrt64\\bin"
$msysUsrBin = "C:\\msys64\\usr\\bin"
Require-Path (Join-Path $ucrtBin "cmake.exe") "MSYS2 UCRT64 CMake not found. Install MSYS2 + packages (cmake/ninja/toolchain)."
Require-Path (Join-Path $ucrtBin "ninja.exe") "MSYS2 UCRT64 Ninja not found. Install mingw-w64-ucrt-x86_64-ninja."
Require-Path (Join-Path $ucrtBin "cc.exe") "MSYS2 UCRT64 GCC not found. Install mingw-w64-ucrt-x86_64-toolchain."

# Minimal FFmpeg prefix
Require-Path (Join-Path $FfmpegRoot "include\\libavformat\\avformat.h") "Minimal FFmpeg headers not found. Build them first: .\\build_ffmpeg_min_ucrt64.ps1 -Install"
Require-Path (Join-Path $FfmpegRoot "lib\\libavformat.a") "Minimal FFmpeg libs not found. Build them first: .\\build_ffmpeg_min_ucrt64.ps1 -Install"

# Ensure tools are on PATH for CMake compiler detection
$oldPath = $env:Path
$env:Path = "$ucrtBin;$msysUsrBin;$env:Path"

$dist = Join-Path $root "dist"

try {
  Write-Host "FFMPEG_ROOT = $ffRootAbs" -ForegroundColor Cyan
  & (Join-Path $ucrtBin "cmake.exe") -S $root -B $buildPath -G Ninja "-DFFMPEG_ROOT:PATH=$ffRootCmake" -DCOPY_RUNTIME_DEPS=ON | Out-Host
  if ($LASTEXITCODE -ne 0) { throw "CMake configure failed" }

  $buildArgs = @("--build", $buildPath)
  if ($Jobs -gt 0) { $buildArgs += @("-j", "$Jobs") }
  & (Join-Path $ucrtBin "cmake.exe") @buildArgs | Out-Host
  if ($LASTEXITCODE -ne 0) { throw "Build failed" }
}
finally {
  $env:Path = $oldPath
}

# Copy only the minimal runtime files into dist/ and do not delete anything already there.
New-Item -ItemType Directory -Force -Path $dist | Out-Null

$exeSrc = Join-Path $buildPath "Ultralight_IP_CAM_Viewer_Recorder.exe"
$sdlSrc = Join-Path $buildPath "SDL2.dll"
$pthSrc = Join-Path $buildPath "libwinpthread-1.dll"

Require-Path $exeSrc "Built executable not found. Build likely failed."

$toCopy = @(
  @{ Name = "Ultralight_IP_CAM_Viewer_Recorder.exe"; Src = $exeSrc }
)

if (Test-Path $sdlSrc) { $toCopy += @{ Name = "SDL2.dll"; Src = $sdlSrc } }
if (Test-Path $pthSrc) { $toCopy += @{ Name = "libwinpthread-1.dll"; Src = $pthSrc } }

foreach ($f in $toCopy) {
  $dst = Join-Path $dist $f.Name
  try {
    Copy-Item -Force $f.Src $dst
  } catch {
    throw ("Failed to copy {0} to dist. Close any running Ultralight_IP_CAM_Viewer_Recorder.exe and retry. Details: {1}" -f $f.Name, $_.Exception.Message)
  }
}

Write-Host "dist\ updated (preserved existing folders/files):" -ForegroundColor Cyan
$toCopy | ForEach-Object { Write-Host ("  {0}" -f $_.Name) }

Write-Host "Done." -ForegroundColor Green

param(
  [string]$Prefix = "$PSScriptRoot\\..\\third_party\\ffmpeg-min\\install",
  [string]$SrcDir = "$PSScriptRoot\\..\\third_party\\ffmpeg-min\\src",
  [string]$BuildDir = "$PSScriptRoot\\..\\third_party\\ffmpeg-min\\build",
  [switch]$Clean,
  [switch]$Update,
  [switch]$Reconfigure,
  [switch]$Install,
  [int]$Jobs = 0
)

$ErrorActionPreference = "Stop"

Write-Warning "This script moved to project root. Use: .\\build_ffmpeg_min_ucrt64.ps1"

$root = Join-Path $PSScriptRoot "..\\build_ffmpeg_min_ucrt64.ps1"
if (-not (Test-Path $root)) {
  throw "Missing: $root"
}

& $root -Prefix $Prefix -SrcDir $SrcDir -BuildDir $BuildDir -Clean:$Clean -Update:$Update -Reconfigure:$Reconfigure -Install:$Install -Jobs $Jobs
exit $LASTEXITCODE

param(
  # Optional camera URL, e.g. "rtsp://user:password@camera-host:554/stream".
  # Without it the app starts with the cameras saved in settings.ini.
  [string]$Url = ""
)

$exe = Join-Path $PSScriptRoot "build\Ultralight_IP_CAM_Viewer_Recorder.exe"
if (-not (Test-Path $exe)) {
  throw "Executable not found: $exe`nBuild first (see README.md)."
}

$msysUcrtBin = "C:\msys64\ucrt64\bin"
if (Test-Path $msysUcrtBin) {
  $env:Path = "$msysUcrtBin;$env:Path"
}

if ($Url) { & $exe $Url } else { & $exe }

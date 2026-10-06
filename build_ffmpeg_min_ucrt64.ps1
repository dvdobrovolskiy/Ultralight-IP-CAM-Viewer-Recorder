param(
  [string]$Prefix = "$PSScriptRoot\\third_party\\ffmpeg-min\\install",
  [string]$SrcDir = "$PSScriptRoot\\third_party\\ffmpeg-min\\src",
  [string]$BuildDir = "$PSScriptRoot\\third_party\\ffmpeg-min\\build",
  [switch]$Clean,
  [switch]$Update,
  [switch]$Reconfigure,
  [switch]$Install,
  [int]$Jobs = 0
)

$ErrorActionPreference = "Stop"

function Bash([string]$cmd) {
  & "C:\\msys64\\usr\\bin\\bash.exe" -lc $cmd
  if ($LASTEXITCODE -ne 0) { throw "bash failed: $cmd" }
}

New-Item -ItemType Directory -Force -Path $Prefix,$SrcDir,$BuildDir | Out-Null

# Pass Windows paths to MSYS2 bash via env; avoid PowerShell here-string interpolation.
$env:FF_PREFIX = $Prefix
$env:FF_SRC = $SrcDir
$env:FF_BUILD = $BuildDir
$env:FF_CLEAN = if ($Clean) { "1" } else { "" }
$env:FF_UPDATE = if ($Update) { "1" } else { "" }
$env:FF_RECONF = if ($Reconfigure) { "1" } else { "" }
$env:FF_INSTALL = if ($Install) { "1" } else { "" }
$env:FF_JOBS = if ($Jobs -gt 0) { "$Jobs" } else { "" }

# Requirements:
#   (script will install via pacman if missing)
Bash @'
set -e;
export PATH=/ucrt64/bin:/usr/bin:\$PATH;
pacman -S --noconfirm --needed diffutils git make mingw-w64-ucrt-x86_64-toolchain mingw-w64-ucrt-x86_64-nasm || true;
prefix=$(cygpath -u "$FF_PREFIX");
src=$(cygpath -u "$FF_SRC");
build=$(cygpath -u "$FF_BUILD");
echo "prefix=$prefix";
echo "src=$src";
echo "build=$build";
if [ ! -d "$src/.git" ]; then rm -rf "$src"; git clone --depth 1 https://github.com/FFmpeg/FFmpeg.git "$src"; fi;
if [ "${FF_UPDATE:-}" = "1" ]; then
  git -C "$src" fetch --depth 1 origin;
  git -C "$src" reset --hard FETCH_HEAD;
fi;
test -f "$src/configure";
if [ "${FF_CLEAN:-}" = "1" ]; then rm -rf "$build"; fi;
mkdir -p "$build";
cd "$build";
jobs="${FF_JOBS:-}";
if [ -z "$jobs" ]; then jobs=$(nproc 2>/dev/null || echo 4); fi;

cfg_args="--prefix=$prefix --pkg-config-flags=--static --arch=x86_64 --target-os=mingw32 --disable-everything --disable-autodetect --disable-doc --disable-debug --disable-programs --disable-avfilter --disable-avdevice --enable-swresample --enable-network --enable-w32threads --disable-pthreads --disable-openssl --disable-gnutls --disable-schannel --disable-zlib --disable-bzlib --disable-lzma --disable-iconv --enable-avutil --enable-avcodec --enable-avformat --enable-swscale --enable-protocol=file --enable-protocol=tcp --enable-protocol=udp --enable-protocol=http --enable-protocol=rtp --enable-demuxer=rtsp --enable-demuxer=sdp --enable-demuxer=rtp --enable-demuxer=mpjpeg --enable-muxer=mpegts --enable-parser=h264 --enable-parser=aac --enable-decoder=h264 --enable-decoder=mjpeg --enable-decoder=aac --enable-decoder=pcm_alaw --enable-decoder=pcm_mulaw --enable-decoder=mp3 --enable-bsf=h264_mp4toannexb --enable-bsf=aac_adtstoasc --enable-small --disable-shared --enable-static";

echo "FF_UPDATE=${FF_UPDATE:-} FF_CLEAN=${FF_CLEAN:-} FF_RECONF=${FF_RECONF:-} FF_INSTALL=${FF_INSTALL:-}";

need_cfg=0;
if [ "${FF_RECONF:-}" = "1" ]; then need_cfg=1; fi;
if [ ! -f ".configure_args" ]; then need_cfg=1; fi;
if [ $need_cfg -eq 0 ] && ! printf "%s" "$cfg_args" | cmp -s - ".configure_args"; then need_cfg=1; fi;

if [ $need_cfg -eq 1 ]; then
  echo "Configuring FFmpeg...";
  printf "%s" "$cfg_args" > ".configure_args";
  "$src/configure" $cfg_args;
else
  echo "Reusing existing FFmpeg configure (pass -Reconfigure to force).";
fi;

# Hard check: ensure muxer is enabled in config (newer FFmpeg writes per-component defines here).
if ! grep -q "CONFIG_MPEGTS_MUXER 1" config_components.h 2>/dev/null; then
  echo "ERROR: CONFIG_MPEGTS_MUXER is not enabled. Configure flags were:";
  echo "$cfg_args";
  exit 2;
fi;

make -j$jobs;

need_install=0;
if [ "${FF_INSTALL:-}" = "1" ]; then need_install=1; fi;
if [ "${FF_CLEAN:-}" = "1" ] || [ "${FF_RECONF:-}" = "1" ] || [ "${FF_UPDATE:-}" = "1" ]; then need_install=1; fi;
if [ ! -f "$prefix/lib/libavformat.a" ]; then need_install=1; fi;

if [ $need_install -eq 1 ]; then
  echo "Installing to prefix...";
  make install;
else
  echo "Skipping install (use -Install to force).";
fi;
'@

Write-Host "Installed minimal FFmpeg to: $Prefix"
Write-Host "Configure CMake with: -DFFMPEG_ROOT=`"$Prefix`""
Write-Host "Tips: re-run without -Clean for incremental builds; use -Update to pull latest; use -Reconfigure if flags changed; use -Install to re-copy to prefix."

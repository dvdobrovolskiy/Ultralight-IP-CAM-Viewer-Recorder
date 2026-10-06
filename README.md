# Ultralight IP CAM Viewer Recorder

A lightweight multi-camera IP camera (RTSP) viewer and recorder for Windows, written in C
on top of **FFmpeg** (libavformat/libavcodec/libswscale/libswresample) and **SDL2**.

- Multi-camera tile view, click a tile to expand it
- ONVIF discovery of stream URLs and PTZ control
- Continuous recording with automatic archive trimming
- Time machine: browse and play back recordings (`F9`)
- Audio playback when the stream carries audio
- Plugin API (Windows DLLs) for video/audio analytics and overlays (`F10`)
- English / Russian UI, system-tray app, single instance

## Build (MSYS2 / pkg-config / Linux)

```sh
cmake -S . -B build
cmake --build build -j
```

## Build (manual include/lib paths, Windows)

```powershell
cmake -S . -B build `
  -DFFMPEG_INCLUDE_DIR="C:\path\to\ffmpeg\include" `
  -DFFMPEG_LIBRARY_DIR="C:\path\to\ffmpeg\lib" `
  -DSDL2_INCLUDE_DIR="C:\path\to\SDL2\include" `
  -DSDL2_LIBRARY_DIR="C:\path\to\SDL2\lib"
cmake --build build -j
```

## Minimal size build (Windows / MSYS2 UCRT64)

To reduce DLL count/size, build a minimal **static** FFmpeg and link it into the app:

- Build minimal FFmpeg (first time): `.\build_ffmpeg_min_ucrt64.ps1 -Install`
- Incremental rebuilds (fast): `.\build_ffmpeg_min_ucrt64.ps1`
- Build the app and copy the runtime files into `dist\`: `.\build_min_portable.ps1`

Or configure manually:

```powershell
cmake -S . -B build -G Ninja -DFFMPEG_ROOT="$PWD\third_party\ffmpeg-min\install"
cmake --build build -j
```

The result is `Ultralight_IP_CAM_Viewer_Recorder.exe` + `SDL2.dll` (+ `libwinpthread-1.dll`
when built with MSYS2 GCC).

Installers: `.\make_installer.ps1` (MSI, needs WiX) and `.\make_msix.ps1` (MSIX).

If you see missing DLL errors (e.g. `avutil-60.dll`), either build once with CMake (it copies
runtime DLLs next to the `.exe`), or run via `run.ps1`, which adds `C:\msys64\ucrt64\bin` to
`PATH` for the current session.

## Run

```sh
Ultralight_IP_CAM_Viewer_Recorder.exe "rtsp://user:password@camera-host:554/stream"
```

Cameras can also be added from the UI. Settings (including camera URLs and credentials) are
stored in `settings.ini` next to the executable — **do not share that file**.

Controls:

- `F1` — help / hints overlay
- `F9` — time machine (recordings playback) for the selected camera
- `F10` — plugins manager
- `Esc` — collapse an expanded tile / close overlays

On Windows the app lives in the system tray: the window's X and minimize buttons hide it to the
tray, a left-click on the tray icon brings it back, and the tray's right-click menu (Show / Exit)
quits. Only one copy runs at a time: launching the exe again brings the running instance back.

## Plugins (Windows DLL)

- Put plugin DLLs into `plugins\` next to the `.exe`.
- Press `F10` to open the Plugins manager (enable/disable, rescan).
- Plugins receive video frames (YUV420P) and optionally audio samples (S16), and can return
  overlay rectangles (e.g. object tracking boxes).
- Plugins are optional; with no plugins present the app works on its own.

Plugin API header: [`src/plugin_api.h`](src/plugin_api.h).
Example plugin: [`examples/plugin_example`](examples/plugin_example).
Your own plugins can live in `plugins\<name>\<name>.c` and be built with
`.\build_plugin_portable.ps1` (the `plugins\` folder is not part of this repository).

## License

Copyright (C) 2026 Dmitriy Dobrovolskiy.

Licensed under the **GNU AGPL v3** — see [`LICENSE`](LICENSE). For commercial use (closed-source
products, bundling, paid services, or any use where you can't comply with the AGPL), a
commercial license is available: contact **Dmitriy Dobrovolskiy** <dima@dobrovolskiy.com>.
Details in [`LICENSING.md`](LICENSING.md).

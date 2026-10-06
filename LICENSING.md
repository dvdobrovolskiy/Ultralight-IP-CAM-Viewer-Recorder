# Licensing

Ultralight IP CAM Viewer Recorder
Copyright (C) 2026 Dmitriy Dobrovolskiy

This project is **dual-licensed**.

## 1. Open-source license: GNU AGPL v3

The source code in this repository is free software: you can redistribute it
and/or modify it under the terms of the **GNU Affero General Public License,
version 3** as published by the Free Software Foundation. The full text is in
[`LICENSE`](LICENSE).

This program is distributed in the hope that it will be useful, but WITHOUT
ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
FOR A PARTICULAR PURPOSE. See the GNU Affero General Public License for more
details.

The AGPL is intended for personal, educational, research and other
non-commercial use, and for open-source projects that are willing to comply
with all of its terms. In short, if you distribute this program or a modified
version of it — **or let users interact with it over a network** — you must
make the complete corresponding source code of your version available under
the AGPL v3 as well.

## 2. Commercial license

If you want to use this software commercially — for example in a proprietary
or closed-source product, bundled with commercial hardware, as part of a paid
service, or in any other way where you cannot or do not want to comply with
the AGPL v3 — you need a separate **commercial license**.

Please contact:

**Dmitriy Dobrovolskiy** — <dima@dobrovolskiy.com>

## Contributions

By submitting a contribution (pull request, patch, etc.) you agree that it is
licensed under the AGPL v3 and that the copyright holder may also distribute
it under the commercial license described above.

## Third-party components

The application links against third-party libraries that keep their own
licenses. They are **not** included in this repository and are fetched/built
separately:

- [FFmpeg](https://ffmpeg.org/) — LGPL v2.1+ (the minimal build produced by
  `build_ffmpeg_min_ucrt64.ps1` does not enable any GPL components)
- [SDL2](https://www.libsdl.org/) — zlib license

Plugins are separate programs and are not part of this repository.

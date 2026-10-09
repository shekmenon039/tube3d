# Third-party notices

Tube3D is free software under the **GNU General Public License v3.0** (`LICENSE`). Its YouTube
request formats were adapted from [FourthTube](https://github.com/erievs/FourthTube)'s
youtube_parser (GPL-3.0).

Tube3D is built with, and its CIA / 3DSX contain, the following software. Their licence texts are
in `licenses/` (or next to the component, as noted).

| Component | Licence | Used for | Text |
|---|---|---|---|
| [FFmpeg](https://ffmpeg.org) 7.1.1 (libavformat, libavcodec, libavutil, libswresample) | LGPL-2.1-or-later | MP4 / Matroska / MPEG-TS demuxing, audio decoding | `licenses/ffmpeg.md`, `licenses/LGPL-2.1.txt` |
| [libsmb2](https://github.com/sahlberg/libsmb2) (51c5910, with `tools/libsmb2-3ds-connect.patch`) | LGPL-2.1-or-later | SMB network places | `licenses/libsmb2.txt`, `licenses/LGPL-2.1.txt` |
| [libssh2](https://libssh2.org) 1.11.1 | BSD-3-Clause | SFTP network places | `licenses/libssh2.txt` |
| [curl / libcurl](https://curl.se) | curl licence (MIT-style) | HTTPS | `licenses/curl.txt` |
| [Mbed TLS](https://www.trustedfirmware.org/projects/mbed-tls/) | Apache-2.0 | TLS | `licenses/mbedtls.txt` |
| [libjpeg-turbo](https://libjpeg-turbo.org) | IJG / BSD-3-Clause / zlib | thumbnails | `licenses/libjpeg-turbo.md` |
| [zlib](https://zlib.net) | zlib | compression (curl) | `licenses/zlib.txt` |
| [RapidJSON](https://rapidjson.org) | MIT | YouTube's JSON | `client/external/rapidjson/license.txt` (git submodule) |
| [libctru](https://github.com/devkitPro/libctru), [citro3d](https://github.com/devkitPro/citro3d), [citro2d](https://github.com/devkitPro/citro2d) | zlib | the 3DS system, graphics | (zlib, as `licenses/zlib.txt`) |
| [Noto Sans](https://fonts.google.com/noto) (pre-rendered into `client/data/ui_font.bin`) | SIL OFL 1.1 | the interface's text | `tools/fonts/OFL-NotoSans.txt` |

This software is based in part on the work of the Independent JPEG Group.

## The LGPL libraries (FFmpeg, libsmb2)

They are linked statically. Their exact sources are the git submodules in `client/external/`
(FFmpeg at release tag n7.1.1, libsmb2 at 51c5910, libssh2 at 1.11.1), and Tube3D's changes to
them are what the build scripts apply: `tools/build_ffmpeg.sh` (the table trim the script
describes) and `tools/build_libsmb2.sh` (`tools/libsmb2-3ds-connect.patch`). To relink Tube3D with
your own build of them, put your libraries in `client/lib/` (or change the submodules and run
`make -C client clean-libs`) and run `make -C client`; Tube3D's own source is all in this
repository.

## Build tools (not shipped)

- The animated HOME banner (`client/cia/banner.cgfx`) is made by `tools/make_banner3d.py` with
  [pycgfx](https://github.com/skyfloogle/pycgfx), which the script fetches at build time; pycgfx
  itself is not part of this repository. The script's HOME-menu safety steps are adapted from
  ClouDS Music FA's `tools/banner/convert_banner_cgfx.py`, under this licence:

  > MIT License
  >
  > Copyright (c) 2026 ClouDS Music contributors
  >
  > Permission is hereby granted, free of charge, to any person obtaining a copy of this software
  > and associated documentation files (the "Software"), to deal in the Software without
  > restriction, including without limitation the rights to use, copy, modify, merge, publish,
  > distribute, sublicense, and/or sell copies of the Software, and to permit persons to whom the
  > Software is furnished to do so, subject to the following conditions:
  >
  > The above copyright notice and this permission notice shall be included in all copies or
  > substantial portions of the Software.
  >
  > THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IMPLIED, INCLUDING
  > BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
  > NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM,
  > DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
  > OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.

- devkitARM, makerom and bannertool build the app; nothing of them is shipped beyond what
  libctru/citro2d/citro3d provide.

YouTube is a trademark of Google LLC. Tube3D is not affiliated with or endorsed by Google or
Nintendo.

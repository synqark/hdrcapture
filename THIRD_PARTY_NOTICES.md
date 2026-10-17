# Third-party notices

`hdrrec.exe` and `hdrrec_gui.exe` are statically linked executables. Besides the hdrrec code
(zlib license, see `LICENSE`) they contain the components below. The full license texts are in
the `LICENSES/` folder, which is included in every release archive.

## FFmpeg — LGPL 2.1 or later

`hdrrec.exe` uses the FFmpeg libraries libavcodec, libavformat and libavutil (version **n8.1.3**),
statically linked.

- License: GNU Lesser General Public License version 2.1 or later — `LICENSES/LGPL-2.1.txt`.
  FFmpeg was configured **without** `--enable-gpl` and `--enable-nonfree`, so no GPL or non-free
  parts are included.
- Copyright: the FFmpeg developers — <https://ffmpeg.org/>
- Source code: <https://github.com/FFmpeg/FFmpeg/tree/n8.1.3>
  (the release page of each hdrrec version also carries the matching FFmpeg source archive).
- Build configuration: `scripts/build_ffmpeg.sh` (exact `configure` options; the prebuilt
  libraries in `third_party/ffmpeg-8.1.3-min-mingw64.tgz` were produced by this script).
- Unmodified: FFmpeg's source code is used as published; hdrrec only calls its public API.

**Relinking with a modified FFmpeg** (LGPL 2.1, section 6): the complete source code of hdrrec is
available in this repository. To use your own FFmpeg build, run `scripts/build_ffmpeg.sh` (or
your modified version of it), which installs into `third_party/ffmpeg-min/`, and then
`scripts/build_hdrrec.sh`, which links `hdrrec.exe` against those libraries.

## nv-codec-headers — MIT

FFmpeg's NVENC support is compiled with nv-codec-headers **n13.0.19.0**
(<https://github.com/FFmpeg/nv-codec-headers>), Copyright (c) 2010-2024 NVIDIA Corporation.
MIT-style license — `LICENSES/nv-codec-headers.txt`.

## mingw-w64 runtime and winpthreads

The executables are built with the MSYS2 MinGW-w64 toolchain and link its runtime statically.

- mingw-w64 runtime — `LICENSES/mingw-w64-runtime.txt`
- winpthreads (Copyright (c) 2011 mingw-w64 project; parts (C) 2010 Lockless Inc.) —
  MIT / BSD-style, `LICENSES/winpthreads.txt`

## GCC runtime libraries

libgcc and libstdc++ are licensed under the GPL version 3 with the GCC Runtime Library Exception
(`LICENSES/GCC-RUNTIME-LIBRARY-EXCEPTION.txt`), which places no license requirements on programs
compiled with GCC.

## Trademarks

NVIDIA and NVENC are trademarks of NVIDIA Corporation. Windows is a trademark of Microsoft
Corporation. hdrrec is not affiliated with or endorsed by them, nor by the OBS Project or the
FFmpeg project.

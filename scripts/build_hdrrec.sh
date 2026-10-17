#!/usr/bin/env bash
# Build build/hdrrec.exe
#
# Run in the MSYS2 "MINGW64" shell (msvcrt runtime; do NOT use UCRT64/CLANG64,
# the bundled static FFmpeg libraries were built against msvcrt).
#   one-time setup:  pacman -S --needed mingw-w64-x86_64-gcc mingw-w64-x86_64-pkgconf tar
#
# Uses the prebuilt minimal static FFmpeg in third_party/ (see scripts/build_ffmpeg.sh to rebuild it).
# Set CXX to override the compiler (e.g. CXX=x86_64-w64-mingw32-g++-posix for Linux cross builds).
set -euo pipefail
cd "$(dirname "$0")/.."

PREFIX="$PWD/third_party/ffmpeg-min"
if [ ! -f "$PREFIX/lib/libavcodec.a" ]; then
    echo "extracting third_party/ffmpeg-8.1.3-min-mingw64.tgz ..."
    mkdir -p "$PREFIX"
    tar xzf third_party/ffmpeg-8.1.3-min-mingw64.tgz -C "$PREFIX"
fi

export PKG_CONFIG_PATH="$PREFIX/lib/pkgconfig"
LIBS=$(pkg-config --static --libs libavformat libavcodec libavutil)
CXX=${CXX:-g++}
STRIP=${STRIP:-strip}

mkdir -p build
"$CXX" -O2 -std=c++17 -Wall -Wno-unused-function -I"$PREFIX/include" \
    src/hdrrec.cpp -o build/hdrrec.exe \
    -static -static-libgcc -static-libstdc++ $LIBS \
    -ld3d11 -ldxgi -ld3dcompiler -lole32 -loleaut32 -luser32 -lshell32 -lwinmm -lmmdevapi
"$STRIP" build/hdrrec.exe
ls -la build/hdrrec.exe

# GUI front-end (tray + small window); runs build/hdrrec.exe from the same folder
WINDRES=${WINDRES:-windres}
"$WINDRES" --include-dir=src -O coff src/hdrrec_gui.rc -o build/hdrrec_gui_res.o
"$CXX" -O2 -std=c++17 -Wall -municode -mwindows \
    src/hdrrec_gui.cpp build/hdrrec_gui_res.o -o build/hdrrec_gui.exe \
    -static -static-libgcc -static-libstdc++ \
    -lcomctl32 -lshell32 -lole32 -luuid -ldxgi -luser32 -lgdi32
"$STRIP" build/hdrrec_gui.exe
ls -la build/hdrrec_gui.exe

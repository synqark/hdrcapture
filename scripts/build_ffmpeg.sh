#!/usr/bin/env bash
# Rebuild the minimal static FFmpeg used by hdrrec into third_party/ffmpeg-min
# (only needed when changing FFmpeg version / enabled components).
#
# Run in the MSYS2 "MINGW64" shell.
#   one-time setup:
#     pacman -S --needed git make nasm diffutils mingw-w64-x86_64-gcc mingw-w64-x86_64-pkgconf
#
# Versions are pinned to what the prototype was verified with:
#   FFmpeg           n8.1.3
#   nv-codec-headers n13.0.19.0  (NVENC API 13.0 -> NVIDIA driver 570+ required)
set -euo pipefail
cd "$(dirname "$0")/.."
ROOT="$PWD"
PREFIX="$ROOT/third_party/ffmpeg-min"
SRC="$ROOT/third_party/src"
mkdir -p "$SRC" "$PREFIX"

cd "$SRC"
[ -d nv-codec-headers ] || git clone --depth 1 --branch n13.0.19.0 https://github.com/FFmpeg/nv-codec-headers.git
[ -d ffmpeg ]           || git clone --depth 1 --branch n8.1.3     https://github.com/FFmpeg/FFmpeg.git ffmpeg

make -C nv-codec-headers PREFIX="$PREFIX" install

cd ffmpeg
PKG_CONFIG_PATH="$PREFIX/lib/pkgconfig" ./configure \
    --prefix="$PREFIX" --arch=x86_64 --target-os=mingw32 --pkg-config=pkg-config \
    --enable-static --disable-shared --disable-programs --disable-doc \
    --disable-everything --disable-autodetect --enable-w32threads \
    --enable-d3d11va --enable-ffnvcodec --enable-nvenc \
    --enable-filter=ddagrab,fps,buffer,buffersink,null,format \
    --enable-encoder=hevc_nvenc,av1_nvenc,aac,pcm_s16le,pcm_f32le \
    --enable-muxer=matroska,mp4 --enable-protocol=file --disable-network
make -j"$(nproc)"
make install

# make the .pc files relocatable
for f in "$PREFIX"/lib/pkgconfig/*.pc; do
    sed -i "s|^prefix=.*|prefix=\${pcfiledir}/../..|; s|$PREFIX|\${prefix}|g" "$f"
done
echo "done: $PREFIX"

#!/usr/bin/env bash
# Package a release into dist/:
#   dist/hdrrec-<version>-win64.zip     executables + README + LICENSE + third-party notices / licenses
#   dist/ffmpeg-n8.1.3-source.tar.gz    the FFmpeg source the executables were built from
#                                       (upload it to the same GitHub release: LGPL source offer)
#
# usage (MSYS2 MINGW64 or Git Bash, after scripts/build_hdrrec.sh):
#   bash scripts/package_release.sh 1.0.0
set -euo pipefail
cd "$(dirname "$0")/.."
VERSION=${1:?usage: package_release.sh VERSION}
FFMPEG_TAG=n8.1.3

for f in build/hdrrec.exe build/hdrrec_gui.exe; do
    [ -f "$f" ] || { echo "missing $f - run scripts/build_hdrrec.sh first" >&2; exit 1; }
done

NAME="hdrrec-$VERSION-win64"
STAGE="dist/$NAME"
rm -rf "$STAGE" "dist/$NAME.zip"
mkdir -p "$STAGE"
cp build/hdrrec.exe build/hdrrec_gui.exe README.md README.ja.md LICENSE THIRD_PARTY_NOTICES.md "$STAGE/"
cp -r LICENSES "$STAGE/"

# zip with PowerShell (available on every Windows; MSYS2 has no zip by default)
(cd dist && powershell.exe -NoProfile -Command "Compress-Archive -Path '$NAME' -DestinationPath '$NAME.zip'")
rm -rf "$STAGE"

SRC="dist/ffmpeg-$FFMPEG_TAG-source.tar.gz"
[ -f "$SRC" ] || curl -sSfL -o "$SRC" "https://github.com/FFmpeg/FFmpeg/archive/refs/tags/$FFMPEG_TAG.tar.gz"

ls -la dist

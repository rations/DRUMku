#!/bin/sh
# make-hpkg.sh — build the `drumku` .hpkg on a running Haiku (x86_64). Builds against
# the VST3-haiku SDK checkout (default ../VST3-haiku/vst3sdk; override VST3_SDK_DIR).
set -e

HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$HERE/.." && pwd)
VERSION=0.1.0
REVISION=1
STAGE="$HERE/stage"
SDK="${VST3_SDK_DIR:-$ROOT/../VST3-haiku/vst3sdk}"

pkgman install -y cmake ninja gcc make || true
[ -d "$SDK" ] || { echo "!! VST3 SDK not found at $SDK (set VST3_SDK_DIR)" >&2; exit 1; }

cd "$ROOT"
rm -rf build "$STAGE"
cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DVST3_SDK_DIR="$SDK"
ninja -C build

BUNDLE=$(find build -type d -name 'DRUMku.vst3' | head -1)
[ -n "$BUNDLE" ] || { echo "!! DRUMku.vst3 not built" >&2; exit 1; }
mkdir -p "$STAGE/add-ons/vst3"
cp -r "$BUNDLE" "$STAGE/add-ons/vst3/"

cp "$HERE/drumku.PackageInfo" "$STAGE/.PackageInfo"
OUT="$HERE/drumku-$VERSION-$REVISION-x86_64.hpkg"
package create -C "$STAGE" "$OUT"
echo ">> built $OUT"

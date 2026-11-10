#!/bin/sh
# make-hpkg.sh — build the `drumku` .hpkg on a running RenkuOS (x86_64): the
# DRUMku.vst3 plug-in and the DRUMku standalone app (with its JACK add-on when
# jack is installed at build time). Builds against the VST3-renku SDK checkout
# (default ../VST3-renku/vst3sdk; override VST3_SDK_DIR).
set -e

HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$HERE/.." && pwd)
VERSION=0.5.0
REVISION=4
STAGE="$HERE/stage"
SDK="${VST3_SDK_DIR:-$ROOT/../VST3-renku/vst3sdk}"

# The system this is built on is the oldest the package may run on (as
# haikuporter does): the version of the installed haiku package.
HAIKU_PKG=$(ls /boot/system/packages/haiku-r*-x86_64.hpkg | head -n 1)
HAIKU_MIN=$(basename "$HAIKU_PKG" | sed -e 's/^haiku-//' -e 's/-[0-9]*-x86_64\.hpkg$//')

pkgman install -y cmake ninja gcc make libsamplerate_devel || true
[ -d "$SDK" ] || { echo "!! VST3 SDK not found at $SDK (set VST3_SDK_DIR)" >&2; exit 1; }

cd "$ROOT"
rm -rf build "$STAGE"
cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DVST3_SDK_DIR="$SDK"
ninja -C build

# The plug-in
BUNDLE=$(find build -type d -name 'DRUMku.vst3' | head -1)
[ -n "$BUNDLE" ] || { echo "!! DRUMku.vst3 not built" >&2; exit 1; }
mkdir -p "$STAGE/add-ons/media/VST3"
cp -r "$BUNDLE" "$STAGE/add-ons/media/VST3/"

# The standalone app, its Deskbar entry, and its JACK add-on (looked for in
# add-ons/DRUMku/; without it the app plays on the Media Kit only)
APP=build/bin/Release/DRUMku
[ -f "$APP" ] || { echo "!! the DRUMku app was not built" >&2; exit 1; }
mkdir -p "$STAGE/apps" "$STAGE/data/deskbar/menu/Applications"
cp "$APP" "$STAGE/apps/DRUMku"
mimeset -f "$STAGE/apps/DRUMku"
ln -s ../../../../apps/DRUMku "$STAGE/data/deskbar/menu/Applications/DRUMku"
if [ -f build/bin/Release/backends/jack.so ]; then
	mkdir -p "$STAGE/add-ons/DRUMku"
	cp build/bin/Release/backends/jack.so "$STAGE/add-ons/DRUMku/jack.so"
else
	echo ">> jack not installed: the package has no JACK add-on (Media Kit only)"
fi

sed -e "s/@HAIKU_MIN@/$HAIKU_MIN/g" "$HERE/drumku.PackageInfo" > "$STAGE/.PackageInfo"
OUT="$HERE/drumku-$VERSION-$REVISION-x86_64.hpkg"
package create -C "$STAGE" "$OUT"
echo ">> built $OUT"

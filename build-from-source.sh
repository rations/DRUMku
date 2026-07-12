#!/bin/sh
# build-from-source.sh — build DRUMku from source and install it for the current user.
# Needs the VST3-haiku SDK checkout (default ../VST3-haiku; override VST3_SDK_DIR).
set -e

HERE=$(cd "$(dirname "$0")" && pwd)
SDK="${VST3_SDK_DIR:-$HERE/../VST3-haiku/vst3sdk}"

pkgman install -y cmake ninja gcc make
[ -d "$SDK" ] || { echo "!! VST3 SDK not found at $SDK (set VST3_SDK_DIR)" >&2; exit 1; }

cd "$HERE"
cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DVST3_SDK_DIR="$SDK"
ninja -C build

mkdir -p "$HOME/config/non-packaged/add-ons/vst3"
cp -r build/VST3/Release/DRUMku.vst3 "$HOME/config/non-packaged/add-ons/vst3/"
echo ">> Installed DRUMku.vst3 to ~/config/non-packaged/add-ons/vst3"

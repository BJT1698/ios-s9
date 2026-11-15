#!/usr/bin/env bash
set -euo pipefail

# Build /System/Library/Extensions/Chlorophyll.bundle (arm64, iOS >= 16.0, ad-hoc signed).
# Usage: op6/metal/plugin/build.sh <libbgair_dylib> <out_bundle_dir>
# Expected to run on macOS with Xcode iphoneos SDK installed (e.g. .github/workflows/op6-mesa.yml).

if [ "$#" -ne 2 ]; then
   echo "usage: $0 <path-to-libbgair.dylib> <out-bundle-dir>" >&2
   exit 1
fi

LIBBGAIR="$1"
OUT_DIR="$2"
HERE="$(cd "$(dirname "$0")" && pwd)"
SDK="${SDK:-$(xcrun --sdk iphoneos --show-sdk-path)}"
CC="${CC:-xcrun --sdk iphoneos clang}"

if [ ! -f "$LIBBGAIR" ]; then
   echo "missing libbgair.dylib at $LIBBGAIR" >&2
   exit 1
fi

# Rename all BG* ObjC classes and C entry points in the bundle copy so processes
# that also load /usr/local/lib/libbgmtl.dylib never hit duplicate class/symbol warnings.
RENAME=""
for c in $(grep -oE '^@interface (BG[A-Za-z0-9_]+)' "$HERE/../src/bgmtl.m" | sed 's/@interface //' | sort -u); do
   RENAME="$RENAME -D$c=CHL_$c"
done
for fn in bgmtl_create_device bgmtl_gpu_state bgmtl_render_info bgmtl_rwa; do
   RENAME="$RENAME -D$fn=chl_$fn"
done

BUNDLE="$OUT_DIR/Chlorophyll.bundle"
rm -rf "$BUNDLE"
mkdir -p "$BUNDLE"

# shellcheck disable=SC2086
$CC -arch arm64 -miphoneos-version-min=16.0 -isysroot "$SDK" \
   -bundle -O1 -g -fno-objc-arc -Wall -Wno-deprecated-declarations \
   -I"$HERE/../../mesa/air" \
   $RENAME \
   "$HERE/chlorophyll_plugin.m" "$LIBBGAIR" \
   -framework Foundation -framework Metal -framework IOSurface -lobjc \
   -Wl,-rpath,/usr/local/lib -Wl,-rpath,@loader_path \
   -o "$BUNDLE/Chlorophyll"

# Allow resolving libbgair.dylib from either /usr/local/lib or @loader_path.
xcrun --sdk iphoneos install_name_tool \
   -change /usr/local/lib/libbgair.dylib @rpath/libbgair.dylib \
   "$BUNDLE/Chlorophyll"

cp "$HERE/Info.plist" "$BUNDLE/Info.plist"
cp "$HERE/op6-mtl-clients" "$OUT_DIR/op6-mtl-clients"
chmod 0755 "$BUNDLE" "$BUNDLE/Chlorophyll"
chmod 0644 "$BUNDLE/Info.plist" "$OUT_DIR/op6-mtl-clients"

codesign -s - -f "$BUNDLE/Chlorophyll"

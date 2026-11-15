#!/usr/bin/env bash
# Build OP6 iOS phone diagnostic tools (lsreg, oslog, exq) into out_dir
# (default: op6/phone-tools/out), ad-hoc signed with NAME-ents.plist when present.
#
# Modelled on ios-flame's phone/tools/build.sh (dipa-studio, MIT License).
# Usage: op6/phone-tools/build.sh [out_dir]
set -euo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"
OUT_DIR="${1:-$HERE/out}"
SDK="${SDK:-$(xcrun --sdk iphoneos --show-sdk-path)}"
CC="${CC:-xcrun --sdk iphoneos clang}"
CFLAGS="${CFLAGS:--arch arm64 -miphoneos-version-min=16.0 -isysroot $SDK -O2 -Wall}"

mkdir -p "$OUT_DIR"

build() {	# NAME SOURCE [FLAGS...]
	local name=$1 src=$2
	shift 2
	# shellcheck disable=SC2086
	$CC $CFLAGS "$@" -o "$OUT_DIR/$name" "$HERE/$src"
	if [ -f "$HERE/$name-ents.plist" ]; then
		codesign -f -s - --entitlements "$HERE/$name-ents.plist" "$OUT_DIR/$name"
		codesign -d --entitlements - "$OUT_DIR/$name"
	else
		codesign -f -s - "$OUT_DIR/$name"
	fi
}

build exq exq.m -fobjc-arc -framework Foundation
build oslog oslog.m -fobjc-arc -framework Foundation
build lsreg lsreg.m -fobjc-arc -framework Foundation -framework MobileCoreServices

ls -l "$OUT_DIR/exq" "$OUT_DIR/oslog" "$OUT_DIR/lsreg"

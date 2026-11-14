#!/bin/bash
# Build Mesa 25.0.7 for OnePlus 6 iOS 16 arm64: libOSMesa.8.dylib (OpenGL ES 2/3 + GL,
# Gallium freedreno on Adreno 630 via SweetGPUUserClient, softpipe fallback).
#
# Usage (on macOS with Xcode iPhoneOS SDK, e.g. GitHub Actions macos-15):
#   op6/mesa/build.sh OUT
#
# Built -O2 -g (meson debugoptimized, NDEBUG kept, nothing stripped by meson). OUT/lib/libOSMesa.8.dylib
# loses only its debug map (strip -S: every function name, static ones too, stays, so .ips frames
# show names); OUT/unstripped/libOSMesa.8.dylib is the linker's output as is, and
# OUT/dSYM/libOSMesa.8.dylib.dSYM its DWARF (same UUID as both), for llvm-symbolizer / atos.
#
# Adapted from HackiOS tools/mk-mesa.sh (Copyright (c) 2026 OOSEX GANG, MIT License)
# and ios-apollo userland/mesa/build.sh (Copyright (c) 2026 Leo Malik / dipa-studio, MIT).
set -euo pipefail

VERSION=25.0.7
SHA256=592272df3cf01e85e7db300c449df5061092574d099da275d19e97ef0510f8a6
here=$(cd "$(dirname "$0")" && pwd)
out=$(mkdir -p "${1:?usage: build.sh OUT}" && cd "$1" && pwd)
work=${MESA_WORK:-$PWD/mesa-work}
mkdir -p "$work"
cd "$work"

[ -f mesa-$VERSION.tar.xz ] || curl -sSL --fail -o mesa-$VERSION.tar.xz https://archive.mesa3d.org/mesa-$VERSION.tar.xz
echo "$SHA256  mesa-$VERSION.tar.xz" | shasum -a 256 -c --quiet
rm -rf mesa-$VERSION build
tar xJf mesa-$VERSION.tar.xz
for p in "$here"/patches/*.patch; do
	echo "patch: ${p##*/}"
	patch -s --no-backup-if-mismatch -d mesa-$VERSION -p1 < "$p"
done
cp -R "$here/sweet-drm" mesa-$VERSION/src/sweet-drm
cp -R "$here/air" mesa-$VERSION/src/compiler/air

if [ ! -x venv/bin/python3 ]; then
	python3 -m venv venv
	venv/bin/pip -q install meson ninja mako pyyaml packaging
fi
export PATH=$work/venv/bin:$PATH
if command -v brew >/dev/null && [ -d "$(brew --prefix bison 2>/dev/null)/bin" ]; then
	export PATH=$(brew --prefix bison)/bin:$PATH
fi
bison --version | head -1

SDK=$(xcrun --sdk iphoneos --show-sdk-path)
CC=$(xcrun --sdk iphoneos -f clang)
CXX=$(xcrun --sdk iphoneos -f clang++)
cat > cross.ini <<INI
[binaries]
c = ['$CC', '-target', 'arm64-apple-ios16.0', '-isysroot', '$SDK']
cpp = ['$CXX', '-target', 'arm64-apple-ios16.0', '-isysroot', '$SDK']
objc = ['$CC', '-target', 'arm64-apple-ios16.0', '-isysroot', '$SDK']
ar = '$(xcrun -f ar)'
strip = '$(xcrun -f strip)'
pkg-config = 'false'

[host_machine]
system = 'darwin'
subsystem = 'ios'
cpu_family = 'aarch64'
cpu = 'arm64'
endian = 'little'

[properties]
needs_exe_wrapper = true
INI

meson setup build mesa-$VERSION --cross-file cross.ini --wrap-mode=nofallback \
	--prefix=/usr/local --libdir=lib --buildtype=debugoptimized -Db_ndebug=true -Dstrip=false -Dsplit-debug=disabled \
	-Dplatforms= -Dgallium-drivers=softpipe,freedreno -Dfreedreno-kmds=msm -Dvulkan-drivers= \
	-Dglx=disabled -Degl=disabled -Dgbm=disabled -Dosmesa=true -Dllvm=disabled -Dshared-glapi=enabled \
	-Dgles1=disabled -Dgles2=enabled -Dopengl=true -Dzstd=disabled -Dxmlconfig=disabled -Dexpat=disabled \
	-Dshader-cache=disabled -Dvalgrind=disabled -Dlibunwind=disabled -Dglvnd=disabled -Dvideo-codecs= \
	-Dtools= -Dgallium-vdpau=disabled -Dgallium-va=disabled -Dgallium-xa=disabled -Dlmsensors=disabled \
	-Dbuild-tests=false
ninja -C build src/gallium/targets/osmesa/libOSMesa.8.dylib src/gallium/drivers/freedreno/libbgair.dylib

mkdir -p "$out/lib" "$out/unstripped" "$out/dSYM"

package_dylib() {
	local built=$1 name=$2 min_dwarf=$3
	local lib=$out/lib/$name
	local unstripped=$out/unstripped/$name
	local dsym=$out/dSYM/$name.dSYM
	# the DWARF stays in the build's .o files: collect it now, while they are there
	rm -rf "$dsym"
	xcrun dsymutil "$built" -o "$dsym"
	cp "$built" "$unstripped"
	xcrun install_name_tool -id "/usr/local/lib/$name" "$unstripped"
	codesign -s - -f "$unstripped"
	xcrun strip -S -o "$lib" "$built"
	xcrun install_name_tool -id "/usr/local/lib/$name" "$lib"
	codesign -s - -f "$lib"
	xcrun dwarfdump --uuid "$built" "$lib" "$dsym"
	local uuids
	uuids=$(xcrun dwarfdump --uuid "$built" "$lib" "$unstripped" "$dsym" |
		awk '{ print $2 }' | sort -u | wc -l)
	[ "$uuids" -eq 1 ] || { echo "build.sh: dSYM UUID does not match $name" >&2; exit 1; }
	local dwarf=$dsym/Contents/Resources/DWARF/$name
	[ "$(stat -f %z "$dwarf")" -gt "$min_dwarf" ] || { echo "build.sh: (almost) no DWARF in $dwarf" >&2; exit 1; }
}

package_dylib build/src/gallium/targets/osmesa/libOSMesa.8.dylib libOSMesa.8.dylib 1000000
package_dylib build/src/gallium/drivers/freedreno/libbgair.dylib libbgair.dylib 200000

lib=$out/lib/libOSMesa.8.dylib
bgair=$out/lib/libbgair.dylib
mkdir -p "$work/meta"
cp mesa-$VERSION/src/mapi/glapi/registry/gl.xml "$work/meta/gl.xml"
nm -gU "$lib" | awk '$3 ~ /^_gl/ { print substr($3, 2) }' > "$work/meta/exported-gl.txt"
echo "build.sh: Mesa $VERSION: $lib, $(stat -f %z "$lib") bytes ($(stat -f %z "$out/unstripped/libOSMesa.8.dylib") unstripped," \
	"$(du -sk "$out/dSYM/libOSMesa.8.dylib.dSYM" | cut -f1) KiB dSYM, $(nm "$lib" | grep -c ' [tT] ') functions named)," \
	"$(nm -gU "$lib" | grep -c ' _gl') gl* functions, SweetGPU named $(strings "$lib" | grep -c '^SweetGPU$') time(s)"
echo "build.sh: libbgair: $bgair, $(stat -f %z "$bgair") bytes ($(stat -f %z "$out/unstripped/libbgair.dylib") unstripped," \
	"$(du -sk "$out/dSYM/libbgair.dylib.dSYM" | cut -f1) KiB dSYM), sweet_drm_* exports: $(nm -gU "$bgair" | grep -c ' _sweet_drm_')"

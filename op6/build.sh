#!/bin/bash
# Build the OnePlus 6 (enchilada) shim and wrap it the way OnePlus ABL
# expects a kernel: boot.img header v0, Image.gz with the DTB appended
# (same layout pmOS uses, see /usr/share/deviceinfo/deviceinfo on the phone).
#   ./build.sh [m1|m2|m8|m5|m7|m10|m11|m12|m13] stage (default m1)
#   m10/m11/m12 embed shim/fw_a630_gmu.h if present (tools/gpu-fw-embed.py; git-ignored)
#   SHIM_CFLAGS=-DGPU_RPMH=1 ./build.sh m11  GPU-RSC wake handshake as mainline (see GPU-NOTES.md)
#   m12 = GX on through the GMU; PDC TCS cmd2 = 0x30080 as Linux unless SHIM_CFLAGS sets GPU_PDC_CMD2,
#   -DM12_OPP=1 boots GX at the lowest OPP instead of Linux's highest
#   m13 = m12 + zap shader via TZ (PAS 13) + CP ringbuffer (ME_INIT, NOP, MEM_WRITE); also embeds
#   shim/fw_a630_sqe.h + fw_a630_zap.h (tools/gpu-fw-embed-cp.py; git-ignored); -DM13_STOP=1 stops after zap auth
#   SHIM_CFLAGS=-DM7_SMMU=0 ./build.sh m7    extra compiler flags (M7: only report the SMMU)
#   PAYLOAD=out/payload.bin ./build.sh m4   XNU payload as the ramdisk
# Test with: fastboot boot out/boot.img   (nothing is flashed)
# Derived from s9/build.sh (ios-s9) and ios-sweet shim/build.sh (MIT, dipa-studio).
# Toolchain: clang + ld.lld (no GNU cross gcc needed).
set -euo pipefail
cd "$(dirname "$0")"
CC="clang --target=aarch64-none-elf"
OBJCOPY=llvm-objcopy
READELF=llvm-readelf
NM=llvm-nm
DTS=${DTS:-dtb/op6-live.dts}
CFLAGS="-O2 -ffreestanding -nostdlib -fno-pic -fno-pie -mgeneral-regs-only -mstrict-align -fno-stack-protector -fno-builtin -fno-jump-tables -Wall -Wextra -Wno-unused-command-line-argument ${SHIM_CFLAGS:-}"

STAGE=${1:-m1}
case $STAGE in
m1) main="m1.c screen.c" ;;
m2) main="m2.c con.c" ;;
m8) main="m8.c con.c plat.c" ;;
m5) main="m5.c con.c plat.c i2c_bb.c rmi4.c rpmh.c" ;;
m7) main="m7.c con.c plat.c i2c_bb.c rpmh.c" ;;
m10) main="m10.c gpu.c con.c plat.c rpmh.c i2c_bb.c" ;;
m11) main="m11.c gpu.c con.c plat.c rpmh.c i2c_bb.c" ;;
m12) main="m12.c gpu.c con.c plat.c rpmh.c i2c_bb.c"
     case "${SHIM_CFLAGS:-}" in *GPU_PDC_CMD2*) ;; *) CFLAGS="$CFLAGS -DGPU_PDC_CMD2=0x30080u" ;; esac ;;
m13) main="m13.c gpu.c con.c plat.c rpmh.c i2c_bb.c"
     case "${SHIM_CFLAGS:-}" in *GPU_PDC_CMD2*) ;; *) CFLAGS="$CFLAGS -DGPU_PDC_CMD2=0x30080u" ;; esac
     # m13.c writes the zap into the DT carve-out: the DT we append must still say 0x8c415000 + 0x2000
     grep -A1 'gpu@8c415000 {' "$DTS" | grep -q 'reg = <0x00 0x8c415000 0x00 0x2000>;' ||
	{ echo "m13: $DTS has no gpu@8c415000 0x2000 zap carve-out (m13.c ZAP_PHYS/ZAP_SIZE)" >&2; exit 1; }
     for h in fw_a630_gmu.h fw_a630_sqe.h fw_a630_zap.h; do
	[ -f "shim/$h" ] || echo "m13: shim/$h missing: the image will only show SEM FIRMWARE" >&2
     done ;;
m4) main="m4.c con.c plat.c loader.c i2c_bb.c rpmh.c"
    [ -f "${PAYLOAD:-}" ] || { echo "m4 needs PAYLOAD=path/to/payload.bin" >&2; exit 2; } ;;
*) echo "usage: build.sh [m1|m2|m8|m5|m7|m10|m11|m12|m13|m4]" >&2; exit 2 ;;
esac

mkdir -p out/obj
rm -f out/obj/*.o
objs=()
for f in start.S $main fb.c mdss.c fdt.c ramlog.c lib.c; do
	o=out/obj/${f%.*}.o
	$CC $CFLAGS -c "shim/$f" -o "$o"
	objs+=("$o")
done
# ABL runs us wherever it loads us: only start.o may hold absolute relocations.
abs=$($READELF -r "${objs[@]:1}" | grep -c R_AARCH64_ABS64 || true)
[ "$abs" = 0 ] || { echo "error: $abs absolute relocations in C objects" >&2; exit 1; }
$CC $CFLAGS -static -fuse-ld=lld -T shim/shim.ld -Wl,--build-id=none -Wl,-N \
	"${objs[@]}" -o out/shim.elf
$OBJCOPY -O binary out/shim.elf out/Image
size=$($NM out/shim.elf | awk '$3=="_end"{print strtonum("0x"$1)}')
truncate -s "$size" out/Image
gzip -9 -n -c out/Image > out/Image.gz

# ABL picks the DTB by qcom,msm-id <0x141 0x20001> / board-id <8 0 0x459b 0x16>.
dtc -q -I dts -O dtb -o out/op6.dtb "$DTS"
cat out/Image.gz out/op6.dtb > out/Image.gz-dtb

if [ "$STAGE" = m4 ]; then
	cp "$PAYLOAD" out/ramdisk.cpio.gz
else
	(cd out && rm -rf rd && mkdir rd && cd rd && touch -d @0 . && find . | cpio -o -H newc --reproducible --quiet | gzip -9 -n > ../ramdisk.cpio.gz)
fi

# Offsets from pmOS deviceinfo for oneplus-enchilada.
mkbootimg --header_version 0 --kernel out/Image.gz-dtb --ramdisk out/ramdisk.cpio.gz \
	--base 0x00000000 --kernel_offset 0x00008000 --ramdisk_offset 0x01000000 \
	--second_offset 0x00f00000 --tags_offset 0x00000100 --pagesize 4096 \
	--cmdline "console=null" -o out/boot.img
cp out/boot.img "out/boot-$STAGE.img"
ls -l out/Image out/boot.img
sha256sum out/boot.img

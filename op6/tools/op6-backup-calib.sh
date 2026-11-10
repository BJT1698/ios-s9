#!/bin/sh
# Back up the OnePlus 6 per-device partitions before userdata is replaced by
# the iOS root. Runs ON THE PHONE under postmarketOS, as root (read-only dd).
#   scp op6/tools/op6-backup-calib.sh lorenzo@172.16.42.1:
#   ssh -t lorenzo@172.16.42.1 sudo sh op6-backup-calib.sh
# Writes /home/lorenzo/op6-backup/<label>.img + SHA256SUMS, owned by lorenzo,
# then fetch with: scp -r lorenzo@172.16.42.1:op6-backup ~/Progetti/enchilada-work/
set -eu
OUT=/home/lorenzo/op6-backup
PARTS="persist modemst1 modemst2 fsg fsc boot_a boot_b dtbo_a dtbo_b vbmeta_a vbmeta_b"
mkdir -p "$OUT"
cd "$OUT"
for p in $PARTS; do
	dev=/dev/disk/by-partlabel/$p
	[ -e "$dev" ] || { echo "missing $dev" >&2; exit 1; }
	dd if="$dev" of="$p.img" bs=1M status=none
	echo "$p: $(readlink -f "$dev") $(stat -c %s "$p.img") bytes"
done
sha256sum *.img > SHA256SUMS
# Device-side checksum straight from the partitions, to prove the copies.
for p in $PARTS; do
	a=$(sha256sum < "/dev/disk/by-partlabel/$p" | cut -d' ' -f1)
	b=$(grep " $p.img\$" SHA256SUMS | cut -d' ' -f1)
	[ "$a" = "$b" ] || { echo "MISMATCH $p" >&2; exit 1; }
done
chown -R lorenzo:lorenzo "$OUT"
echo "OK: $(ls *.img | wc -l) partitions in $OUT, verified against the device"

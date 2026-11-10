#!/bin/bash
# Build the iOS 16.7.16 (iPhone X) root image for the OnePlus 6, milestone B.
# Run from a real terminal (asks for the sudo password several times):
#   ~/Progetti/enchilada-work/ios-op6/op6/tools/op6-mkroot.sh
#   RESUME=1 .../op6-mkroot.sh   reuse an ios16-root.img whose copy finished
# Output: ~/Progetti/enchilada-work/root/ios16-root.img (16 GiB HFS+J) and
#         ios16-root.simg (sparse, for `fastboot flash userdata`).
set -euo pipefail
REPO=$(cd "$(dirname "$0")/../.." && pwd)
DMG=$REPO/../ipsw/dmg
OUT=$REPO/../root
mkdir -p "$OUT"
img=$OUT/ios16-root.img

# Linux fsck_hfs sometimes needs a second pass ("Volume header needs minor
# repair" right after a repair): repair until a read-only check is clean.
fsck_clean() {
	for i in 1 2 3 4; do
		if sudo fsck.hfsplus -f -n "$img" >/dev/null 2>&1; then
			echo "fsck: clean"
			return 0
		fi
		echo "fsck: repair pass $i"
		sudo fsck.hfsplus -f -y "$img" | grep -v 'Illegal name' || true
	done
	sudo fsck.hfsplus -f -n "$img"	# show what is left, fail
}

if [[ ${RESUME:-0} == 1 ]]; then
	[[ -e $img ]] || { echo "RESUME=1 but $img is missing" >&2; exit 1; }
	echo "resuming with existing $img"
else
	[[ -e $img ]] && { echo "$img exists: remove it, or RESUME=1 if its copy finished" >&2; exit 1; }
	# OS / Cryptex1,SystemOS / Cryptex1,AppOS of BuildManifest d22ap Erase.
	# mkiosroot's last read-only fsck may fail on a header nit: fixed below.
	"$REPO/tools/mkiosroot.sh" "$DMG/087-86698-044.dmg" "$DMG/087-86593-047.dmg" \
		"$DMG/087-86715-047.dmg" "$img" 16G || echo "mkiosroot exited $?: checking the image"
fi

fsck_clean
mnt=$(mktemp -d)
sudo mount -t hfsplus -o loop,force "$img" "$mnt"
trap 'mountpoint -q "$mnt" && sudo umount "$mnt"; rmdir "$mnt"' EXIT
# The copy must be complete: dyld shared cache and SpringBoard present.
sudo test -d "$mnt/System/Library/Caches/com.apple.dyld" || { echo "no dyld cache link: copy incomplete" >&2; exit 1; }
sudo test -e "$mnt/System/Library/CoreServices/SpringBoard.app/SpringBoard" || { echo "no SpringBoard: copy incomplete" >&2; exit 1; }
sudo python3 -I "$REPO/tools/sweet-root-tweaks.py" "$mnt"
sync
sudo umount "$mnt"
fsck_clean
sudo chown "$(id -u):$(id -g)" "$img"

img2simg "$img" "$OUT/ios16-root.simg"
sha256sum "$img" "$OUT/ios16-root.simg" | tee "$OUT/SHA256SUMS"
echo "done: $img"

#!/bin/bash
# Build the iOS 16.7.16 (iPhone X) root image for the OnePlus 6, milestone B.
# Run from a real terminal (asks for the sudo password several times):
#   ~/Progetti/enchilada-work/ios-op6/op6/tools/op6-mkroot.sh
# Output: ~/Progetti/enchilada-work/root/ios16-root.img (16 GiB HFS+J) and
#         ios16-root.simg (sparse, for `fastboot flash userdata`).
set -euo pipefail
REPO=$(cd "$(dirname "$0")/../.." && pwd)
DMG=$REPO/../ipsw/dmg
OUT=$REPO/../root
mkdir -p "$OUT"
img=$OUT/ios16-root.img
[[ -e $img ]] && { echo "$img exists: remove it first" >&2; exit 1; }

# OS / Cryptex1,SystemOS / Cryptex1,AppOS of BuildManifest d22ap Erase
"$REPO/tools/mkiosroot.sh" "$DMG/087-86698-044.dmg" "$DMG/087-86593-047.dmg" \
	"$DMG/087-86715-047.dmg" "$img" 16G

mnt=$(mktemp -d)
sudo fsck.hfsplus -fy "$img" >/dev/null || true
sudo mount -t hfsplus -o loop,force "$img" "$mnt"
trap 'mountpoint -q "$mnt" && sudo umount "$mnt"; rmdir "$mnt"' EXIT
sudo python3 -I "$REPO/tools/sweet-root-tweaks.py" "$mnt"
sync
sudo umount "$mnt"
sudo fsck.hfsplus -f -n "$img"
sudo chown "$(id -u):$(id -g)" "$img"

img2simg "$img" "$OUT/ios16-root.simg"
sha256sum "$img" "$OUT/ios16-root.simg" | tee "$OUT/SHA256SUMS"
echo "done: $img"

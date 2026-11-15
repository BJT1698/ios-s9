#!/bin/bash
# Run on the PC with the phone on USB (root SSH). iOS on the OP6 reports a
# 828x1792 @2x screen = logical screen class 414w-896h@2x~iphone, which no
# iPhone X system wallpaper declares (375w-812h@3x, 393w-852h@3x, 430w-932h@3x),
# so WallpaperKit finds "no compatible wallpaper" and the wallpaper stays black.
# 375x812 and 414x896 have the same aspect ratio: clone every 375w-812h@3x
# wallpaper bundle under the OP6 class. Idempotent; undo = rm the clones.
set -euo pipefail
K=${K:-$HOME/.ssh/ios-op6} H=${H:-root@192.168.7.2}
T=$(mktemp -d); trap 'rm -rf "$T"' EXIT
ssh -i "$K" "$H" 'ls -d /Library/Wallpaper/Collections/*/Wallpapers/*-375w-812h@3x~iphone.wallpaper' > "$T/list"
while read -r src; do
	dst=${src//375w-812h@3x/414w-896h@2x}
	ssh -i "$K" "$H" "test -e '$dst'" </dev/null && continue
	scp -q -i "$K" "$H:$src/Wallpaper.plist" "$T/w.plist" </dev/null
	python3 -I - "$T/w.plist" <<'PY'
import plistlib, sys
p = sys.argv[1]; d = plistlib.load(open(p, 'rb'))
assert d['logicalScreenClass'] == '375w-812h@3x~iphone', d.get('logicalScreenClass')
d['logicalScreenClass'] = '414w-896h@2x~iphone'
plistlib.dump(d, open(p, 'wb'), fmt=plistlib.FMT_BINARY)
PY
	ssh -i "$K" "$H" "cp -R '$src' '$dst'" </dev/null
	scp -q -i "$K" "$T/w.plist" "$H:$dst/Wallpaper.plist" </dev/null
	echo "added $dst"
done < "$T/list"

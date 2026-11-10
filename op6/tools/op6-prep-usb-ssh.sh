#!/bin/bash
# OnePlus 6 milestone D: root SSH over the USB cable (CDC-ECM, patch 0106,
# boot-arg sweetusb=2). The Galaxy S9's route (s9/tools/prep-ios6.sh), cut to
# what SSH needs; see op6/USB-NOTES.md. Not run by any other script.
#
#   sudo op6/tools/op6-prep-usb-ssh.sh host          NetworkManager profile op6-usb
#                                                     (PC side 192.168.7.1/24) + NAT hook
#   sudo op6/tools/op6-prep-usb-ssh.sh dev /dev/sdX  the phone's userdata over USB mass
#                                                     storage (pmOS debug shell): fsck,
#                                                     mount, root changes, umount, fsck
#   sudo op6/tools/op6-prep-usb-ssh.sh image IMG     same on an image file (e.g.
#                                                     root/ios16-root.img, then reflash)
#   sudo op6/tools/op6-prep-usb-ssh.sh root MNT      root changes on an HFS+ root already
#                                                     mounted read-write at MNT
#
# Inputs (fetched by the user, never by this script):
#   s9/jailbreak/jb/  from `bash s9/jailbreak/fetch-jailbreak.sh` (palera1n's
#                     Procursus rootful bootstrap 1800, OpenSSH 9.7 debs, s9net, ...)
#                     with SHA256SUMS; JB=dir overrides.
#   the PC's public key: jb/authorized_keys, or PUBKEY=~/.ssh/ios-op6.pub
#
# Root changes (idempotent):
#   - Procursus bootstrap (skipped if /.procursus_strapped or /usr/bin/dpkg exists)
#   - openssh-server/-client/-sftp-server files unpacked now (sshd exists on the
#     first boot) and their debs queued for dpkg on the first boot
#   - s9net (publishes ue0 192.168.7.2 to configd) if jb/s9net exists
#   - /var/root/.ssh/authorized_keys
#   - launchd jobs (launchd_unsecure_cache=1: only System/Library/xpc/launchd.plist
#     counts): com.ios-s9.jbsetup (first boot: prep_bootstrap.sh, root/mobile
#     password alpine, dpkg -i the queued debs), com.ios-s9.s9net,
#     com.openssh.sshd (inetd style, ports 22 and 2222). Job definitions are taken
#     from s9/tools/jb-launchd.py.
#   FULL=1: also all jb debs (ElleKit, Sileo, NewTerm, ...) and all
#     jb-launchd.py jobs + SpringBoard injection. Not the S9's respring script,
#     wallpaper, repos or daemon trim: copy those from s9/tools/prep-ios6.sh.
# Variables go after sudo: sudo PUBKEY=... FULL=1 op6/tools/op6-prep-usb-ssh.sh ...
set -euo pipefail
here=$(cd "$(dirname "$0")/../.." && pwd)
s9=$here/s9
jb=${JB:-$s9/jailbreak/jb}
cmd=${1:-}
[ "$(id -u)" = 0 ] || { echo "run with sudo" >&2; exit 1; }

host_setup() {
	nmcli connection delete op6-usb >/dev/null 2>&1 || true
	if nmcli -g NAME connection show | grep -qx s9-usb; then
		echo "note: profile s9-usb (same MAC) exists; delete it if both fight: nmcli connection delete s9-usb"
	fi
	# Host-side MAC = the device's ECM iMACAddress string 02005E77EE01 (sweet_usb.c).
	nmcli connection add type ethernet con-name op6-usb ethernet.mac-address 02:00:5E:77:EE:01 \
		connection.autoconnect yes ipv4.method manual ipv4.addresses 192.168.7.1/24 \
		ipv4.never-default yes ipv6.method disabled >/dev/null
	cat > /etc/NetworkManager/dispatcher.d/90-ios-op6-share <<'EOF'
#!/bin/sh
# OnePlus 6 iOS over USB (NetworkManager profile op6-usb): NAT this PC's
# connection to the phone (192.168.7.2) while its USB Ethernet is up.
[ "$CONNECTION_ID" = "op6-usb" ] || exit 0
IFACE=$1
rules() {
	iptables -t nat "$1" POSTROUTING -s 192.168.7.0/24 ! -o "$IFACE" -j MASQUERADE
	iptables "$1" FORWARD -i "$IFACE" -j ACCEPT
	iptables "$1" FORWARD -o "$IFACE" -m conntrack --ctstate RELATED,ESTABLISHED -j ACCEPT
}
case "$2" in
up)
	sysctl -qw net.ipv4.ip_forward=1
	rules -D 2>/dev/null
	iptables -t nat -A POSTROUTING -s 192.168.7.0/24 ! -o "$IFACE" -j MASQUERADE
	iptables -I FORWARD 1 -i "$IFACE" -j ACCEPT
	iptables -I FORWARD 1 -o "$IFACE" -m conntrack --ctstate RELATED,ESTABLISHED -j ACCEPT
	;;
down|pre-down)
	rules -D 2>/dev/null
	;;
esac
exit 0
EOF
	chmod 755 /etc/NetworkManager/dispatcher.d/90-ios-op6-share
	systemctl enable --now NetworkManager-dispatcher.service >/dev/null 2>&1 || true
	echo "profile op6-usb + /etc/NetworkManager/dispatcher.d/90-ios-op6-share installed"
}

# Unpack a .deb's data archive into the root (whatever compression it uses).
unpack_deb() {
	local deb=$1 mnt=$2 member
	member=$(ar t "$deb" | grep '^data\.tar' | head -n1)
	case $member in
	data.tar.zst) ar p "$deb" "$member" | zstd -dc ;;
	data.tar.xz) ar p "$deb" "$member" | xz -dc ;;
	data.tar.gz) ar p "$deb" "$member" | gzip -dc ;;
	data.tar.lzma) ar p "$deb" "$member" | xz --format=lzma -dc ;;
	data.tar) ar p "$deb" "$member" ;;
	*) echo "$deb: no data archive" >&2; return 1 ;;
	esac | tar -xp --numeric-owner --keep-directory-symlink -C "$mnt"
}

root_changes() {
	local mnt=$1
	local plist=$mnt/System/Library/xpc/launchd.plist
	[ -f "$plist" ] || { echo "$mnt is not an iOS root (no System/Library/xpc/launchd.plist)" >&2; exit 1; }
	[ -d "$jb" ] || { echo "no $jb: run (as your user) bash s9/jailbreak/fetch-jailbreak.sh" >&2; exit 1; }
	(cd "$jb" && sha256sum -c --quiet SHA256SUMS)
	local keys=$jb/authorized_keys
	[ -n "${PUBKEY:-}" ] && keys=$PUBKEY
	[ -s "$keys" ] || { echo "no public key: jb/authorized_keys or PUBKEY=~/.ssh/ios-op6.pub" >&2; exit 1; }
	local ssh_debs=("$jb"/debs/5-openssh-*.deb)
	[ -f "${ssh_debs[0]}" ] || { echo "no $jb/debs/5-openssh-*.deb" >&2; exit 1; }

	if [ -e "$mnt/.procursus_strapped" ] || [ -e "$mnt/usr/bin/dpkg" ]; then
		echo "bootstrap already present: not unpacked again"
	else
		echo "unpacking the Procursus rootful bootstrap"
		zstd -dc "$jb/bootstrap-rootful-1800.tar.zst" | tar -xp --numeric-owner --keep-directory-symlink -C "$mnt"
	fi
	install -d -m 755 "$mnt/private/var/ios-s9/debs"
	rm -f "$mnt"/private/var/ios-s9/debs/*.deb
	if [ "${FULL:-0}" = 1 ]; then
		ar p "$jb/debs/0-ellekit.deb" data.tar.xz | tar -xJp --numeric-owner --keep-directory-symlink -C "$mnt"
		install -m 644 "$jb"/debs/*.deb "$mnt/private/var/ios-s9/debs/"
	else
		install -m 644 "${ssh_debs[@]}" "$mnt/private/var/ios-s9/debs/"
	fi
	for d in "${ssh_debs[@]}"; do
		echo "unpacking $(basename "$d")"
		unpack_deb "$d" "$mnt"
	done
	rm -f "$mnt/private/var/ios-s9/jbsetup.done"	# a new deb set: run the first-boot job again

	if [ -f "$jb/s9net" ]; then
		install -m 755 "$jb/s9net" "$mnt/usr/libexec/s9net"
	else
		echo "warning: no jb/s9net: ue0 keeps the kernel's 192.168.7.2 but configd does not know it"
	fi
	install -d -m 700 -o 0 -g 0 "$mnt/private/var/root/.ssh"
	install -m 600 -o 0 -g 0 "$keys" "$mnt/private/var/root/.ssh/authorized_keys"

	cat > "$mnt/usr/libexec/ios-s9-jbsetup" <<'EOF'
#!/bin/sh
# OnePlus 6 iOS: first-boot jailbreak setup (launchd job com.ios-s9.jbsetup,
# root), from s9/tools/prep-ios6.sh. Log: /private/var/ios-s9/jbsetup.log
exec >>/private/var/ios-s9/jbsetup.log 2>&1
export PATH=/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin
[ -f /private/var/ios-s9/jbsetup.done ] && exit 0
echo "=== $(date) jbsetup start"
sleep 30
if [ -f /prep_bootstrap.sh ]; then
	NO_PASSWORD_PROMPT=1 /bin/sh /prep_bootstrap.sh
	printf 'alpine\n' | /usr/sbin/pw usermod 501 -h 0
	printf 'alpine\n' | /usr/sbin/pw usermod 0 -h 0
fi
dpkg -i /private/var/ios-s9/debs/*.deb || dpkg -i --force-depends /private/var/ios-s9/debs/*.deb
dpkg --configure -a
[ -x /usr/bin/uicache ] && uicache -a
touch /private/var/ios-s9/jbsetup.done
echo "=== $(date) jbsetup done"
EOF
	chmod 755 "$mnt/usr/libexec/ios-s9-jbsetup"

	if [ "${FULL:-0}" = 1 ]; then
		python3 -I "$s9/tools/jb-launchd.py" "$mnt"
	else
		# Only the jbsetup, s9net and sshd jobs of jb-launchd.py; no SpringBoard change.
		python3 -I - "$s9/tools/jb-launchd.py" "$plist" <<'EOF'
import importlib.util, os, plistlib, sys
spec = importlib.util.spec_from_file_location('jb_launchd', sys.argv[1])
jl = importlib.util.module_from_spec(spec)
spec.loader.exec_module(jl)
path = sys.argv[2]
st = os.stat(path)
with open(path, 'rb') as f:
    plist = plistlib.load(f)
for key, job in jl.JOBS.items():
    if job['Label'] in ('com.ios-s9.jbsetup', 'com.ios-s9.s9net', 'com.openssh.sshd'):
        plist['LaunchDaemons'][key] = job
        print('job', job['Label'])
tmp = path + '.op6tmp'
with open(tmp, 'wb') as f:
    plistlib.dump(plist, f, fmt=plistlib.FMT_BINARY)
os.chown(tmp, st.st_uid, st.st_gid)
os.chmod(tmp, st.st_mode & 0o7777)
os.replace(tmp, path)
EOF
		[ -f "$jb/s9net" ] || echo "note: com.ios-s9.s9net job added; it only respawns (throttled) until /usr/libexec/s9net exists"
	fi
	sync
	echo "root changes done on $mnt"
}

fsck_clean() {	# as op6-mkroot.sh: repair until clean, accept the 540.1 header nit
	local dev=$1 out
	for i in 1 2 3 4; do
		fsck.hfsplus -f -n "$dev" >/dev/null 2>&1 && { echo "fsck: clean"; return 0; }
		echo "fsck: repair pass $i"
		out=$(fsck.hfsplus -f -y "$dev" 2>&1 | grep -v 'Illegal name' || true)
		echo "$out"
		if [[ $i -ge 2 && $out == *"appears to be OK"* && $out != *"Repairing volume"* ]]; then
			return 0
		fi
	done
	fsck.hfsplus -f -n "$dev"
}

on_volume() {	# $1 = block device or image file
	local src=$1 mnt opts=rw,force
	[ -b "$src" ] || [ -f "$src" ] || { echo "$src: no such device or image" >&2; exit 1; }
	[ -f "$src" ] && opts=$opts,loop
	[ "$(blkid -o value -s TYPE "$src" 2>/dev/null)" = hfsplus ] || { echo "$src is not HFS+: refusing" >&2; exit 1; }
	! findmnt -S "$src" >/dev/null 2>&1 || { echo "$src is mounted (udisks?): umount it first" >&2; exit 1; }
	fsck_clean "$src"
	mnt=$(mktemp -d)
	vol_mnt=$mnt
	trap 'mountpoint -q "$vol_mnt" && umount "$vol_mnt"; rmdir "$vol_mnt" 2>/dev/null || true' EXIT
	mount -t hfsplus -o "$opts" "$src" "$mnt"
	root_changes "$mnt"
	umount "$mnt"
	fsck_clean "$src"
	if [ -f "$src" ]; then
		chown "${SUDO_UID:-0}:${SUDO_GID:-0}" "$src"
		echo "image updated: img2simg $src ${src%.img}.simg; then fastboot flash userdata it"
	fi
}

case $cmd in
host) host_setup ;;
root) [ -d "${2:-}" ] || { echo "usage: $0 root MOUNTPOINT" >&2; exit 2; }; root_changes "$2" ;;
dev | image) [ -n "${2:-}" ] || { echo "usage: $0 $cmd PATH" >&2; exit 2; }; on_volume "$2" ;;
*) sed -n '2,36p' "$0"; exit 2 ;;
esac

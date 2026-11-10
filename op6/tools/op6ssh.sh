#!/bin/sh
# Shell (or one command) on the OnePlus 6's iOS over the USB cable:
# root@192.168.7.2 with the key whose .pub op6-prep-usb-ssh.sh installed
# (default ~/.ssh/ios-op6; OP6_KEY=path overrides). As s9/tools/s9ssh.sh.
#   op6/tools/op6ssh.sh [COMMAND ...]
exec ssh -i "${OP6_KEY:-$HOME/.ssh/ios-op6}" -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null \
	-o LogLevel=ERROR -o ConnectTimeout=30 root@192.168.7.2 "$@"

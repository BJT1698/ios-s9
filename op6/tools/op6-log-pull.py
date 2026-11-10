#!/usr/bin/env python3
"""Pull the XNU persistent log ring (last 1025 4-KiB blocks of userdata) from a
OnePlus 6 booted into the pmOS initramfs debug shell:
    fastboot boot op6/out/pmos-debugshell.img      (pmOS kernel, pmos.debug-shell)
    python3 op6/tools/op6-log-pull.py OUT.bin
Read-only: dd from the partition into the initramfs tmpfs, served by busybox
httpd over USB networking (172.16.42.1). Then: s9/tools/sweet-log.py OUT.bin
"""
import hashlib, socket, sys, time, urllib.request

HOST, PORT = '172.16.42.1', 23
out = sys.argv[1] if len(sys.argv) > 1 else 'op6-log.bin'
CMD = ('d=$(readlink -f /dev/disk/by-partlabel/userdata); '
       'b=$(blockdev --getsize64 $d); s=$((b/4096-1025)); '
       'dd if=$d of=/tmp/log.bin bs=4096 skip=$s count=1025 2>/dev/null; '
       'echo "DEV=$d BYTES=$b SKIP=$s"; sha256sum /tmp/log.bin; '
       'busybox-extras httpd -p 8080 -h /tmp; echo PULL-READY\n')

for i in range(60):
    try:
        s = socket.create_connection((HOST, PORT), timeout=5)
        break
    except OSError:
        time.sleep(2)
else:
    sys.exit('no telnet on 172.16.42.1:23 (is the debug-shell image booted?)')

def drain(sock, until, secs=60):
    buf, end = b'', time.time() + secs
    sock.settimeout(1)
    while time.time() < end and until not in buf:
        try:
            d = sock.recv(4096)
        except socket.timeout:
            continue
        if not d:
            break
        # refuse telnet option negotiation (IAC DO/WILL x -> WONT/DONT x)
        i = 0
        while (i := d.find(b'\xff', i)) >= 0 and i + 2 < len(d):
            if d[i + 1] in (0xfb, 0xfd):
                sock.sendall(b'\xff' + (b'\xfe' if d[i + 1] == 0xfb else b'\xfc') + d[i + 2:i + 3])
            i += 3
        buf += d
    return buf.decode('latin-1')

drain(s, b'#', 20)
s.sendall(CMD.encode())
txt = drain(s, b'PULL-READY', 120)
print(txt.strip().splitlines()[-3:] if txt else 'no output')
want = next((w.split()[0] for w in txt.splitlines() if w.endswith('/tmp/log.bin')), None)
data = urllib.request.urlopen(f'http://{HOST}:8080/log.bin', timeout=60).read()
got = hashlib.sha256(data).hexdigest()
open(out, 'wb').write(data)
print(f'{out}: {len(data)} bytes, sha256 {got} {"OK" if got == want else "MISMATCH vs " + str(want)}')

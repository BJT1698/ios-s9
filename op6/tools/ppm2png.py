#!/usr/bin/env python3
"""Convert a binary PPM (P6) to PNG, optionally scaled down, without PIL."""
import struct
import sys
import zlib


def read_ppm(path):
    data = open(path, 'rb').read()
    fields, pos = [], 0
    while len(fields) < 4:
        while data[pos:pos + 1].isspace():
            pos += 1
        end = pos
        while not data[end:end + 1].isspace():
            end += 1
        fields.append(data[pos:end])
        pos = end
    if fields[0] != b'P6' or fields[3] != b'255':
        sys.exit('not an 8-bit P6 PPM')
    return int(fields[1]), int(fields[2]), data[pos + 1:]


def main():
    if len(sys.argv) not in (3, 4):
        sys.exit('usage: ppm2png.py IN.ppm OUT.png [downscale]')
    w, h, px = read_ppm(sys.argv[1])
    k = int(sys.argv[3]) if len(sys.argv) == 4 else 1
    ow, oh = w // k, h // k
    rows = bytearray()
    for y in range(oh):
        rows.append(0)
        base = y * k * w * 3
        for x in range(ow):
            i = base + x * k * 3
            rows += px[i:i + 3]

    def chunk(tag, body):
        return struct.pack('>I', len(body)) + tag + body + struct.pack('>I', zlib.crc32(tag + body))

    png = b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', ow, oh, 8, 2, 0, 0, 0))
    png += chunk(b'IDAT', zlib.compress(bytes(rows), 9)) + chunk(b'IEND', b'')
    open(sys.argv[2], 'wb').write(png)


if __name__ == '__main__':
    main()

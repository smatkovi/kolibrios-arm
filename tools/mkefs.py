#!/usr/bin/env python3
"""The modem's EFS images in one file, for KolibriOS's rmtfs service
(kernel/modem/rmtfs.c, docs/pro1x-modem.md): passed as the kexec initrd.

    mkefs.py OUT NAME=FILE...      e.g. modemst1=/dev/disk/by-partlabel/modemst1

The format: "KOSEFS01", the number of images (u32 LE), for each a name (16
bytes, NUL padded), offset and size (u32 LE each), then the images, 4 KiB
aligned."""
import struct
import sys


def main():
    if len(sys.argv) < 3:
        sys.exit(__doc__)
    out, specs = sys.argv[1], sys.argv[2:]
    images = []
    for s in specs:
        name, _, path = s.partition('=')
        if not path or len(name) > 15:
            sys.exit('mkefs: NAME=FILE, NAME up to 15 characters: ' + s)
        with open(path, 'rb') as f:
            images.append((name, f.read()))
    head = 12 + 24 * len(images)
    off = (head + 4095) & ~4095
    table, body = b'', b''
    for name, data in images:
        table += name.encode().ljust(16, b'\0') + struct.pack('<II', off, len(data))
        pad = (-len(data)) % 4096
        body += data + b'\0' * pad
        off += len(data) + pad
    hdr = b'KOSEFS01' + struct.pack('<I', len(images)) + table
    with open(out, 'wb') as f:
        f.write(hdr + b'\0' * (((head + 4095) & ~4095) - len(hdr)) + body)
    for name, data in images:
        print('mkefs: %s, %d KiB' % (name, len(data) >> 10))


main()

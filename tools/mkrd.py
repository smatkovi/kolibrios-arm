#!/usr/bin/env python3
"""The ramdisk of KolibriOS for ARM (kernel/fs.c reads it):

    "KRD1", u32 count, count x { char name[48]; u32 offset, size; u32 pad[2] },
    then the files, each at a 16-byte boundary

    mkrd.py OUT FILE...   (each file under its own name; NAME=FILE renames)

A name's %20 is a space ("File%20managers/opendial": make's words have
none), as KolibriOS's image has folders with spaces.
"""
import os
import struct
import sys

out, args = sys.argv[1], sys.argv[2:]
entries = []
for a in args:
    name, _, path = a.partition('=') if '=' in a else (os.path.basename(a), '', a)
    name = name.replace('%20', ' ')
    data = open(path, 'rb').read()
    if len(name.encode()) >= 48:
        sys.exit(f'{name}: name too long')
    entries.append((name, data))

head = 8 + 64 * len(entries)
blob = b''
table = b''
off = (head + 15) & ~15
for name, data in entries:
    table += struct.pack('<48sII8x', name.encode(), off, len(data))
    pad = (-len(data)) % 16
    blob += data + b'\0' * pad
    off += len(data) + pad
img = b'KRD1' + struct.pack('<I', len(entries)) + table
img += b'\0' * (((head + 15) & ~15) - len(img)) + blob
open(out, 'wb').write(img)
print(f'{out}: {len(entries)} files, {len(img)} bytes')

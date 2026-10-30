#!/usr/bin/env python3
"""KolibriOS's BMP files as C arrays, at build time (stdlib only):

    kos-bmp.py font IN.bmp OUT.h NAME
        a 1-bit font of 16 x 16 characters (KolibriOS's font*.bmp: the
        glyphs black on white, rows bottom up) as NAME[height][256], each
        row's pixels from bit 0 (the leftmost) on, set = ink, as the
        sources' font.inc macros make them; NAME_W, NAME_H
    kos-bmp.py image IN.bmp OUT.h NAME
        a 24-bit picture as NAME[] of B, G, R bytes, rows top down
        (function 7's format); NAME_W, NAME_H
"""
import struct
import sys

mode, src, out, name = sys.argv[1:5]
d = open(src, 'rb').read()
if d[:2] != b'BM':
    sys.exit(f'{src}: not a BMP file')
off = struct.unpack_from('<I', d, 10)[0]
w, h, planes, bpp = struct.unpack_from('<iiHH', d, 18)
lines = [f'/* {src.split("/")[-1]}, by tools/kos-bmp.py */']
if mode == 'font':
    if bpp != 1 or w % 16 or h % 16:
        sys.exit(f'{src}: not a 1-bit font of 16 x 16 characters')
    fw, fh = w // 16, h // 16
    stride = (w + 31) // 32 * 4
    rows = [[0] * 256 for _ in range(fh)]
    for c in range(256):
        gx, gy = c % 16, c // 16
        for r in range(fh):
            line = h - 1 - (gy * fh + r)          # bottom up
            v = 0
            for x in range(fw):
                px = gx * fw + x
                bit = (d[off + line * stride + px // 8] >> (7 - px % 8)) & 1
                if not bit:
                    v |= 1 << x
            rows[r][c] = v
    t = 'unsigned char' if fw <= 8 else 'unsigned short'
    lines.append(f'#define {name}_W {fw}\n#define {name}_H {fh}')
    lines.append(f'static const {t} {name}[{fh}][256] = {{')
    for r in rows:
        lines.append('\t{' + ','.join(str(v) for v in r) + '},')
    lines.append('};')
elif mode == 'image':
    if bpp != 24:
        sys.exit(f'{src}: not a 24-bit picture')
    stride = (w * 3 + 3) & ~3
    top_down = h < 0
    h = abs(h)
    data = bytearray()
    for y in range(h):
        line = y if top_down else h - 1 - y
        data += d[off + line * stride: off + line * stride + w * 3]
    lines.append(f'#define {name}_W {w}\n#define {name}_H {h}')
    lines.append(f'static const unsigned char {name}[{len(data)}] = {{')
    for i in range(0, len(data), 24):
        lines.append('\t' + ','.join(str(b) for b in data[i:i + 24]) + ',')
    lines.append('};')
else:
    sys.exit('mode: font or image')
open(out, 'w').write('\n'.join(lines) + '\n')

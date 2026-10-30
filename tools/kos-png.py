#!/usr/bin/env python3
"""KolibriOS's PNG pictures as raw pixels, at build time (stdlib only:
zlib and PNG's filters; 8-bit RGB or RGBA, palettes, gray, no
interlacing):

    kos-png.py icon IN.png OUT INDEX SIZE BG
        icon INDEX of a strip of SIZE x SIZE icons (icons32.png: one
        under the other) over the colour BG (0xRRGGBB), as B, G, R bytes
    kos-png.py bgr IN.png OUT [BG]
        the whole picture as B, G, R bytes (alpha over BG, default 0),
        rows top down
"""
import struct
import sys
import zlib


def decode(path):
    d = open(path, 'rb').read()
    if d[:8] != b'\x89PNG\r\n\x1a\n':
        sys.exit(f'{path}: not a PNG file')
    pos, idat, plte, trns = 8, b'', None, None
    while pos < len(d):
        n, kind = struct.unpack('>I4s', d[pos:pos + 8])
        body = d[pos + 8:pos + 8 + n]
        if kind == b'IHDR':
            w, h, depth, ctype, _, _, inter = struct.unpack('>IIBBBBB', body)
        elif kind == b'PLTE':
            plte = body
        elif kind == b'tRNS':
            trns = body
        elif kind == b'IDAT':
            idat += body
        pos += 12 + n
    if inter or depth not in (1, 2, 4, 8):
        sys.exit(f'{path}: interlaced or {depth}-bit PNG not handled')
    chans = {0: 1, 2: 3, 3: 1, 4: 2, 6: 4}[ctype]
    bpp = max(1, chans * depth // 8)
    stride = (w * chans * depth + 7) // 8
    raw = zlib.decompress(idat)
    rows, prev, p = [], bytearray(stride), 0
    for _ in range(h):
        f, line = raw[p], bytearray(raw[p + 1:p + 1 + stride])
        p += 1 + stride
        for i in range(stride):
            a = line[i - bpp] if i >= bpp else 0
            b = prev[i]
            c = prev[i - bpp] if i >= bpp else 0
            if f == 1: line[i] = (line[i] + a) & 255
            elif f == 2: line[i] = (line[i] + b) & 255
            elif f == 3: line[i] = (line[i] + (a + b) // 2) & 255
            elif f == 4:
                pa, pb, pc = abs(b - c), abs(a - c), abs(a + b - 2 * c)
                line[i] = (line[i] + (a if pa <= pb and pa <= pc else b if pb <= pc else c)) & 255
        rows.append(line)
        prev = line
    px = []                                   # RGBA per pixel
    for line in rows:
        out = []
        for x in range(w):
            if depth < 8:
                v = (line[x * depth // 8] >> (8 - depth - (x * depth) % 8)) & ((1 << depth) - 1)
            else:
                v = None
            if ctype == 3:
                i = v if v is not None else line[x]
                r, g, b = plte[3 * i:3 * i + 3]
                a = trns[i] if trns and i < len(trns) else 255
            elif ctype == 0:
                v = v * 255 // ((1 << depth) - 1) if v is not None else line[x]
                r = g = b = v; a = 255
            elif ctype == 4:
                r = g = b = line[2 * x]; a = line[2 * x + 1]
            elif ctype == 2:
                r, g, b = line[3 * x:3 * x + 3]; a = 255
            else:
                r, g, b, a = line[4 * x:4 * x + 4]
            out.append((r, g, b, a))
        px.append(out)
    return w, h, px


def bgr(pixels, bg):
    br, bgg, bb = (bg >> 16) & 255, (bg >> 8) & 255, bg & 255
    o = bytearray()
    for row in pixels:
        for r, g, b, a in row:
            o += bytes(((b * a + bb * (255 - a)) // 255, (g * a + bgg * (255 - a)) // 255,
                        (r * a + br * (255 - a)) // 255))
    return o


mode, src, dst = sys.argv[1:4]
w, h, px = decode(src)
if mode == 'icon':
    index, size, bg = int(sys.argv[4]), int(sys.argv[5]), int(sys.argv[6], 0)
    if (index + 1) * size > h:
        sys.exit(f'{src}: no icon {index}')
    data = bgr([row[:size] for row in px[index * size:(index + 1) * size]], bg)
elif mode == 'bgr':
    data = bgr(px, int(sys.argv[4], 0) if len(sys.argv) > 4 else 0)
else:
    sys.exit('mode: icon or bgr')
open(dst, 'wb').write(data)

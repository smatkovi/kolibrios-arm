#!/usr/bin/env python3
"""A PNG image as KolibriOS's programs have it after libimg's decoding to
24 bits (include_image_file in load_img.inc): 3 bytes a pixel, blue, green,
red, the rows top down, without padding. Alpha is dropped, as libimg's
24-bit conversion drops it. The Python library only (zlib and PNG's row
filters), no PIL, so the build needs nothing more.

    png2bgr.py IN.png OUT.c NAME     a C array: const unsigned char NAME[]
                                     (and NAME_w, NAME_h)
    png2bgr.py IN.png OUT.bgr        the raw bytes

Bit depths 1, 2, 4, 8 (grey and palette) and 8 (RGB, grey with alpha,
RGBA); 16-bit and interlaced images are refused.
"""
import struct
import sys
import zlib


def paeth(a, b, c):
    p = a + b - c
    pa, pb, pc = abs(p - a), abs(p - b), abs(p - c)
    if pa <= pb and pa <= pc:
        return a
    return b if pb <= pc else c


def decode(data):
    if data[:8] != b'\x89PNG\r\n\x1a\n':
        raise ValueError('not a PNG file')
    i, idat, plte = 8, b'', None
    while i < len(data):
        n, = struct.unpack('>I', data[i:i + 4])
        kind, body = data[i + 4:i + 8], data[i + 8:i + 8 + n]
        if kind == b'IHDR':
            w, h, depth, ctype, _, _, interlace = struct.unpack('>IIBBBBB', body)
        elif kind == b'PLTE':
            plte = body
        elif kind == b'IDAT':
            idat += body
        elif kind == b'IEND':
            break
        i += 12 + n
    if interlace:
        raise ValueError('interlaced PNG images are not supported')
    channels = {0: 1, 2: 3, 3: 1, 4: 2, 6: 4}[ctype]
    if depth == 16 or (depth < 8 and ctype not in (0, 3)):
        raise ValueError(f'bit depth {depth} of colour type {ctype} is not supported')
    bits = depth * channels
    stride = (w * bits + 7) // 8
    bpp = max(1, bits // 8)
    raw = zlib.decompress(idat)
    rows, prev, pos = [], bytearray(stride), 0
    for _ in range(h):
        f = raw[pos]
        cur = bytearray(raw[pos + 1:pos + 1 + stride])
        pos += 1 + stride
        for x in range(stride):
            a = cur[x - bpp] if x >= bpp else 0
            b = prev[x]
            c = prev[x - bpp] if x >= bpp else 0
            if f == 1:
                cur[x] = (cur[x] + a) & 255
            elif f == 2:
                cur[x] = (cur[x] + b) & 255
            elif f == 3:
                cur[x] = (cur[x] + (a + b) // 2) & 255
            elif f == 4:
                cur[x] = (cur[x] + paeth(a, b, c)) & 255
            elif f != 0:
                raise ValueError(f'unknown row filter {f}')
        rows.append(cur)
        prev = cur
    out = bytearray()
    for row in rows:
        for x in range(w):
            if depth < 8:
                per = 8 // depth
                v = row[x // per] >> (8 - depth * (x % per + 1)) & ((1 << depth) - 1)
            else:
                v = None
            if ctype == 3:
                k = (v if v is not None else row[x]) * 3
                r, g, b = plte[k], plte[k + 1], plte[k + 2]
            elif ctype == 0:
                g = v * 255 // ((1 << depth) - 1) if v is not None else row[x]
                r = b = g
            elif ctype == 4:
                r = g = b = row[2 * x]
            else:
                r, g, b = row[channels * x:channels * x + 3]
            out += bytes((b, g, r))
    return w, h, out


def main():
    src, dst = sys.argv[1], sys.argv[2]
    w, h, pix = decode(open(src, 'rb').read())
    if dst.endswith('.c'):
        name = sys.argv[3]
        lines = [f'/* {src}: {w}x{h}, B G R (tools/png2bgr.py) */',
                 f'const unsigned {name}_w = {w}, {name}_h = {h};',
                 f'const unsigned char {name}[{len(pix)}] = {{']
        for k in range(0, len(pix), 24):
            lines.append('\t' + ','.join(str(v) for v in pix[k:k + 24]) + ',')
        lines.append('};')
        open(dst, 'w').write('\n'.join(lines) + '\n')
    else:
        open(dst, 'wb').write(pix)


if __name__ == '__main__':
    main()

#!/usr/bin/env python3
"""kfar's test archives for the ramdisk (apps/kfar/app.mk), made at build
time with Python's own modules, no tools and no downloads:

    mkarc.py zip OUT.zip     zipfile: stored and deflated files, folders
                             (one only in the names: kfar_arc's pseudo-folder)
    mkarc.py 7z OUT.7z       a 7z archive written here (lzma's raw LZMA1 and
                             BCJ x86 filters): LZMA, LZMA after BCJ, Copy,
                             an empty file and folders, CRCs, times

The contents are fixed (text from a fixed generator, fixed times), so the
archives are the same on every build; tests/apps/run.py and tests/kfar_arc
know what is in them (CONTENTS below).
"""
import binascii
import io
import lzma
import struct
import sys
import zipfile


def big_text(n):
    """n bytes of lines of words (a fixed pseudo-random sequence): long
    enough for Deflate's and LZMA's windows to be used across 64 KiB"""
    words = ['kolibri', 'far', 'archive', 'plugin', 'arm', 'deflate', 'lzma', 'panel', 'folder',
             'file', 'stream', 'window', 'huffman', 'literal', 'match', 'distance', 'length']
    x, out, line = 12345, [], 0
    while sum(map(len, out)) < n:
        line += 1
        ws = []
        for _ in range(8):
            x = (x * 1103515245 + 12345) & 0x7fffffff
            ws.append(words[x >> 16 & 0xf] if x & 0x100 else str(x % 1000))
        out.append(f'{line:05d} ' + ' '.join(ws) + '\r\n')
    return ''.join(out).encode()[:n]


T = (2026, 10, 30, 12, 34, 56)
CONTENTS = [
    # name, data, method (zip)
    ('readme.txt', b'kfar_arc on KolibriOS for ARM: a stored file.\r\n', zipfile.ZIP_STORED),
    ('big.txt', big_text(150000), zipfile.ZIP_DEFLATED),
    ('sub/', None, None),
    ('sub/inner.txt', b'Hello from inside the archive\r\n' * 3, zipfile.ZIP_DEFLATED),
    ('sub/deeper/note.txt', b'A note in a folder that is only in the names.\r\n', zipfile.ZIP_DEFLATED),
    ('empty/', None, None),
]


def make_zip(path):
    buf = io.BytesIO()
    with zipfile.ZipFile(buf, 'w') as z:
        for name, data, method in CONTENTS:
            zi = zipfile.ZipInfo(name, T)
            if data is None:
                zi.external_attr = 0o40755 << 16 | 0x10
                z.writestr(zi, b'')
            else:
                zi.compress_type = method
                zi.external_attr = 0o100644 << 16 | 0x20
                z.writestr(zi, data)
    open(path, 'wb').write(buf.getvalue())


# ---- 7z (7-Zip's 7zFormat.txt) ----

def num(n):
    """7z's NUMBER: the first byte's leading 1 bits say how many follow"""
    for k in range(8):
        if n < 1 << (7 * (k + 1)):
            first = (0xff << (8 - k)) & 0xff
            rest = n >> (8 * k)
            return bytes([first | rest]) + (n & ((1 << (8 * k)) - 1)).to_bytes(k, 'little')
    return b'\xff' + n.to_bytes(8, 'little')


def bits(v):
    out = bytearray((len(v) + 7) // 8)
    for i, b in enumerate(v):
        if b:
            out[i // 8] |= 0x80 >> (i % 8)
    return bytes(out)


def filetime(t):
    import calendar
    s = calendar.timegm(t + (0, 0, 0))
    return (s + 11644473600) * 10000000


FILES7Z = [
    # name, data, folder kind: 'lzma', 'bcj', 'copy' (one folder each)
    ('readme.txt', b'kfar_arc on KolibriOS for ARM: 7z with LZMA.\r\n', 'lzma'),
    ('big.txt', big_text(150000), 'lzma'),
    ('sub/inner.txt', b'Hello from inside the 7z archive\r\n' * 3, 'lzma'),
    ('sub/code.bin', bytes(range(256)) * 16 + b'\xe8\x10\x00\x00\x00\xe9\x20\x00\x00\x00' * 200, 'bcj'),
    ('stored.txt', b'A file of the 7z archive that is only copied.\r\n', 'copy'),
    ('sub/empty.txt', b'', None),
    ('sub', None, None),
    ('emptydir', None, None),
]
LZMA_DICT = 1 << 16


def lzma_coder():
    props = lzma._encode_filter_properties({'id': lzma.FILTER_LZMA1, 'dict_size': LZMA_DICT})
    return props, b'\x03\x01\x01'


def make_7z(path):
    packed, folders = [], []
    groups = {}
    for name, data, kind in FILES7Z:
        if data:
            groups.setdefault(kind, []).append(data)
    for kind in ('lzma', 'bcj', 'copy'):
        if kind not in groups:
            continue
        data = b''.join(groups[kind])
        props, lid = lzma_coder()
        if kind == 'copy':
            packed.append(data)
            folders.append(([(b'\x00', None)], [], [len(data)], data))
            continue
        filters = [{'id': lzma.FILTER_LZMA1, 'dict_size': LZMA_DICT}]
        if kind == 'bcj':
            # packing runs BCJ first, then LZMA; the folder's coder 0 is
            # BCJ (its output the file), coder 1 LZMA, BCJ's input bound
            # to LZMA's output (in 0 <- out 1), the packed stream LZMA's
            packed.append(lzma.compress(data, format=lzma.FORMAT_RAW, filters=[{'id': lzma.FILTER_X86}] + filters))
            folders.append(([(b'\x03\x03\x01\x03', None), (lid, props)], [(0, 1)], [len(data), len(data)], data))
            continue
        packed.append(lzma.compress(data, format=lzma.FORMAT_RAW, filters=filters))
        folders.append(([(lid, props)], [], [len(data)], data))
    h = bytearray(b'\x01\x04')                  # kHeader, kMainStreamsInfo
    # kPackInfo
    h += b'\x06' + num(0) + num(len(packed)) + b'\x09' + b''.join(num(len(p)) for p in packed) + b'\x00'
    # kUnPackInfo
    h += b'\x07\x0b' + num(len(folders)) + b'\x00'
    for coders, bind, sizes, data in folders:
        h += num(len(coders))
        for cid, props in coders:
            flag = len(cid) | (0x20 if props else 0)
            h += bytes([flag]) + cid
            if props:
                h += num(len(props)) + props
        for inidx, outidx in bind:
            h += num(inidx) + num(outidx)
    h += b'\x0c' + b''.join(num(s) for f in folders for s in f[2])
    h += b'\x0a\x01' + b''.join(struct.pack('<I', binascii.crc32(f[3])) for f in folders)
    h += b'\x00'
    # kSubStreamsInfo: the files of each folder, their sizes and CRCs
    h += b'\x08\x0d'
    for kind in ('lzma', 'bcj', 'copy'):
        if kind in groups:
            h += num(len(groups[kind]))
    h += b'\x09'
    for kind in ('lzma', 'bcj', 'copy'):
        if kind in groups:
            h += b''.join(num(len(d)) for d in groups[kind][:-1])
    crcs = [binascii.crc32(d) for kind in ('lzma', 'bcj', 'copy') if kind in groups
            for d in groups[kind] if len(groups[kind]) > 1]
    if crcs:
        h += b'\x0a\x01' + b''.join(struct.pack('<I', c) for c in crcs)
    h += b'\x00\x00'
    # kFilesInfo: the files with data first, in the order of the folders
    order = [f for kind in ('lzma', 'bcj', 'copy') for f in FILES7Z if f[2] == kind and f[1]]
    order += [f for f in FILES7Z if not f[1]]
    h += b'\x05' + num(len(order))
    empty = [not f[1] for f in order]
    v = bits(empty)
    h += b'\x0e' + num(len(v)) + v
    efile = [f[1] is not None for f in order if not f[1]]
    v = bits(efile)
    h += b'\x0f' + num(len(v)) + v
    names = b'\x00' + b''.join(f[0].encode('utf-16-le') + b'\x00\x00' for f in order)
    h += b'\x11' + num(len(names)) + names
    tm = b'\x01\x00' + b''.join(struct.pack('<Q', filetime(T)) for _ in order)
    h += b'\x14' + num(len(tm)) + tm
    at = b'\x01\x00' + b''.join(struct.pack('<I', 0x10 if f[1] is None else 0x20) for f in order)
    h += b'\x15' + num(len(at)) + at
    h += b'\x00\x00'
    body = b''.join(packed)
    start = struct.pack('<QQI', len(body), len(h), binascii.crc32(h))
    sig = b'7z\xbc\xaf\x27\x1c\x00\x04' + struct.pack('<I', binascii.crc32(start)) + start
    open(path, 'wb').write(sig + body + bytes(h))


if __name__ == '__main__':
    if sys.argv[1] == 'zip':
        make_zip(sys.argv[2])
    elif sys.argv[1] == '7z':
        make_7z(sys.argv[2])
    else:
        sys.exit(__doc__)

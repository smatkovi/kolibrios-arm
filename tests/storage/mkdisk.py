#!/usr/bin/env python3
"""tests/storage: disk images like the phones' storage, for QEMU, without root.

  - "pro1x": a GPT disk of 4096-byte blocks (the Pro1X's UFS LUN 0): small
    partitions without a file system and a small ext4 one ("persist"), then
    entry 13, "userdata": ext4 with a tree of files and the container
    (kolibri/kolibri.img in two pieces, immutable, kolibri/kolibri.map);
  - "n950": an MBR disk of 512-byte blocks (the N950's eMMC): p1 MyDocs
    (FAT32, files, optionally the container), p2 Harmattan's root (ext4 with
    Harmattan's features), p4 Leste's (ext4, optionally the container).

The container is made as tools/kolibri-home makes it (its header and map
functions are used), placed into the ext4 image with debugfs (its extents
set by hand, the blocks marked used, e2fsck's counts fixed, then checked
clean) since mkfs.ext4 -d would leave holes for its zero blocks; into FAT
with mtools, its cluster chain read back by a FAT walker of its own here.

    mkdisk.py pro1x OUT.img [--tamper KIND] [--fat32]
    mkdisk.py n950 OUT.img [--home mydocs|leste] [--tamper KIND]

KIND: shift (a range moved by 4 KiB), size, id, mutable (not immutable /
not read only and system), uuid (another header), none. The manifest of
the files (path size crc32, as storetest tree prints them) goes to
OUT.img.manifest, the container's ranges on the disk (bytes) to
OUT.img.ranges.
"""
import binascii
import importlib.machinery
import importlib.util
import os
import random
import shutil
import struct
import subprocess
import sys
import uuid as uuidlib

sys.dont_write_bytecode = True

TOP = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
_loader = importlib.machinery.SourceFileLoader("kolibri_home", os.path.join(TOP, "tools", "kolibri-home"))
_spec = importlib.util.spec_from_loader("kolibri_home", _loader)
kh = importlib.util.module_from_spec(_spec)
_loader.exec_module(kh)

MiB = 1 << 20
ENV = dict(os.environ, MTOOLS_SKIP_CHECK="1", LANG="C.UTF-8", LC_ALL="C.UTF-8")


def run(*a, **k):
    return subprocess.run(a, check=True, capture_output=True, text=True, env=ENV, **k).stdout


def pattern(n, seed, at=0):
    """storetest's pattern()"""
    out = bytearray(n)
    for i in range(n):
        x = ((at + i) * 2654435761 ^ seed * 40503) & 0xffffffff
        out[i] = (x >> 13 ^ x >> 5) & 0xff
    return bytes(out)


# --- the files -----------------------------------------------------------------

def make_tree(d, seed, big=False):
    """A tree of files with known contents; returns {path: (size, crc)}"""
    rnd = random.Random(seed)
    files = {
        "readme.txt": b"KolibriOS storage test\n",
        "empty": b"",
        "one": b"1",
        "docs/4095.bin": bytes(rnd.getrandbits(8) for _ in range(4095)),
        "docs/4096.bin": bytes(rnd.getrandbits(8) for _ in range(4096)),
        "docs/4097.bin": bytes(rnd.getrandbits(8) for _ in range(4097)),
        "docs/deep/er/still/file.txt": b"deep\n",
        "docs/Grüße ш.txt".encode().decode(): b"utf-8 name\n",
        "music/A long file name with spaces.mp3": os.urandom(0) + bytes(range(256)) * 300,
    }
    if big:
        files["video/big.bin"] = random.Random(seed + 1).randbytes(5 * MiB + 123)
    for i in range(60):
        files["many/f%02d.txt" % i] = ("file %d\n" % i).encode()
    for p, data in files.items():
        full = os.path.join(d, p)
        os.makedirs(os.path.dirname(full), exist_ok=True)
        with open(full, "wb") as f:
            f.write(data)
    return {p: (len(b), binascii.crc32(b)) for p, b in files.items()}


# --- the container --------------------------------------------------------------

def make_container(path, size, uid):
    """As kolibri-home create makes it: zeros, FAT inside, the header"""
    with open(path, "wb") as f:
        f.truncate(size)
    fat = "32" if size >= (260 << 20) else "16"
    run("mkfs.fat", "-S", "4096", "-s", "1", "-R", "32", "-F", fat, "-n", "KOLIBRI", "-i", uid[:4].hex(), path)
    with open(path, "r+b") as f:
        f.seek(kh.HDR_OFF)
        f.write(kh.header(uid, size))


def tamper_map(kind, fs, ident, size, uid, ranges):
    if kind == "shift":
        ranges = [(ranges[0][0] + 4096, ranges[0][1])] + ranges[1:]
    elif kind == "size":
        size += 4096
    elif kind == "id":
        ident += 1
    elif kind == "uuid":
        uid = uuidlib.uuid4().bytes
    return kh.map_text(fs, ident, size, uid, ranges)


# --- ext4 ----------------------------------------------------------------------

def free_ranges(img):
    """ext4's free blocks as (first, count), from dumpe2fs"""
    out = run("dumpe2fs", img)
    rs = []
    for line in out.splitlines():
        line = line.strip()
        if not line.startswith("Free blocks: "):
            continue
        for part in line[len("Free blocks: "):].split(","):
            part = part.strip()
            if not part:
                continue
            a, _, b = part.partition("-")
            a = int(a)
            b = int(b) if b else a
            if rs and rs[-1][0] + rs[-1][1] == a:          # on across a group's end
                rs[-1] = (rs[-1][0], rs[-1][1] + b - a + 1)
            else:
                rs.append((a, b - a + 1))
    return rs


def ext4_part(img, size, src, features=None, home=None, tamper="none", label=None):
    """An ext4 image of the tree src; home: the container's size (in two
    pieces, from two block groups). Returns the container's ranges
    (bytes in the partition) or None."""
    if home:
        os.makedirs(os.path.join(src, "kolibri"), exist_ok=True)
    with open(img, "wb") as f:
        f.truncate(size)
    args = ["mkfs.ext4", "-q", "-F", "-b", "4096", "-d", src]
    if label:
        args += ["-L", label]
    if features:
        args += ["-O", features]
    run(*args, img)
    if not home:
        return None
    uid = uuidlib.uuid4().bytes
    cont = img + ".container"
    make_container(cont, home, uid)
    nb = home // 4096
    # in pieces, apart (at least two; at most 30000 blocks each: an extent
    # holds 32768, a block group 32768), from the free ranges in order
    n = max(2, -(-nb // 30000))
    need = [nb // n + (1 if i < nb % n else 0) for i in range(n)]
    fr = free_ranges(img)
    pieces, at = [], 0
    for first, count in fr:
        pos, end = first + 32, first + count
        while at < n and pos + need[at] <= end:
            pieces.append((pos, need[at]))
            pos += need[at] + 257              # a gap: the pieces apart
            at += 1
    if at < n or n > 4:
        raise SystemExit("no room for the container in %s" % img)
    run("debugfs", "-w", "-R", "write /dev/null kolibri/kolibri.img", img)
    cmds = []
    words = [0xF30A | len(pieces) << 16, 4, 0]
    logical = 0
    for first, count in pieces:
        if count > 32768:
            raise SystemExit("a piece too long for one extent")
        cmds.append("setb %d %d" % (first, count))
        words += [logical, count, first]
        logical += count
    for i, w in enumerate(words):
        cmds.append("sif kolibri/kolibri.img block[%d] %d" % (i, w))
    cmds += ["sif kolibri/kolibri.img size %d" % home, "sif kolibri/kolibri.img blocks %d" % (home // 512),
             "sif kolibri/kolibri.img flags 0x%x" % (0x80000 if tamper == "mutable" else 0x80010)]
    subprocess.run(["debugfs", "-w", "-f", "-", img], input="\n".join(cmds) + "\n", check=True,
                   capture_output=True, text=True)
    subprocess.run(["e2fsck", "-fy", img], capture_output=True)       # the free counts
    r = subprocess.run(["e2fsck", "-fn", img], capture_output=True, text=True, env=ENV)
    if r.returncode:
        raise SystemExit("e2fsck after placing the container:\n" + r.stdout)
    with open(cont, "rb") as c, open(img, "r+b") as f:
        for first, count in pieces:
            f.seek(first * 4096)
            f.write(c.read(count * 4096))
    os.unlink(cont)
    ino = int(run("debugfs", "-R", "stat kolibri/kolibri.img", img).split("Inode:")[1].split()[0])
    ranges = [(first * 4096, count * 4096) for first, count in pieces]
    mapf = img + ".map"
    with open(mapf, "w") as f:
        f.write(tamper_map(tamper, "ext4", ino, home, uid, ranges))
    run("debugfs", "-w", "-R", "write %s kolibri/kolibri.map" % mapf, img)
    os.unlink(mapf)
    return ranges


# --- FAT -----------------------------------------------------------------------

def fat_geometry(img):
    with open(img, "rb") as f:
        bs = f.read(512)
    bps, spc = struct.unpack_from("<HB", bs, 11)
    rsvd, nfats = struct.unpack_from("<HB", bs, 14)
    rootents = struct.unpack_from("<H", bs, 17)[0]
    total = struct.unpack_from("<H", bs, 19)[0] or struct.unpack_from("<I", bs, 32)[0]
    fatsz = struct.unpack_from("<H", bs, 22)[0] or struct.unpack_from("<I", bs, 36)[0]
    data = rsvd + nfats * fatsz + (rootents * 32 + bps - 1) // bps
    clusters = (total - data) // spc
    kind = 12 if clusters < 4085 else 16 if clusters < 65525 else 32
    return dict(bps=bps, spc=spc, fat=rsvd * bps, data=data * bps, kind=kind,
                root=struct.unpack_from("<I", bs, 44)[0] if kind == 32 else None,
                rootdir=(rsvd + nfats * fatsz) * bps)


def fat_entry(f, g, c):
    if g["kind"] == 32:
        f.seek(g["fat"] + 4 * c)
        return struct.unpack("<I", f.read(4))[0] & 0x0fffffff
    f.seek(g["fat"] + 2 * c)
    return struct.unpack("<H", f.read(2))[0]


def fat_chain(img, first):
    """the cluster chain from first, as byte ranges in the partition"""
    g = fat_geometry(img)
    csize = g["bps"] * g["spc"]
    out, c, seen = [], first, set()
    end = 0x0ffffff8 if g["kind"] == 32 else 0xfff8
    with open(img, "rb") as f:
        while 2 <= c < end and c not in seen:
            seen.add(c)
            off = g["data"] + (c - 2) * csize
            if out and out[-1][0] + out[-1][1] == off:
                out[-1] = (out[-1][0], out[-1][1] + csize)
            else:
                out.append((off, csize))
            c = fat_entry(f, g, c)
    return out


def fat_first_cluster(img, path):
    """the first cluster of a file in the root's folder path (8.3 or long)"""
    g = fat_geometry(img)
    csize = g["bps"] * g["spc"]
    parts = path.upper().split("/")
    with open(img, "rb") as f:
        def entries(cluster):
            if cluster is None:                                  # FAT16's root
                f.seek(g["rootdir"])
                data = f.read(g["data"] - g["rootdir"])
            else:
                data = b""
                for off, ln in fat_chain(img, cluster):
                    f.seek(off)
                    data += f.read(ln)
            for i in range(0, len(data), 32):
                e = data[i:i + 32]
                if e[0] == 0:
                    break
                if e[0] == 0xe5 or e[11] == 0x0f:
                    continue
                name = e[:8].decode("latin-1").rstrip()
                ext = e[8:11].decode("latin-1").rstrip()
                c = struct.unpack_from("<H", e, 26)[0] | struct.unpack_from("<H", e, 20)[0] << 16
                yield (name + ("." + ext if ext else "")), c, struct.unpack_from("<I", e, 28)[0]
        cur = g["root"] if g["kind"] == 32 else None
        for k, p in enumerate(parts):
            for name, c, size in entries(cur):
                if name == p:
                    cur = c
                    break
            else:
                raise SystemExit("%s: %s not found" % (img, path))
    return cur


def fat_part(img, size, src, fat="32", home=None, tamper="none", label="MYDOCS"):
    with open(img, "wb") as f:
        f.truncate(size)
    run("mkfs.fat", "-F", fat, "-S", "512", "-n", label, img)
    for name in sorted(os.listdir(src)):
        run("mcopy", "-s", "-i", img, os.path.join(src, name), "::/")
    if not home:
        return None
    uid = uuidlib.uuid4().bytes
    cont = img + ".container"
    make_container(cont, home, uid)
    run("mmd", "-i", img, "::/KOLIBRI")
    run("mcopy", "-i", img, cont, "::/KOLIBRI/KOLIBRI.IMG")
    os.unlink(cont)
    if tamper != "mutable":
        run("mattrib", "-i", img, "+r", "+s", "::/KOLIBRI/KOLIBRI.IMG")
    first = fat_first_cluster(img, "KOLIBRI/KOLIBRI.IMG")
    ranges = fat_chain(img, first)
    tot = 0
    for i, (o, l) in enumerate(ranges):                   # trimmed to the size
        if tot + l > home:
            ranges[i] = (o, home - tot)
            ranges = ranges[:i + 1]
            break
        tot += l
    mapf = img + ".map"
    with open(mapf, "w") as f:
        f.write(tamper_map(tamper, "fat", first, home, uid, ranges))
    run("mcopy", "-i", img, mapf, "::/KOLIBRI/KOLIBRI.MAP")
    os.unlink(mapf)
    r = subprocess.run(["fsck.fat", "-n", img], capture_output=True, text=True)
    if r.returncode:
        raise SystemExit("fsck.fat after the container:\n" + r.stdout)
    return ranges


# --- partition tables -------------------------------------------------------------

def guid_bytes(s):
    return uuidlib.UUID(s).bytes_le


LINUX_DATA = "0fc63daf-8483-4772-8e79-3d69d8477de4"
BASIC_DATA = "ebd0a0a2-b9e5-4433-87c0-68b6b72699c7"


def gpt_disk(out, parts, bs):
    """parts: [(name, image or None, size)] in entry order; 1 MiB aligned"""
    lba = lambda b: b // bs
    at = MiB
    layout = []
    for name, img, size in parts:
        layout.append((name, img, at, size))
        at += (size + MiB - 1) // MiB * MiB
    total = at + MiB
    n_ent = 128
    ents = bytearray(n_ent * 128)
    for i, (name, img, start, size) in enumerate(layout):
        e = bytearray(128)
        e[0:16] = guid_bytes(LINUX_DATA)
        e[16:32] = uuidlib.uuid4().bytes_le
        struct.pack_into("<QQQ", e, 32, lba(start), lba(start + size) - 1, 0)
        e[56:56 + 72] = name.encode("utf-16-le")[:72].ljust(72, b"\0")
        ents[i * 128:(i + 1) * 128] = e
    ecrc = binascii.crc32(ents)
    nb = total // bs
    ent_blocks = len(ents) // bs + (1 if len(ents) % bs else 0)

    def header(cur, other, ent_lba):
        h = bytearray(92)
        h[0:8] = b"EFI PART"
        struct.pack_into("<IIIIQQQQ", h, 8, 0x10000, 92, 0, 0, cur, other, 2 + ent_blocks, nb - 2 - ent_blocks)
        h[56:72] = uuidlib.uuid4().bytes_le
        struct.pack_into("<QIII", h, 72, ent_lba, n_ent, 128, ecrc)
        struct.pack_into("<I", h, 16, binascii.crc32(bytes(h)))
        return bytes(h).ljust(bs, b"\0")

    with open(out, "wb") as f:
        f.truncate(total)
        mbr = bytearray(512)
        struct.pack_into("<BBBBBBBBII", mbr, 446, 0, 0, 2, 0, 0xee, 0xff, 0xff, 0xff, 1, min(nb - 1, 0xffffffff))
        mbr[510:512] = b"\x55\xaa"
        f.seek(0)
        f.write(mbr)
        f.seek(bs)
        f.write(header(1, nb - 1, 2))
        f.seek(2 * bs)
        f.write(ents)
        f.seek((nb - 1 - ent_blocks) * bs)
        f.write(ents)
        f.seek((nb - 1) * bs)
        f.write(header(nb - 1, 1, nb - 1 - ent_blocks))
        for name, img, start, size in layout:
            if img:
                with open(img, "rb") as s:
                    f.seek(start)
                    shutil.copyfileobj(s, f, 4 * MiB)
    return {name: start for name, img, start, size in layout}


def mbr_disk(out, parts):
    """parts: [(number 1-4, type, image, size)]; 512-byte sectors"""
    at = MiB
    starts = {}
    mbr = bytearray(512)
    for num, typ, img, size in parts:
        starts[num] = at
        struct.pack_into("<BBBBBBBBII", mbr, 446 + 16 * (num - 1), 0, 0, 0, 0, typ, 0, 0, 0, at // 512, size // 512)
        at += (size + MiB - 1) // MiB * MiB
    mbr[510:512] = b"\x55\xaa"
    size = 1 << (at + MiB - 1).bit_length()        # QEMU's eMMC: a power of 2
    with open(out, "wb") as f:
        f.truncate(size)
        f.write(mbr)
        for num, typ, img, size in parts:
            with open(img, "rb") as s:
                f.seek(starts[num])
                shutil.copyfileobj(s, f, 4 * MiB)
    return starts


# --- the disks ------------------------------------------------------------------

def manifest(out, trees):
    with open(out + ".manifest", "w") as f:
        for vol, files in trees:
            for p, (size, crc) in sorted(files.items()):
                f.write("%s %s %d %08x\n" % (vol, p, size, crc & 0xffffffff))


def pro1x(out, tamper="none", fat32=False):
    w = out + ".d"
    shutil.rmtree(w, ignore_errors=True)
    os.makedirs(w)
    tree_u, tree_p = os.path.join(w, "userdata"), os.path.join(w, "persist")
    files_u = make_tree(tree_u, 1, big=True)
    files_p = make_tree(tree_p, 2)
    home = (270 if fat32 else 40) * MiB
    ranges = ext4_part(w + "/userdata.img", (home + 64 * MiB) // MiB * MiB, tree_u, home=home,
                       tamper=tamper, label="userdata")
    ext4_part(w + "/persist.img", 16 * MiB, tree_p, label="persist")
    names = ["ssd", "persist", "misc", "keystore", "frp", "super", "recovery_a", "recovery_b",
             "splitboot", "metadata", "logfs", "splash"]
    parts = [(n, w + "/persist.img" if n == "persist" else None, 16 * MiB if n == "persist" else MiB)
             for n in names]
    parts.append(("userdata", w + "/userdata.img", os.path.getsize(w + "/userdata.img")))
    starts = gpt_disk(out, parts, 4096)
    manifest(out, [("ufs0/13", files_u), ("ufs0/2", files_p)])
    with open(out + ".ranges", "w") as f:
        for o, l in ranges:
            f.write("%d %d\n" % (starts["userdata"] + o, l))
    shutil.rmtree(w)


def n950(out, home="leste", tamper="none"):
    w = out + ".d"
    shutil.rmtree(w, ignore_errors=True)
    os.makedirs(w)
    t1, t2, t4 = (os.path.join(w, n) for n in ("mydocs", "harmattan", "leste"))
    f1, f2, f4 = make_tree(t1, 3, big=True), make_tree(t2, 4), make_tree(t4, 5)
    hsize = 40 * MiB
    r1 = fat_part(w + "/p1.img", 96 * MiB, t1, home=hsize if home == "mydocs" else None, tamper=tamper)
    harm = "none,has_journal,ext_attr,resize_inode,dir_index,filetype,extent,flex_bg,sparse_super," \
           "large_file,huge_file,uninit_bg,dir_nlink,extra_isize"
    ext4_part(w + "/p2.img", 24 * MiB, t2, features=harm)
    r4 = ext4_part(w + "/p4.img", 80 * MiB, t4, features=harm, home=hsize if home == "leste" else None,
                   tamper=tamper)
    starts = mbr_disk(out, [(1, 0x0c, w + "/p1.img", 96 * MiB), (2, 0x83, w + "/p2.img", 24 * MiB),
                            (4, 0x83, w + "/p4.img", 80 * MiB)])
    manifest(out, [("mmc0/1", f1), ("mmc0/2", f2), ("mmc0/4", f4)])
    with open(out + ".ranges", "w") as f:
        for num, rs in ((1, r1), (4, r4)):
            for o, l in rs or []:
                f.write("%d %d\n" % (starts[num] + o, l))
    shutil.rmtree(w)


def main():
    import argparse
    ap = argparse.ArgumentParser()
    ap.add_argument("kind", choices=("pro1x", "n950"))
    ap.add_argument("out")
    ap.add_argument("--tamper", default="none", choices=("none", "shift", "size", "id", "mutable", "uuid"))
    ap.add_argument("--home", default="leste", choices=("leste", "mydocs"))
    ap.add_argument("--fat32", action="store_true")
    a = ap.parse_args()
    if a.kind == "pro1x":
        pro1x(a.out, a.tamper, a.fat32)
    else:
        n950(a.out, a.home, a.tamper)


if __name__ == "__main__":
    main()

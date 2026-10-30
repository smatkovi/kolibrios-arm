#!/usr/bin/env python3
"""kernel/storage/fat.c on the PC: FAT12/16/32 images made by mkfs.fat and
filled by mtools, read and written through fat_test (fat_test.c: a shell
over an image), checked against mtools (mdir, mcopy, mattrib, mshowfat),
fsck.fat and a model. tests/storage/fat.mk runs it:

    fat_test.py FAT_TEST_BINARY [quick]

The parts: read (every file byte for byte, listings against mdir's), read
only (not one write), write (phases, fsck and mtools after each), stress
(seeded, against a model, the disk filled), crash (a reset after every
write of a sequence of operations, fsck after each), map (against the FAT
and mshowfat), fuzz (broken images). Images in /tmp/kolibri-fat-test."""
import os
import random
import re
import shutil
import struct
import subprocess
import sys

WORK = '/tmp/kolibri-fat-test'
BIN = os.path.abspath(sys.argv[1])
QUICK = 'quick' in sys.argv[2:]
ENV = dict(os.environ, MTOOLS_SKIP_CHECK='1', LC_ALL='C.UTF-8', LANG='C.UTF-8',
           ASAN_OPTIONS='detect_leaks=1:abort_on_error=1', UBSAN_OPTIONS='halt_on_error=1:print_stacktrace=1')
MB = 1 << 20
E_OK, E_NOT_FOUND, E_DISK_FULL, E_FS, E_ACCESS = 0, 5, 8, 9, 10
A_RO, A_HIDDEN, A_SYSTEM, A_DIR, A_ARCH = 1, 2, 4, 0x10, 0x20

fails = 0
counts = {}


def fail(msg):
    global fails
    fails += 1
    print('FAIL:', msg, flush=True)
    if fails > 40:
        sys.exit('too many failures')


def count(what, n=1):
    counts[what] = counts.get(what, 0) + n


def run(args, **kw):
    return subprocess.run(args, env=ENV, capture_output=True, **kw)


class Shell:
    def __init__(self):
        self.p = subprocess.Popen([BIN], stdin=subprocess.PIPE, stdout=subprocess.PIPE, env=ENV)
        self.log = []

    def cmd(self, *fields):
        line = '\t'.join(str(x) for x in fields)
        self.p.stdin.write(line.encode('utf-8', 'surrogateescape') + b'\n')
        self.p.stdin.flush()
        entries = []
        while True:
            raw = self.p.stdout.readline()
            if not raw:
                sys.exit('fat_test ended at: %r (exit %s)' % (line, self.p.wait()))
            text = raw.decode('utf-8', 'surrogateescape').rstrip('\n')
            if text.startswith('r\t'):
                f = text.split('\t')
                return int(f[1]), f[2:], entries
            if text.startswith('e\t'):
                entries.append(text.split('\t')[1:])
            elif text.startswith('FAIL'):
                fail(text)
            else:
                self.log.append(text)

    def mount(self, img, rw, bsize=512):
        e, f, _ = self.cmd('mount', img, 'rw' if rw else 'ro', bsize)
        return e, (f[0] if f else '')

    def write(self, path, off, data, rewrite):
        with open(WORK + '/data.bin', 'wb') as fp:
            fp.write(data)
        e, f, _ = self.cmd('write', path, off, int(rewrite), WORK + '/data.bin')
        return e, int(f[0])

    def info(self, path):
        e, f, _ = self.cmd('info', path)
        return e, (int(f[0]), int(f[1])) if not e else None

    def get(self, path):
        e, f, _ = self.cmd('get', path, WORK + '/got.bin')
        if e:
            return e, None
        with open(WORK + '/got.bin', 'rb') as fp:
            return e, fp.read()

    def tree(self):
        e, f, ents = self.cmd('tree')
        return e, {x[0]: (int(x[1]), int(x[2])) for x in ents}

    def writes(self):
        return int(self.cmd('stats')[1][0])

    def close(self):
        self.cmd('quit') if False else None
        self.p.stdin.close()
        if self.p.wait():
            fail('fat_test exited with %d' % self.p.returncode)


# --- images -------------------------------------------------------------------

def mkimg(img, size, fat, S, s, label='KOLIBRI', extra=()):
    if os.path.exists(img):
        os.unlink(img)
    with open(img, 'wb') as fp:
        fp.truncate(size)
    r = run(['mkfs.fat', '-F', str(fat), '-S', str(S), '-s', str(s), '-n', label] + list(extra) + [img])
    if r.returncode or b'Not enough clusters' in r.stdout + r.stderr:
        sys.exit('mkfs.fat %s: %s' % (img, (r.stdout + r.stderr).decode()))


def mtools(cmd, img, *args, check=True):
    r = run([cmd, '-i', img] + list(args))
    if check and r.returncode:
        fail('%s %s: %s' % (cmd, ' '.join(args), (r.stdout + r.stderr).decode(errors='replace')))
    return r


def fsck(img):
    r = run(['fsck.fat', '-n', '-v', img])
    return r.returncode, r.stdout.decode(errors='replace') + r.stderr.decode(errors='replace')


def fsck_clean(img, what):
    rc, out = fsck(img)
    count('fsck runs')
    if rc:
        fail('%s: fsck.fat finds errors:\n%s' % (what, '\n'.join(fsck_problems(out))))
        return False
    return True


def fsck_problems(out):
    """what fsck.fat -n -v reports past its description of the boot sector"""
    lines = out.split('\n')
    i = next((k for k, l in enumerate(lines) if 'sectors total' in l), -1)
    probs = []
    for l in lines[i + 1:]:
        s = l.strip()
        if not s or s.startswith('Checking') or s.startswith('Leaving filesystem') or \
                re.match(r'^\S.*: \d+ files, \d+/\d+ clusters$', s):
            continue
        probs.append(l)
    return probs


def bpb(img):
    with open(img, 'rb') as fp:
        b = fp.read(512)
    bps, spc, rsvd, nfats, rootent = struct.unpack_from('<HBHBH', b, 11)
    tot = struct.unpack_from('<H', b, 19)[0] or struct.unpack_from('<I', b, 32)[0]
    fatsz = struct.unpack_from('<H', b, 22)[0] or struct.unpack_from('<I', b, 36)[0]
    rootsec = (rootent * 32 + bps - 1) // bps
    meta = rsvd + nfats * fatsz + rootsec
    ncl = (tot - meta) // spc
    t = 12 if ncl < 4085 else 16 if ncl < 65525 else 32
    return dict(bps=bps, csize=bps * spc, fat_off=rsvd * bps, data_off=meta * bps, nclus=ncl, type=t,
                fat_bytes=fatsz * bps, nfats=nfats,
                root_cl=struct.unpack_from('<I', b, 44)[0] if t == 32 else 0)


def fat_entry(fp, g, c):
    t = g['type']
    if t == 12:
        fp.seek(g['fat_off'] + c + c // 2)
        w = struct.unpack('<H', fp.read(2))[0]
        return w >> 4 if c & 1 else w & 0xfff
    if t == 16:
        fp.seek(g['fat_off'] + 2 * c)
        return struct.unpack('<H', fp.read(2))[0]
    fp.seek(g['fat_off'] + 4 * c)
    return struct.unpack('<I', fp.read(4))[0] & 0x0fffffff


def chain_extents(img, first, size):
    """the file's bytes on the volume as fat.c's map gives them, from the FAT"""
    g = bpb(img)
    eoc = {12: 0xff8, 16: 0xfff8, 32: 0x0ffffff8}[g['type']]
    ext, c, left = [], first, size
    with open(img, 'rb') as fp:
        while left > 0:
            off, n = g['data_off'] + (c - 2) * g['csize'], min(left, g['csize'])
            if ext and ext[-1][0] + ext[-1][1] == off:
                ext[-1][1] += n
            else:
                ext.append([off, n])
            left -= n
            if left > 0:
                c = fat_entry(fp, g, c)
                if c < 2 or c >= eoc:
                    return None
    return [tuple(x) for x in ext]


# --- a model of the files ------------------------------------------------------

def key(name):
    return name.upper()


class Node:
    def __init__(self, name, isdir, data=b'', attr=None):
        self.name, self.isdir = name, isdir
        self.kids = {} if isdir else None
        self.data = bytearray(data)
        self.attr = attr if attr is not None else (A_DIR if isdir else A_ARCH)


class Model:
    def __init__(self):
        self.root = Node('', True)

    def find(self, path):
        n = self.root
        if not path:
            return n
        for c in path.split('/'):
            if not n.isdir:
                return None
            n = n.kids.get(key(c))
            if n is None:
                return None
        return n

    def parent(self, path):
        i = path.rfind('/')
        return (self.find(path[:i]) if i >= 0 else self.root), path[i + 1:]

    def walk(self, n=None, prefix=''):
        n = n or self.root
        for k in sorted(n.kids):
            c = n.kids[k]
            p = prefix + c.name
            yield p, c
            if c.isdir:
                yield from self.walk(c, p + '/')


def name_ok(name):
    if not name or len(name.encode('utf-16-le')) // 2 > 255:
        return False
    if any(ord(c) < 0x20 or c in '"*/:<>?\\|' for c in name):
        return False
    return name[-1] not in '. '


def m_write(m, path, off, data, rewrite):
    """what fat.c should answer, and the model changed (E_DISK_FULL: left to the caller)"""
    par, name = m.parent(path)
    n = m.find(path)
    if n is None:
        if not rewrite or par is None or not par.isdir or not name_ok(name):
            return E_NOT_FOUND
        n = par.kids[key(name)] = Node(name, False)
    elif n.isdir:
        return E_ACCESS
    if rewrite:
        n.data = bytearray(),
        n.data = bytearray()
        off = 0
    if off > len(n.data):
        n.data += bytes(off - len(n.data))
    n.data[off:off + len(data)] = data
    n.attr |= A_ARCH
    return E_OK


def compare_tree(sh, m, what):
    e, t = sh.tree()
    if e:
        fail('%s: tree: %d' % (what, e))
        return
    want = {p: (c.attr if c.isdir else c.attr, 0 if c.isdir else len(c.data)) for p, c in m.walk()}
    if t != want:
        extra = sorted(set(t) - set(want))[:5]
        miss = sorted(set(want) - set(t))[:5]
        diff = [(p, t[p], want[p]) for p in sorted(set(t) & set(want)) if t[p] != want[p]][:5]
        fail('%s: listing differs: extra %s missing %s differing %s' % (what, extra, miss, diff))
    for p, c in m.walk():
        if not c.isdir:
            e, d = sh.get(p)
            if e or d != bytes(c.data):
                fail('%s: %s read %s (%s bytes, %d wanted)' % (what, p, e, None if d is None else len(d), len(c.data)))
            count('files compared')


def mtools_compare(img, m, what):
    """mtools sees what the model has: names, sizes, bytes, attributes"""
    r = mtools('mdir', img, '-/', '-a', '-b', '::/', check=False)
    got = set()
    for l in r.stdout.decode('utf-8', 'surrogateescape').split('\n'):
        if l.startswith('::/'):
            got.add(l[3:].rstrip('/'))
    want = {p for p, c in m.walk()}
    if got != want:
        fail('%s: mdir differs: extra %s missing %s' % (what, sorted(got - want)[:5], sorted(want - got)[:5]))
    out = WORK + '/out'
    shutil.rmtree(out, ignore_errors=True)
    os.makedirs(out)
    if m.root.kids:
        r = mtools('mcopy', img, '-s', '-n', '-m', '-b', '::/*', out + '/', check=False)
        if r.returncode:
            fail('%s: mcopy out: %s' % (what, r.stderr.decode(errors='replace')))
    for p, c in m.walk():
        q = os.path.join(out, p)
        if c.isdir:
            if not os.path.isdir(q):
                fail('%s: mtools: %s not a folder' % (what, p))
        else:
            try:
                with open(q, 'rb') as fp:
                    d = fp.read()
            except OSError:
                d = None
            if d != bytes(c.data):
                fail('%s: mtools: %s differs (%s bytes, %d wanted)' % (what, p, None if d is None else len(d), len(c.data)))
    shutil.rmtree(out, ignore_errors=True)
    r = mtools('mattrib', img, '-/', '::/', check=False)
    for l in r.stdout.decode('utf-8', 'surrogateescape').split('\n'):
        if len(l) < 14 or not l[13:].startswith('::/') or l[13:] == '::/':
            continue
        p = l[16:]
        c = m.find(p)
        if c is None:
            fail('%s: mattrib: %s unknown' % (what, p))
            continue
        a = (A_ARCH if l[2] == 'A' else 0) | (A_SYSTEM if l[5] == 'S' else 0) | (A_HIDDEN if l[6] == 'H' else 0) | \
            (A_RO if l[7] == 'R' else 0)
        if a != c.attr & ~A_DIR:
            fail('%s: mattrib %s: %x, %x wanted' % (what, p, a, c.attr & ~A_DIR))
    count('mtools compares')


# --- 1, 2: read, and read only ----------------------------------------------------

def make_sources():
    rnd = random.Random(1)
    trees = {}
    for kind in ('full', 'small'):
        src = '%s/src-%s' % (WORK, kind)
        shutil.rmtree(src, ignore_errors=True)
        files = {}

        def add(p, data):
            files[p] = data
            q = os.path.join(src, p)
            os.makedirs(os.path.dirname(q), exist_ok=True)
            with open(q, 'wb') as fp:
                fp.write(data)
        for n in (0, 1, 511, 512, 4095, 4096, 4097):
            add('sizes/s%d.bin' % n, rnd.randbytes(n))
        add('Long Name ä.txt', b'hello')
        add('шрифт файл.txt', 'привет'.encode())
        add('€uro sign.dat', rnd.randbytes(100))
        add('lower.txt', b'lower')
        add('UPPER.TXT', b'upper')
        add('mixed.TXT', b'mixed flags')
        add('MiXeD Case Name.Text', b'mixed')
        add('A' * 120 + '.long', b'long')
        add('n' * 240 + '.255', b'max')
        add('.hidden', b'dot')
        add('Dir1/Sub Two/Deep 3/four/file.txt', b'deep')
        add('Case1/readme.txt', b'one')
        add('Case2/README.TXT', b'two')
        add('Case3/ReadMe.Txt', b'three')
        many = 1500 if kind == 'full' else 300
        for i in range(many):
            add('Many/%s' % ('f%04d.txt' % i if i % 3 else 'Long file name number %04d.bin' % i),
                rnd.randbytes(1 + i % 7) if i % 10 == 1 else b'')
        if kind == 'full':
            add('big/two.bin', rnd.randbytes(2 * MB + 123))
            add('big/thirty.bin', rnd.randbytes(30 * MB + 7))
        else:
            add('big/one.bin', rnd.randbytes(300 * 1024 + 5))
        os.makedirs(src + '/Empty Folder')
        trees[kind] = (src, files)
    return trees


def read_test(trees):
    configs = [  # type, sector, sectors a cluster, image bytes, tree
        (12, 512, 1, 2 * MB, 'small'), (12, 4096, 1, 15 * MB, 'small'), (12, 512, 64, 48 * MB, 'full'),
        (12, 4096, 8, 48 * MB, 'full'), (16, 512, 2, 48 * MB, 'full'), (16, 1024, 4, 100 * MB, 'full'),
        (16, 4096, 1, 64 * MB, 'full'), (16, 512, 64, 160 * MB, 'full'), (16, 2048, 1, 20 * MB, 'small'),
        (32, 512, 1, 48 * MB, 'full'), (32, 512, 8, 300 * MB, 'full'), (32, 4096, 1, 300 * MB, 'full'),
        (32, 4096, 8, 2100 * MB, 'full'), (32, 512, 64, 2100 * MB, 'full'), (32, 2048, 2, 300 * MB, 'full'),
    ]
    if QUICK:
        configs = [configs[0], configs[4], configs[9]]
    img = WORK + '/read.img'
    for t, S, s, size, kind in configs:
        src, files = trees[kind]
        what = 'read FAT%d %d/%d' % (t, S, S * s)
        mkimg(img, size, t, S, s, 'READ%d' % t)
        args = [os.path.join(src, x) for x in sorted(os.listdir(src))]
        mtools('mcopy', img, '-s', '-Q', *args, '::/')
        mtools('mattrib', img, '+r', '+s', '::/lower.txt')
        mtools('mattrib', img, '+h', '::/.hidden')
        sh = Shell()
        e, label = sh.mount(img, False, 4096 if S == 4096 else 512)
        typ = [l for l in sh.log if l.startswith('fat: ')]
        if e or not typ or ('FAT%d,' % t) not in typ[-1] or label != 'READ%d' % t:
            fail('%s: mount %d %s %r' % (what, e, typ, label))
            sh.close()
            continue
        # the listing: the sources' names, mdir's names
        e, tr = sh.tree()
        want = {}
        for root, dirs, fs in os.walk(src):
            for d in dirs:
                want[os.path.relpath(os.path.join(root, d), src)] = (A_DIR, 0)
            for x in fs:
                p = os.path.relpath(os.path.join(root, x), src)
                want[p] = (A_ARCH | (A_RO | A_SYSTEM if p == 'lower.txt' else A_HIDDEN if p == '.hidden' else 0),
                           len(files[p]))
        if e or tr != want:
            fail('%s: listing: %d extra %s missing %s differing %s' % (
                what, e, sorted(set(tr) - set(want))[:4], sorted(set(want) - set(tr))[:4],
                [(p, tr[p], want[p]) for p in set(tr) & set(want) if tr[p] != want[p]][:4]))
        r = mtools('mdir', img, '-/', '-a', '-b', '::/', check=False)
        md = {l[3:].rstrip('/') for l in r.stdout.decode('utf-8', 'surrogateescape').split('\n') if l.startswith('::/')}
        if md != set(tr):
            fail('%s: mdir %d names, ours %d: %s' % (what, len(md), len(tr), sorted(md ^ set(tr))[:4]))
        count('entries listed', len(tr))
        for p, data in files.items():
            e, d = sh.get(p)
            if e or d != data:
                fail('%s: %s: %d, %s bytes of %d' % (what, p, e, None if d is None else len(d), len(data)))
            count('files read')
        # names without regard to case, short names, paging
        for p, want_size in (('case1/README.TXT', 3), ('CASE2/readme.txt', 3), ('case3/readme.TXT', 5),
                             ('LONG NAME Ä.TXT', 5), ('ШРИФТ ФАЙЛ.txt', 12), ('LOWER.txt', 5), ('dir1/SUB TWO/deep 3', 0)):
            e, i = sh.info(p)
            if e or i[1] != want_size:
                fail('%s: info %s: %d %s' % (what, p, e, i))
        e, _, ents = sh.cmd('ls', 'Many', 10, 5)
        if e or len(ents) != 5:
            fail('%s: ls Many 10 5: %d %d' % (what, e, len(ents)))
        e, _, _ = sh.cmd('read', 'sizes/s4097.bin', 4096, 10)
        e2, f, _ = sh.cmd('read', 'sizes/s4097.bin', 4096, 10)
        if e2 or f[0] != '1' or f[1] != files['sizes/s4097.bin'][4096:].hex():
            fail('%s: read at the end: %s' % (what, f))
        for p, want_e in (('nothing', E_NOT_FOUND), ('Many', E_ACCESS), ('lower.txt/x', E_NOT_FOUND), ('Dir1//Sub Two', E_NOT_FOUND)):
            e, _ = sh.get(p)
            if e != want_e:
                fail('%s: get %s: %d, %d wanted' % (what, p, e, want_e))
        # map against the FAT and mshowfat
        map_check(sh, img, ['big/two.bin' if kind == 'full' else 'big/one.bin', 'sizes/s4097.bin', 'lower.txt', 'sizes/s0.bin'], what)
        # 2: not one write on a volume that is not writable
        ro_check(sh, what)
        sh.close()
        count('read images')
    os.unlink(img)


def ro_check(sh, what):
    before = sh.writes()
    ops = [('write', 'lower.txt', 0, 1, WORK + '/data.bin'), ('write', 'new.txt', 0, 1, WORK + '/data.bin'),
           ('write', 'lower.txt', 3, 0, WORK + '/data.bin'), ('trunc', 'lower.txt', 1), ('trunc', 'lower.txt', 99999),
           ('mkdir', 'newdir'), ('mkdir', 'Many'), ('rm', 'lower.txt'), ('rm', 'Empty Folder'), ('mv', 'lower.txt', 'x.txt'),
           ('mv', 'Many', 'Dir1/Many'), ('attr', 'lower.txt', 0, 0, 0, 0)]
    with open(WORK + '/data.bin', 'wb') as fp:
        fp.write(b'data')
    for op in ops:
        e, _, _ = sh.cmd(*op)
        if e != E_ACCESS:
            fail('%s: read only: %s gives %d' % (what, op[0], e))
        count('read-only refusals')
    sh.cmd('sync')
    sh.tree()
    if sh.writes() != before or before:
        fail('%s: read only: %d writes' % (what, sh.writes()))


def map_check(sh, img, paths, what):
    g = bpb(img)
    for p in paths:
        e, f, _ = sh.cmd('map', p)
        if e:
            fail('%s: map %s: %d' % (what, p, e))
            continue
        fid, size, flags, n = int(f[0]), int(f[1]), int(f[2]), int(f[3])
        ext = [tuple(int(y) for y in x.split(':')) for x in f[4].split(',')] if n else []
        want = chain_extents(img, fid, size) if size else []
        if ext != want or len(ext) != n or sum(x[1] for x in ext) != size:
            fail('%s: map %s: %s, the FAT says %s' % (what, p, ext[:4], (want or [])[:4]))
        r = mtools('mshowfat', img, '::/' + p, check=False)
        cl = []
        for a, b in re.findall(r'<(\d+)(?:-(\d+))?>', r.stdout.decode('utf-8', 'surrogateescape')):
            cl += list(range(int(a), int(b or a) + 1))
        ms = []
        left = size
        for c in cl:
            off, k = g['data_off'] + (c - 2) * g['csize'], min(left, g['csize'])
            if k <= 0:
                break
            if ms and ms[-1][0] + ms[-1][1] == off:
                ms[-1][1] += k
            else:
                ms.append([off, k])
            left -= k
        if [tuple(x) for x in ms] != ext:
            fail('%s: map %s: mshowfat says %s' % (what, p, ms[:4]))
        want_flags = 0
        a = sh.info(p)[1][0]
        want_flags = (2 if a & A_RO else 0) | (4 if a & A_SYSTEM else 0)
        if flags != want_flags:
            fail('%s: map %s flags %d, %d wanted' % (what, p, flags, want_flags))
        count('maps compared')


# --- 3: write --------------------------------------------------------------------

class Writer:
    """operations through fat.c, the model alongside, the answers checked"""

    def __init__(self, sh, m, what):
        self.sh, self.m, self.what = sh, m, what

    def expect(self, op, got, want):
        count('operations checked')
        if got != want:
            fail('%s: %s: %d, %d wanted' % (self.what, op, got, want))

    def write(self, path, off, data, rewrite=False, want=None):
        e, done = self.sh.write(path, off, data, rewrite)
        w = m_write(self.m, path, off, data, rewrite)
        self.expect('write %s %d %d' % (path, off, len(data)), e, w if want is None else want)
        if e == E_OK and done != len(data):
            fail('%s: write %s: %d of %d' % (self.what, path, done, len(data)))

    def trunc(self, path, size):
        e, _, _ = self.sh.cmd('trunc', path, size)
        n = self.m.find(path)
        w = E_NOT_FOUND if n is None else E_ACCESS if n.isdir else E_OK
        if w == E_OK:
            n.data = n.data[:size] + bytes(max(0, size - len(n.data)))
            if size != len(n.data) or True:
                n.attr |= A_ARCH
        self.expect('trunc %s %d' % (path, size), e, w)

    def mkdir(self, path):
        e, _, _ = self.sh.cmd('mkdir', path)
        n = self.m.find(path)
        par, name = self.m.parent(path)
        if n is not None:
            w = E_OK if n.isdir else E_ACCESS
        elif par is None or not par.isdir or not name_ok(name):
            w = E_NOT_FOUND
        else:
            par.kids[key(name)] = Node(name, True)
            w = E_OK
        self.expect('mkdir %s' % path, e, w)

    def rm(self, path):
        e, _, _ = self.sh.cmd('rm', path)
        n = self.m.find(path)
        if n is None:
            w = E_NOT_FOUND
        elif n is self.m.root or (n.isdir and n.kids):
            w = E_ACCESS
        else:
            par, name = self.m.parent(path)
            del par.kids[key(name)]
            w = E_OK
        self.expect('rm %s' % path, e, w)

    def mv(self, a, b):
        e, _, _ = self.sh.cmd('mv', a, b)
        w = self.m_mv(a, b)
        self.expect('mv %s %s' % (a, b), e, w)
        return e

    def m_mv(self, a, b, apply=True):
        m = self.m
        n = m.find(a)
        if n is None:
            return E_NOT_FOUND
        if n is m.root or not b:
            return E_ACCESS
        i = b.rfind('/')
        cur = m.root
        for c in (b[:i].split('/') if i >= 0 else []):
            if not cur.isdir:
                return E_NOT_FOUND
            if cur is n:
                return E_ACCESS
            cur = cur.kids.get(key(c))
            if cur is None:
                return E_NOT_FOUND
        if not cur.isdir:
            return E_NOT_FOUND
        if cur is n:
            return E_ACCESS
        name = b[i + 1:]
        if not name_ok(name):
            return E_NOT_FOUND
        x = cur.kids.get(key(name))
        if x is not None and x is not n:
            return E_ACCESS
        if not apply:
            return E_OK
        par, oname = m.parent(a)
        del par.kids[key(oname)]
        n.name = name
        cur.kids[key(name)] = n
        return E_OK

    def attr(self, path, a, ct=0, at=0, mt=0):
        e, _, _ = self.sh.cmd('attr', path, a, ct, at, mt)
        n = self.m.find(path)
        w = E_NOT_FOUND if n is None else E_ACCESS if n is self.m.root else E_OK
        if w == E_OK:
            n.attr = (n.attr & A_DIR) | (a & 0x27)
        self.expect('attr %s' % path, e, w)

    def verify(self, img, phase):
        self.sh.cmd('sync')
        what = '%s %s' % (self.what, phase)
        compare_tree(self.sh, self.m, what)
        fsck_clean(img, what)
        mtools_compare(img, self.m, what)


def write_test():
    configs = [  # type, sector, sectors a cluster, image bytes, the volume's block, mkfs.fat's options
        (12, 512, 1, 2 * MB, 512, ()), (12, 4096, 1, 12 * MB, 4096, ()), (16, 512, 4, 16 * MB, 512, ()),
        (16, 4096, 1, 24 * MB, 4096, ()), (16, 2048, 2, 40 * MB, 512, ()), (32, 512, 1, 40 * MB, 512, ()),
        (32, 4096, 1, 270 * MB, 4096, ()), (32, 1024, 4, 280 * MB, 512, ()),
        (16, 512, 4, 16 * MB, 4096, ()), (32, 4096, 1, 270 * MB, 512, ()), (12, 512, 2, 4 * MB, 4096, ('-f', '1')),
        (32, 512, 1, 40 * MB, 4096, ('-f', '1'))]
    if QUICK:
        configs = [configs[0], configs[2], configs[5], configs[8], configs[9]]
    img = WORK + '/write.img'
    rnd = random.Random(3)
    for t, S, s, size, bs, extra in configs:
        mkimg(img, size, t, S, s, 'WRITE', extra)
        sh = Shell()
        e, _ = sh.mount(img, True, bs)
        if e:
            fail('write FAT%d: mount %d' % (t, e))
            continue
        w = Writer(sh, Model(), 'write FAT%d %d/%d on %d-byte blocks%s' % (t, S, S * s, bs, ' ' + ' '.join(extra) if extra else ''))
        cs = S * s
        # creates: sizes about the cluster's, names of every kind
        names = ['a.txt', 'UPPER.TXT', 'lower.txt', 'mixed.TXT', 'Mixed.txt', 'Long Name ä.txt', 'шрифт файл.txt',
                 '€uro.dat', 'x' * 200 + '.y', 'with space', '.dotfile', 'two.dots.here', 'longfilename1.txt',
                 'longfilename2.txt', 'longfilename3.txt', 'a+b,c;d=e[f].g', 'ÄÖÜ.TXT']
        for i, nm in enumerate(names):
            w.write(nm, 0, rnd.randbytes([0, 1, cs - 1, cs, cs + 1, 3 * cs + 5, 5000][i % 7]), True)
        for nm in ('bad|name', 'trailing.', 'trailing ', 'q?', 'a*b', 'x' * 256):
            w.write(nm, 0, b'x', True)
        w.write('missing/file', 0, b'x', True)
        w.write('a.txt/file', 0, b'x', True)
        w.write('not-there.txt', 0, b'x', False)
        w.mkdir('Docs')
        w.mkdir('Docs/Sub Folder')
        w.mkdir('Docs/Sub Folder/Deeper')
        w.mkdir('docs')
        w.mkdir('a.txt')
        w.mkdir('nope/deeper')
        w.mkdir('Docs/' + 'L' * 150)
        for i in range(40):
            w.write('Docs/Sub Folder/file %02d with a long name.bin' % i, 0, rnd.randbytes(rnd.randrange(3 * cs)), True)
        w.write('Docs', 0, b'x', True)
        w.verify(img, 'creates')
        # overwrites, appends with gaps, truncates
        w.write('UPPER.TXT', 0, rnd.randbytes(2 * cs + 3), True)
        w.write('a.txt', 0, b'', True)
        w.write('lower.txt', 2, b'ab')
        w.write('lower.txt', 10 * cs + 7, rnd.randbytes(cs + 9))
        w.write('Mixed.txt', 0, rnd.randbytes(3 * cs))
        w.write('Mixed.txt', 3 * cs, rnd.randbytes(1))
        w.write('€uro.dat', 50000, b'')
        w.write('Long Name ä.txt', 1, rnd.randbytes(5 * cs))
        w.trunc('Long Name ä.txt', cs + 1)
        w.trunc('Long Name ä.txt', 4 * cs + 3)
        w.trunc('шрифт файл.txt', 0)
        w.trunc('шрифт файл.txt', 0)
        w.trunc('two.dots.here', 7 * cs)
        w.trunc('Docs', 5)
        w.trunc('nothing', 5)
        w.write('Docs/Sub Folder/file 03 with a long name.bin', 0, rnd.randbytes(100000))
        w.verify(img, 'overwrites')
        # renames: in a folder, across folders, into and out of the root, case only, folders with ".."
        w.mv('a.txt', 'b.txt')
        w.mv('b.txt', 'B.TXT')
        w.mv('B.TXT', 'Docs/b moved.txt')
        w.mv('Docs/b moved.txt', 'back in root with a long name.txt')
        w.mv('lower.txt', 'LOWER.TXT')
        w.mv('LOWER.TXT', 'lower.txt')
        w.mv('Long Name ä.txt', 'long name Ä.TXT')
        w.mv('x' * 200 + '.y', 'Docs/' + 'z' * 230)
        w.mv('Docs/' + 'z' * 230, 'short')
        w.mv('UPPER.TXT', 'lower.txt')
        w.mv('nothing', 'x')
        w.mv('mixed.TXT', 'nowhere/x')
        w.mv('mixed.TXT', 'bad|name')
        w.mv('Docs', 'Docs/Sub Folder/Docs')
        w.mv('Docs', 'docs/inside')
        w.mv('Docs/Sub Folder', 'Moved Folder')
        w.mv('Moved Folder/Deeper', 'Docs/Deeper Again')
        w.mv('Moved Folder', 'Docs/Deeper Again/Moved Folder')
        w.mv('Docs/Deeper Again', 'deeper again')
        w.mv('deeper again', 'DEEPER AGAIN')
        w.mv('', 'x')
        w.mv('short', '')
        w.mkdir('deeper again/Moved Folder/new inside')
        w.verify(img, 'renames')
        # removes
        w.rm('two.dots.here')
        w.rm('deeper again')
        w.rm('deeper again/Moved Folder/new inside')
        w.rm('nothing')
        w.rm('')
        w.rm('Docs/' + 'L' * 150)
        for i in range(0, 40, 3):
            w.rm('deeper again/Moved Folder/file %02d with a long name.bin' % i)
        w.write('deeper again/Moved Folder/a new one with a name.txt', 0, b'reuse', True)
        w.verify(img, 'removes')
        # attributes and times
        w.attr('lower.txt', A_RO | A_SYSTEM)
        w.attr('Mixed.txt', A_HIDDEN, 1600000000, 1600000000, 1600000000)
        w.attr('Docs', A_HIDDEN | A_ARCH)
        w.attr('short', 0)
        w.attr('nothing', 0)
        w.attr('', 0)
        e, i = sh.info('Mixed.txt')
        e, f, _ = sh.cmd('info', 'Mixed.txt')
        if e or int(f[4]) != 1600000000 or int(f[2]) != 1600000000 or int(f[3]) != 1599955200:
            fail('%s: times %s' % (w.what, f))
        w.verify(img, 'attributes')
        # a folder of many entries, emptied, filled again
        for i in range(300):
            w.write('many/entry number %03d.txt' % i if i % 2 else 'many/e%03d' % i, 0, b'', True) if i else w.mkdir('many')
        for i in range(1, 300, 2):
            w.rm('many/entry number %03d.txt' % i)
        for i in range(1, 300, 4):
            w.write('many/another long name %03d.txt' % i, 0, b'x', True)
        w.verify(img, 'many')
        map_frag(sh, img, w)
        sh.close()
        count('write images')
    os.unlink(img)


def root_full_test():
    """the FAT12/16 root has its size: E_DISK_FULL when it is full, then room again"""
    img = WORK + '/root.img'
    for t in (12, 16):
        mkimg(img, 2 * MB if t == 12 else 8 * MB, t, 512, 1, 'ROOT', ('-r', '32'))
        sh = Shell()
        sh.mount(img, True)
        w = Writer(sh, Model(), 'full root FAT%d' % t)
        made = 0
        for i in range(40):
            e, done = sh.write('a long file name %02d.txt' % i, 0, b'x', True)
            if e == E_DISK_FULL:
                break
            if e:
                fail('%s: create %d: %d' % (w.what, i, e))
            w.m.root.kids[key('a long file name %02d.txt' % i)] = Node('a long file name %02d.txt' % i, False, b'x')
            made += 1
        e, _, _ = sh.cmd('mkdir', 'one more folder')
        if made != 32 // 3 or e != E_DISK_FULL:
            fail('%s: %d made, mkdir %d' % (w.what, made, e))
        w.verify(img, 'full')
        w.rm('a long file name 03.txt')
        w.write('SHORT.TXT', 0, b'fits', True)
        w.write('8.3', 0, b'fits too', True)
        w.verify(img, 'room again')
        sh.close()
        count('full roots')
    os.unlink(img)


def map_frag(sh, img, w):
    """6: map of fragmented files against the FAT and mshowfat, the flags by mattrib"""
    g = bpb(img)
    cs = g['csize']
    for k in range(6):
        for nm in ('frag one.bin', 'frag two.bin'):
            w.write(nm, k * cs, bytes([k]) * cs, k == 0)
    w.verify(img, 'fragments')
    sh.cmd('umount')
    mtools('mattrib', img, '+r', '+s', '::/frag one.bin')
    mtools('mattrib', img, '+s', '::/frag two.bin')
    sh.mount(img, True, 4096 if g['bps'] == 4096 else 512)
    map_check(sh, img, ['frag one.bin', 'frag two.bin', 'short'], w.what + ' map')
    e, f, _ = sh.cmd('map', 'frag one.bin')
    if int(f[3]) < 2:
        fail('%s: frag one.bin is in %s ranges' % (w.what, f[3]))
    for p in ('Docs', ''):
        if sh.cmd('map', p)[0] != E_ACCESS:
            fail('%s: map %r of a folder' % (w.what, p))
    w.m.find('frag one.bin').attr |= A_RO | A_SYSTEM
    w.m.find('frag two.bin').attr |= A_SYSTEM


# --- 4: stress ------------------------------------------------------------------------

def stress_test():
    configs = [(12, 512, 1, 1 * MB, 11), (16, 512, 1, 4200 * 1024 // 2 * 2, 12), (32, 512, 1, 34 * MB, 13),
               (12, 4096, 1, 4 * MB, 14)]
    nops = 600 if QUICK else 3000
    if QUICK:
        configs = configs[:2]
    img = WORK + '/stress.img'
    for t, S, s, size, seed in configs:
        mkimg(img, size, t, S, s, 'STRESS')
        what = 'stress FAT%d %d/%d seed %d' % (t, S, S * s, seed)
        g = bpb(img)
        cs = g['csize']
        cap = g['nclus'] * cs
        rnd = random.Random(seed)
        sh = Shell()
        sh.mount(img, True, 4096 if S == 4096 else 512)
        w = Writer(sh, Model(), what)
        m = w.m
        names = ['a', 'b.txt', 'C.DAT', 'dd.TXT', 'Ee', 'long name %d', 'шш %d.txt', 'äöü€ %d', 'x' * 100 + '%d',
                 'NAME%d.EXT', 'sameprefix%d.txt', 'bad|%d', 'z' * 210 + '%d']
        full = 0

        def rname():
            return rnd.choice(names).replace('%d', str(rnd.randrange(6)))

        def rdir():
            ds = [p for p, c in m.walk() if c.isdir]
            return rnd.choice([''] + ds) if ds else ''

        def rpath():
            ps = [p for p, c in m.walk()]
            if ps and rnd.random() < 0.7:
                p = rnd.choice(ps)
                return ''.join(ch.swapcase() if rnd.random() < 0.2 else ch for ch in p)
            d = rdir()
            return (d + '/' if d else '') + rname()

        for i in range(nops):
            r = rnd.random()
            if r < 0.35:
                p = rpath()
                n = m.find(p)
                rewrite = rnd.random() < 0.4
                big = rnd.random() < 0.1
                ln = rnd.randrange(cap // 3) if big else rnd.randrange(3 * cs)
                off = rnd.randrange(len(n.data) + 2 * cs) if n is not None and not n.isdir else 0
                data = rnd.randbytes(ln)
                e, done = sh.write(p, off, data, rewrite)
                if e == E_DISK_FULL:
                    full += 1
                    before = None if n is None else bytes(n.data)
                    # what is there now: created (rewrite), cut, or kept
                    ei, inf = sh.info(p)
                    if ei == E_OK and (n is None or rewrite):
                        if n is None:
                            par, name = m.parent(p)
                            n = par.kids[key(name)] = Node(name, False)
                        n.data = bytearray()
                        off = 0
                    if n is not None and done:
                        if off > len(n.data):
                            n.data += bytes(off - len(n.data))
                        n.data[off:off + done] = data[:done]
                        n.attr |= A_ARCH
                    elif n is not None and rewrite and ei == E_OK:
                        n.attr |= A_ARCH
                    if done > len(data):
                        fail('%s: write done %d > %d' % (what, done, len(data)))
                    count('disk full answers')
                else:
                    w.expect('write %s %d %d %d' % (p, off, ln, rewrite), e, m_write(m, p, off, data, rewrite))
                    if e == E_OK and done != ln:
                        fail('%s: write %s: %d of %d' % (what, p, done, ln))
            elif r < 0.45:
                p = rpath()
                n = m.find(p)
                size = rnd.randrange(4 * cs) if rnd.random() < 0.9 else rnd.randrange(cap)
                e, _, _ = sh.cmd('trunc', p, size)
                if e == E_DISK_FULL and n is not None and not n.isdir and size > len(n.data):
                    full += 1
                    count('disk full answers')
                else:
                    if n is not None and not n.isdir:
                        n.data = n.data[:size] + bytes(max(0, size - len(n.data)))
                        n.attr |= A_ARCH
                    w.expect('trunc %s %d' % (p, size), e, E_NOT_FOUND if n is None else E_ACCESS if n.isdir else E_OK)
            elif r < 0.55:
                d = rdir()
                p = (d + '/' if d else '') + rname()
                n = m.find(p)
                e, _, _ = sh.cmd('mkdir', p)
                par, name = m.parent(p)
                if e == E_DISK_FULL and n is None and par is not None and par.isdir and name_ok(name):
                    full += 1
                    count('disk full answers')
                else:
                    sh_e = e
                    par, name = m.parent(p)
                    if n is not None:
                        want = E_OK if n.isdir else E_ACCESS
                    elif par is None or not par.isdir or not name_ok(name):
                        want = E_NOT_FOUND
                    else:
                        par.kids[key(name)] = Node(name, True)
                        want = E_OK
                    w.expect('mkdir %s' % p, sh_e, want)
            elif r < 0.7:
                w.rm(rpath())
            elif r < 0.9:
                a = rpath()
                b = rpath() if rnd.random() < 0.3 else (lambda d: (d + '/' if d else '') + rname())(rdir())
                if rnd.random() < 0.1:
                    b = ''.join(ch.swapcase() for ch in a)
                e, _, _ = sh.cmd('mv', a, b)
                if e == E_DISK_FULL and w.m_mv(a, b, False) == E_OK:
                    full += 1
                    count('disk full answers')
                else:
                    w.expect('mv %s %s' % (a, b), e, w.m_mv(a, b))
            else:
                w.attr(rpath(), rnd.randrange(64))
            if i % 500 == 499 or i == nops - 1:
                w.verify(img, 'after %d operations' % (i + 1))
        count('stress operations', nops)
        for p, c in reversed(list(m.walk())):   # all removed, the disk filled
            w.rm(p)
        # filling the disk: E_DISK_FULL with a partial write, then the room again
        sh.cmd('mkdir', 'fill')
        m.find('').kids.setdefault(key('fill'), Node('fill', True))
        data = rnd.randbytes(cap)
        e, done = sh.write('fill/all.bin', 0, data, True)
        n = m.find('fill').kids[key('all.bin')] = Node('all.bin', False, data[:done])
        if e != E_DISK_FULL or not 0 < done < len(data):
            fail('%s: filling: %d, %d of %d' % (what, e, done, len(data)))
        e2, done2 = sh.write('fill/more.bin', 0, b'x' * (cs + 1), True)
        if e2 != E_DISK_FULL or done2 > cs:
            fail('%s: one more: %d %d' % (what, e2, done2))
        if sh.info('fill/more.bin')[0] == E_OK:
            m.find('fill').kids[key('more.bin')] = Node('more.bin', False, b'x' * done2)
        e3, _, _ = sh.cmd('trunc', 'fill/all.bin', done + 10 * cs)
        if e3 != E_DISK_FULL or sh.info('fill/all.bin')[1][1] != done:
            fail('%s: growing a full one: %d' % (what, e3))
        w.verify(img, 'full')
        w.rm('fill/all.bin')
        w.write('fill/again.bin', 0, data[:done // 2], True)
        w.write('fill/again.bin', done // 2, data[done // 2:done // 2 + done // 3])
        w.verify(img, 'filled again')
        sh.close()
        count('disk full answers', 0)
        print('  %s: %d disk-full answers' % (what, full), flush=True)
    os.unlink(img)


# --- 5: resets in the middle -----------------------------------------------------------

def crash_ops(rnd, cs):
    """a sequence touching everything: (command fields...)"""
    ops = []
    for i in range(6):
        ops.append(('W', 'file %d with a long name.bin' % i, 0, 1, rnd.randbytes(rnd.randrange(1, 4 * cs))))
    ops += [('mkdir', 'Folder One'), ('mkdir', 'Folder One/Inner'), ('W', 'Folder One/Inner/deep.txt', 0, 1, b'deep' * 300),
            ('W', 'file 1 with a long name.bin', 3 * cs, 0, rnd.randbytes(2 * cs)),  # append with a gap
            ('W', 'file 2 with a long name.bin', 0, 1, rnd.randbytes(cs // 2)),      # rewrite smaller
            ('trunc', 'file 3 with a long name.bin', 1), ('trunc', 'file 4 with a long name.bin', 5 * cs),
            ('mv', 'file 0 with a long name.bin', 'Folder One/moved file 0.bin'),
            ('mv', 'Folder One/Inner', 'Inner moved to the root'),
            ('mv', 'file 5 with a long name.bin', 'FILE5.BIN'),
            ('mv', 'FILE5.BIN', 'n' * 220),
            ('rm', 'file 3 with a long name.bin'), ('rm', 'Inner moved to the root/deep.txt'), ('rm', 'Inner moved to the root'),
            ('attr', 'file 1 with a long name.bin', A_RO | A_ARCH, 0, 0, 1600000000)]
    for i in range(30):
        ops.append(('W', 'Folder One/many %02d with a name.txt' % i, 0, 1, b'm' * (i * 37)))
    for i in range(0, 30, 2):
        ops.append(('rm', 'Folder One/many %02d with a name.txt' % i))
    ops.append(('sync',))
    return ops


def run_ops(sh, ops):
    """the writes done after each operation"""
    bounds = []
    for op in ops:
        if op[0] == 'W':
            sh.write(op[1], op[2], op[4], op[3])
        else:
            sh.cmd(*op)
        bounds.append(sh.writes())
    return bounds


def multi_sector(op, per):
    """an operation whose new name's entries cannot be in one sector"""
    return op[0] in ('mv', 'W') and len(op[2 if op[0] == 'mv' else 1].split('/')[-1]) > 13 * (per - 1)


# what a reset may leave (fsck's report, a first line and its indented ones):
# lost clusters, the FSInfo's count, the first FAT newer than the second, a
# chain longer than the size (an append between its link and its size)
ALLOWED = [r'Free cluster summary wrong', r'Free cluster summary uninitialized', r'FATs differ but appear to be intact',
           r'Reclaimed \d+ unused clusters?', r'cluster chain length is > \d+ bytes', r'Dirty bit']
# inherent in FAT (no order of single-sector writes avoids them), allowed only
# for the operation under way: a name's entries over two sectors, a moved
# folder's ".." and its entry in its new parent
INHERENT = {'lfn': [r'Orphaned long file name part', r'Unfinished long file name'],
            'dotdot': [r"Invalid '\.\.' entry"]}


def fsck_blocks(probs):
    """fsck's reports: a line that is not indented and the indented ones after it"""
    blocks = []
    for l in probs:
        if l.startswith(' ') and blocks:
            blocks[-1] += '\n' + l
        else:
            blocks.append(l)
    # a path's line belongs to the report after it
    out = []
    for b in blocks:
        if out and out[-1].startswith('/') and '\n' not in out[-1]:
            out[-1] += '\n' + b
        else:
            out.append(b)
    return out


def m_isdir(path, ops):
    """was path made a folder by the operations before (mkdir, or moved there)?"""
    dirs = set()
    for op in ops:
        if op[0] == 'mkdir':
            dirs.add(op[1])
        elif op[0] == 'mv' and op[1] in dirs:
            dirs.add(op[2])
    return path in dirs


def crash_test():
    configs = [(12, 512, 1, 2 * MB, 512), (16, 512, 2, 8 * MB, 512), (32, 512, 1, 34 * MB, 512), (16, 4096, 1, 20 * MB, 4096),
               (16, 512, 2, 8 * MB, 4096), (16, 4096, 1, 20 * MB, 512)]
    if QUICK:
        configs = configs[:2] + configs[4:]
    base, img = WORK + '/crash-base.img', WORK + '/crash.img'
    seen = {}
    for t, S, s, size, bs in configs:
        mkimg(base, size, t, S, s, 'CRASH')
        mtools('mmd', base, '::/existing folder')
        with open(WORK + '/pre.bin', 'wb') as fp:
            fp.write(os.urandom(40000))
        mtools('mcopy', base, WORK + '/pre.bin', '::/existing folder/pre-existing file.bin')
        cs = S * s
        ops = crash_ops(random.Random(t), cs)
        shutil.copy(base, img)
        sh = Shell()
        sh.mount(img, True, bs)
        w0 = sh.writes()
        bounds = [b - w0 for b in run_ops(sh, ops)]
        total = sh.writes() - w0
        sh.close()
        per = min(S, bs) // 32
        if not fsck_clean(img, 'crash FAT%d: without a reset' % t):
            continue
        step = 1 if total <= (150 if QUICK else 700) else total // (150 if QUICK else 700) + 1
        what = 'crash FAT%d %d/%d on %d-byte blocks' % (t, S, cs, bs)
        bad = 0
        sh = Shell()
        for n in range(0, total + 1, step):
            shutil.copy(base, img)
            sh.mount(img, True, bs)
            sh.cmd('fail', n)
            run_ops(sh, ops)
            sh.cmd('umount')
            sh.cmd('fail', -1)
            rc, out = fsck(img)
            probs = fsck_problems(out)
            i = next((k for k, b in enumerate(bounds) if b > n), len(ops) - 1)
            op = ops[i]
            ok = list(ALLOWED)
            if multi_sector(op, per):
                ok += INHERENT['lfn']
            if op[0] == 'mv' and op[1].rsplit('/', 1)[0] != op[2].rsplit('/', 1)[0] and \
                    m_isdir(op[1], ops[:i]):
                ok += INHERENT['dotdot']
            for blk in fsck_blocks(probs):
                if not any(re.search(a, blk) for a in ok):
                    bad += 1
                    fail('%s: a reset after write %d of %d (in %s): fsck: %s' % (what, n, total, op[:3], blk))
                k = [x.strip() for x in blk.split('\n') if not x.startswith('/')][0]
                k = re.sub(r'"[^"]*"', '"..."', re.sub(r'\d+', 'N', k))
                k = '%-58s (in %s)' % (k, {'W': 'write', 'mv': 'rename', 'rm': 'remove', 'trunc': 'truncate'}.get(op[0], op[0]))
                seen[k] = seen.get(k, 0) + 1
            # it mounts and lists, by fat.c and mtools
            e, _ = sh.mount(img, False, bs)
            e2, _ = sh.tree()
            if e or e2:
                fail('%s: after a reset at %d: mount %d tree %d' % (what, n, e, e2))
            sh.cmd('umount')
            if mtools('mdir', img, '-/', '-a', '::/', check=False).returncode:
                fail('%s: mdir after a reset at %d' % (what, n))
            count('resets simulated')
        sh.close()
        print('  %s: %d writes, resets after every %d, %d bad' % (what, total, step, bad), flush=True)
    print('  fsck reports after resets (count, report, the operation under way):')
    for k, v in sorted(seen.items(), key=lambda x: -x[1]):
        print('    %5d  %s' % (v, k))
    for p in (base, img):
        os.unlink(p)


# --- 6 (broken chains), 7: fuzz ---------------------------------------------------------

def broken_chain_test():
    img = WORK + '/broken.img'
    mkimg(img, 4 * MB, 16, 512, 1, 'BROKEN')
    for name, n in (('cycle.bin', 5000), ('short.bin', 3000), ('self.bin', 2000)):
        with open(WORK + '/x.bin', 'wb') as fp:
            fp.write(os.urandom(n))
        mtools('mcopy', img, WORK + '/x.bin', '::/' + name)
    g = bpb(img)
    sh = Shell()
    sh.mount(img, False)
    ids = {p: int(sh.cmd('map', p)[1][0]) for p in ('cycle.bin', 'short.bin', 'self.bin')}
    sh.cmd('umount')
    with open(img, 'r+b') as fp:
        def put(c, v):
            for k in range(2):
                fp.seek(g['fat_off'] + k * g['fat_bytes'] + 2 * c)
                fp.write(struct.pack('<H', v))
        c = ids['cycle.bin']
        put(c + 3, c + 1)          # 10 clusters: the 4th back to the 2nd
        put(ids['short.bin'] + 2, 0xffff)
        put(ids['self.bin'], ids['self.bin'])
    sh.mount(img, False)
    for p, want_map, want_read in (('cycle.bin', E_FS, (E_OK, E_FS)), ('short.bin', E_FS, (E_FS,)), ('self.bin', E_FS, (E_OK, E_FS))):
        e = sh.cmd('map', p)[0]
        if e != want_map:
            fail('broken chains: map %s: %d' % (p, e))
        e, _ = sh.get(p)
        if e not in want_read:
            fail('broken chains: read %s: %d' % (p, e))
        count('broken chains')
    sh.close()
    # written: the cycle is not followed forever
    sh = Shell()
    sh.mount(img, True)
    for op in (('trunc', 'cycle.bin', 100000), ('rm', 'self.bin'), ('W', 'cycle.bin', 9000, 0, b'z' * 10000), ('rm', 'cycle.bin')):
        if op[0] == 'W':
            sh.write(op[1], op[2], op[4], op[3])
        else:
            sh.cmd(*op)
    sh.tree()
    sh.close()
    os.unlink(img)


def fuzz_test():
    sh = Shell()
    n = 60 if QUICK else 400
    for t, S, s, size in ((12, 512, 1, 1 * MB), (16, 512, 1, 3 * MB), (32, 512, 1, 33 * MB), (16, 4096, 1, 17 * MB)):
        img = WORK + '/fuzz%d.img' % t
        mkimg(img, size, t, S, s, 'FUZZ')
        mtools('mmd', img, '::/a folder', '::/a folder/b')
        for i in range(12):
            with open(WORK + '/x.bin', 'wb') as fp:
                fp.write(os.urandom(i * 700))
            mtools('mcopy', img, WORK + '/x.bin', '::/%s/file number %d with a long name.bin' % ('a folder' if i % 2 else 'a folder/b', i))
            mtools('mcopy', img, WORK + '/x.bin', '::/F%d.TXT' % i)
        if t == 32:  # the FAT32 image is cut to its first 2 MiB for the fuzz (the rest is empty)
            pass
        sh.cmd('fuzz', img, t, n)
        line = [l for l in sh.log if l.startswith('fuzz:')]
        print('  ' + (line[-1] if line else 'fuzz: no result'), flush=True)
        count('fuzzed images', n)
        os.unlink(img)
    sh.close()


def main():
    shutil.rmtree(WORK, ignore_errors=True)
    os.makedirs(WORK)
    try:
        trees = make_sources()
        for name, fn in (('read', lambda: read_test(trees)), ('write', write_test), ('full root', root_full_test),
                         ('stress', stress_test),
                         ('crash', crash_test), ('broken chains', broken_chain_test), ('fuzz', fuzz_test)):
            print('fat: %s ...' % name, flush=True)
            fn()
    finally:
        shutil.rmtree(WORK, ignore_errors=True)
    for k in sorted(counts):
        print('  %-22s %d' % (k, counts[k]))
    if fails:
        sys.exit('fat: %d failures' % fails)
    print('fat: all passed')


if __name__ == '__main__':
    main()

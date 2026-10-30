#!/usr/bin/env python3
"""tests/kfar_arc: kfar_arc on the PC (make -C tests/kfar_arc).

    run.py OUTDIR

The archives: apps/kfar/mkarc.py's (the ramdisk's: zip, 7z with LZMA, BCJ,
Copy) and, when 7-Zip (7z, 7za or 7zz) is on the PC, more of its own:
zip with Deflate64, Copy and ZipCrypto, 7z with LZMA (an encoded header),
PPMd, BCJ2, Deflate, Deflate64, AES-256 with an encrypted header. Each is
unpacked by OUTDIR/host (host.c) twice, through the folders (with
setpos, and once more with no memory streams) and through GetFiles,
and compared with Python's zipfile or 7-Zip's own unpacking.
"""
import filecmp
import os
import shutil
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
TOP = os.path.dirname(os.path.dirname(HERE))
out = sys.argv[1]
host = os.path.join(out, 'host')
sys.path.insert(0, os.path.join(TOP, 'apps', 'kfar'))
import mkarc  # noqa: E402

failed = []


def check(ok, what):
    print(('ok   ' if ok else 'FAIL ') + what)
    if not ok:
        failed.append(what)


def same(a, b):
    c = filecmp.dircmp(a, b)

    def walk(c):
        if c.left_only or c.right_only or c.diff_files or c.funny_files:
            return False
        return all(walk(s) for s in c.subdirs.values())
    return walk(c)


def tree(path):
    """a folder of mkarc's CONTENTS and FILES7Z-like files for 7-Zip"""
    shutil.rmtree(path, ignore_errors=True)
    for name, data, _ in mkarc.CONTENTS + [(n, d, None) for n, d, _ in mkarc.FILES7Z]:
        p = os.path.join(path, name)
        if data is None:
            os.makedirs(p, exist_ok=True)
        else:
            os.makedirs(os.path.dirname(p), exist_ok=True)
            open(p, 'wb').write(data)
    return path


def run_host(arc, dest, *extra, env=None):
    shutil.rmtree(dest, ignore_errors=True)
    r = subprocess.run([host, arc, dest, *extra], capture_output=True, text=True,
                       env=dict(os.environ, **(env or {})))
    if r.returncode:
        print('     ' + (r.stderr.strip() or r.stdout.strip()).replace('\n', '\n     '))
    return r.returncode == 0


def test(arc, ref, what, password=None):
    env = {'PASSWORD': password} if password else {}
    ok1 = run_host(arc, out + '/walk', env=env) and same(ref, out + '/walk')
    ok2 = run_host(arc, out + '/stream', env=dict(env, FREEMEM='0')) and same(ref, out + '/stream')
    ok3 = run_host(arc, out + '/getfiles', 'getfiles', env=env) and same(ref, out + '/getfiles')
    check(ok1 and ok2 and ok3, f'{what} (folders {ok1}, no memory streams {ok2}, GetFiles {ok3})')


os.makedirs(out, exist_ok=True)
# the ramdisk's archives
z = os.path.join(out, 'arc.zip')
mkarc.make_zip(z)
ref = os.path.join(out, 'ref-zip')
shutil.rmtree(ref, ignore_errors=True)
import zipfile  # noqa: E402
zipfile.ZipFile(z).extractall(ref)
test(z, ref, 'mkarc zip: stored, deflated, folders')
s = os.path.join(out, 'arc.7z')
mkarc.make_7z(s)
sevenzip = shutil.which('7z') or shutil.which('7za') or shutil.which('7zz')
if sevenzip:
    ref = os.path.join(out, 'ref-7z')
    shutil.rmtree(ref, ignore_errors=True)
    subprocess.run([sevenzip, 'x', '-o' + ref, s], check=True, stdout=subprocess.DEVNULL)
    test(s, ref, 'mkarc 7z: LZMA, LZMA+BCJ, Copy')
    src = tree(os.path.join(out, 'src'))
    cases = [
        ('zip', ['-tzip', '-mm=Deflate64'], None, 'zip Deflate64'),
        ('zip', ['-tzip', '-mm=Copy'], None, 'zip Copy'),
        ('zip', ['-tzip', '-mm=Deflate', '-pkolibri', '-mem=ZipCrypto'], 'kolibri', 'zip Deflate with ZipCrypto'),
        ('7z', ['-t7z', '-m0=LZMA'], None, '7z LZMA, encoded header'),
        ('7z', ['-t7z', '-m0=LZMA', '-ms=off', '-mhc=off'], None, '7z LZMA, not solid, plain header'),
        ('7z', ['-t7z', '-m0=PPMd'], None, '7z PPMd'),
        ('7z', ['-t7z', '-m0=BCJ2', '-m1=LZMA', '-m2=LZMA', '-m3=LZMA', '-mb0:1', '-mb0s1:2', '-mb0s2:3'],
         None, '7z BCJ2 with LZMA'),
        ('7z', ['-t7z', '-m0=Deflate'], None, '7z Deflate'),
        ('7z', ['-t7z', '-m0=Deflate64'], None, '7z Deflate64'),
        ('7z', ['-t7z', '-m0=LZMA', '-pkolibri', '-mhe=on'], 'kolibri', '7z AES-256, encrypted header'),
    ]
    for i, (ext, opts, pw, what) in enumerate(cases):
        arc = os.path.join(out, f'case{i}.{ext}')
        if os.path.exists(arc):
            os.unlink(arc)
        r = subprocess.run([sevenzip, 'a', *opts, arc, '.'], cwd=src, capture_output=True, text=True)
        if r.returncode:
            check(False, f'{what}: 7-Zip could not make it')
            continue
        ref = os.path.join(out, f'ref{i}')
        shutil.rmtree(ref, ignore_errors=True)
        subprocess.run([sevenzip, 'x', '-o' + ref, *(['-p' + pw] if pw else []), arc], check=True,
                       stdout=subprocess.DEVNULL)
        test(arc, ref, what, pw)
else:
    print('     (no 7-Zip on the PC: its archives are left out)')
print('tests/kfar_arc: ' + ('all passed' if not failed else f'{len(failed)} failed'))
sys.exit(1 if failed else 0)

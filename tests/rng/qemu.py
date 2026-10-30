#!/usr/bin/env python3
"""The random numbers in QEMU (make -C tests/rng qemu; docs/rng.md). Builds
the kernels (make PLATFORM=arm64 with tests/linux/rngtest, make for
realview) and runs:

  virt          virtio-rng and QEMU's /chosen/rng-seed: both in the seed,
                kolibri.rngtest's FIPS 140-2 on virtio-rng and the
                generator, the Linux personality's getrandom, /dev/urandom,
                /dev/random and crypto/rand (tests/linux/rngtest)
  virtio        virtio-rng alone (dtb-randomness=off)
  jitter        no source at all: seeded from timing jitter, the warning
  noseed        no source and kolibri.rngjitter=off: no seed; getrandom
                with GRND_NONBLOCK gives EAGAIN, a blocking one waits
                (rngtest wait)
  realview      ARMv7 (the N950's kernel without the N950): jitter only
  pro1x-*       the Pro1X's code paths (tests/power/mkdtb.py's tree) with a
                qcom,prng-ee node and a qcom,gcc-sm6115 node in RAM, and
                /chosen/rng-seed as kexec -s gives it (128 bytes):
                  pro1x-seed   the seed taken (and wiped in the tree); the
                               PRNG's RAM gives one word again and again:
                               the continuous test throws it out
                  pro1x-nodata STATUS never says a word is there: 10 ms
                  pro1x-clock  the clock's CLK_OFF stays set: no read
  messengers    with PURPLE_HOST, SIGNAL_HOST, YAPPARI_WA (paths) in the
                environment: each backend started by the messenger takes
                its first random bytes from the kernel's generator (the
                network: QEMU's user networking; Telegram logs in with a
                number of its reserved test range, 99966...)

    qemu.py [SCENARIO...]          all but messengers when none given
"""
import os
import re
import socket
import struct
import subprocess
import sys
import tempfile
import time

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
TMP = tempfile.mkdtemp(prefix='kos-rng-')
fails = 0


def run(cmd, **kw):
    return subprocess.run(cmd, cwd=ROOT, check=True, **kw)


def build(linux_files=''):
    run(['make', '-s', '-j8', 'PLATFORM=arm64', 'LINUX_FILES=' + linux_files, 'all'],
        stdout=subprocess.DEVNULL)


def qemu64(name, append, seconds, machine='virt,gic-version=3', extra=(), until=None):
    """the AArch64 kernel in QEMU's virt; the serial console's text"""
    log = os.path.join(TMP, name + '.log')
    cmd = ['qemu-system-aarch64', '-M', machine, '-cpu', 'cortex-a53', '-m', '1024', '-device', 'ramfb',
           '-display', 'none', '-global', 'virtio-mmio.force-legacy=false', '-serial', 'file:' + log,
           '-kernel', 'build/arm64/kernel.bin', '-append', append] + list(extra)
    q = subprocess.Popen(cmd, cwd=ROOT, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    t0 = time.time()
    while time.time() - t0 < seconds:
        time.sleep(0.5)
        text = open(log, errors='replace').read() if os.path.exists(log) else ''
        if until and all(u in text for u in until):
            break
    q.terminate()
    q.wait()
    return open(log, errors='replace').read()


def expect(name, text, wanted, unwanted=()):
    """every line of wanted in text in that order, none of unwanted"""
    global fails
    pos, missing = 0, []
    for w in wanted:
        i = text.find(w, pos)
        if i < 0:
            missing.append(w)
        else:
            pos = i + len(w)
    bad = [u for u in unwanted if u in text]
    for l in text.splitlines():
        if l.startswith(('rng:', 'rngtest:', 'lx:', 'pro1x: the PRNG', 'n950: the secure')):
            print(f'  {name}: {l}')
    if missing or bad:
        fails += 1
        print(f'FAIL {name}: ' + '; '.join([f'missing "{m}"' for m in missing] + [f'has "{b}"' for b in bad]))
        print(f'  (the log: {TMP}/{name}.log)')
    else:
        print(f'ok {name}: {len(wanted)} lines in order')


def virt():
    t = qemu64('virt', 'kolibri.nomodem kolibri.rngtest kolibri.run=/bin/rngtest', 60,
               extra=['-device', 'virtio-rng-device'], until=['rngtest: done'])
    expect('virt', t, [
        'rng: virtio-rng',
        'rng: seeded from virtio-rng (64 bytes), the boot loader\'s seed (32 bytes)',
        'rngtest: virtio-rng: 8 of 8 blocks pass FIPS 140-2',
        'rngtest: the generator: 8 of 8 blocks pass FIPS 140-2',
        'lx: rngtest: getrandom from the kernel\'s generator, seeded from virtio-rng (64 bytes)',
        'rngtest: ok FIPS 140-2: 4 of 4 blocks from getrandom pass',
        'rngtest: done, 0 failures'], ['WEAK', 'FAIL', 'NOT seeded'])


def virtio():
    t = qemu64('virtio', 'kolibri.nomodem kolibri.run=/bin/rngtest', 60, machine='virt,gic-version=3,dtb-randomness=off',
               extra=['-device', 'virtio-rng-device'], until=['rngtest: done'])
    expect('virtio', t, ['rng: seeded from virtio-rng (64 bytes)\n', 'rngtest: done, 0 failures'],
           ['WEAK', 'boot loader', 'FAIL'])


def jitter():
    t = qemu64('jitter', 'kolibri.nomodem kolibri.run=/bin/rngtest', 90, machine='virt,gic-version=3,dtb-randomness=off',
               until=['rngtest: done'])
    expect('jitter', t, ['rng: seeded from timing jitter (', 'rng: WEAK: no hardware source',
                         'lx: rngtest: getrandom from the kernel\'s generator, seeded from timing jitter',
                         'rngtest: done, 0 failures'], ['FAIL'])


def noseed():
    t = qemu64('noseed', 'kolibri.nomodem kolibri.rngjitter=off kolibri.run=/bin/rngtest kolibri.args=wait', 60,
               machine='virt,gic-version=3,dtb-randomness=off', until=['rngtest: done'])
    expect('noseed', t, ['rng: NOT seeded: 0 of 256 bits, no jitter',
                         'rngtest: ok no seed: GRND_NONBLOCK gives EAGAIN',
                         'rngtest: ok no seed: GRND_INSECURE gives bytes',
                         'rngtest: ok no seed: /dev/urandom with O_NONBLOCK gives EAGAIN',
                         'rngtest: ok no seed: a blocking getrandom and a read of /dev/urandom still wait after 3 s',
                         'rngtest: done, 0 failures'], ['rng: seeded', 'FAIL'])


def realview():
    run(['make', '-s', '-j8'], stdout=subprocess.DEVNULL)
    log = os.path.join(TMP, 'realview.log')
    q = subprocess.Popen(['qemu-system-arm', '-M', 'realview-pb-a8', '-m', '256', '-display', 'none',
                          '-serial', 'file:' + log, '-kernel', 'build/realview/kernel.bin',
                          '-append', 'kolibri.nomodem kolibri.rngtest'], cwd=ROOT,
                         stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    for _ in range(60):
        time.sleep(0.5)
        if 'rngtest: the generator: 8' in open(log, errors='replace').read():
            break
    q.terminate()
    q.wait()
    expect('realview', open(log, errors='replace').read(),
           ['rng: seeded from timing jitter (', 'rng: WEAK', 'rngtest: timing jitter: 4096 samples',
            'rngtest: the generator: 8 of 8 blocks pass FIPS 140-2'], ['NOT seeded'])


# --- the Pro1X's code paths ------------------------------------------------------------

PRNG_PA, GCC_PA = 0x50000000, 0x50100000
DTB_PA = 0x48000000                    # where QEMU puts the tree with 8 GiB (hw/arm/boot.c)
SEED = bytes((0xa5 ^ i * 7) & 0xff for i in range(128))


def pro1x_dtb(name):
    src = os.path.join(TMP, name + '.dts')
    env = dict(os.environ, MKDTB_DTS=src)
    run(['python3', 'tests/power/mkdtb.py', os.path.join(TMP, 'base.dtb'),
         '--bootargs', 'kolibri.nomodem kolibri.rngtest kolibri.lifetime=0'], env=env)
    dts = open(src).read()
    seed = ' '.join('%02x' % b for b in SEED)
    dts = dts.replace('bootargs = ', f'rng-seed = [{seed}];\n\t\tkaslr-seed = <0x01020304 0x05060708>;\n\t\tbootargs = ', 1)
    dts = dts.replace('\t\tsplash@5c000000 {', f'''\t\tprng@{PRNG_PA:x} {{
			reg = <0x00 {PRNG_PA:#x} 0x00 0x200000>;
			no-map;
		}};

		splash@5c000000 {{''', 1)
    nodes = f'''
	rng@{PRNG_PA:x} {{
		compatible = "qcom,prng-ee";
		reg = <0x00 {PRNG_PA:#x} 0x00 0x1000>;
	}};

	clock-controller@{GCC_PA:x} {{
		compatible = "qcom,gcc-sm6115";
		reg = <0x00 {GCC_PA:#x} 0x00 0x1f0000>;
	}};
'''
    i = dts.rfind('};')
    dts = dts[:i] + nodes + dts[i:]
    out = os.path.join(TMP, name + '.dtb')
    subprocess.run(['dtc', '-q', '-I', 'dts', '-O', 'dtb', '-o', out, '-'], input=dts, text=True, check=True)
    return out


def ram_file(name, words):
    """{offset: word} as a file for QEMU's loader at PRNG_PA (2 MiB: the
    PRNG's window, the GCC's at +1 MiB)"""
    b = bytearray(0x200000)
    for off, w in words.items():
        struct.pack_into('<I', b, off, w)
    p = os.path.join(TMP, name + '.bin')
    open(p, 'wb').write(b)
    return p


def pro1x(name, words, seconds, wanted, unwanted=()):
    dtb = pro1x_dtb(name)
    sock = os.path.join(TMP, name + '.sock')
    q = subprocess.Popen(['qemu-system-aarch64', '-M', 'virt,gic-version=3', '-cpu', 'cortex-a53', '-m', '8G',
                          '-device', 'ramfb', '-display', 'none', '-serial', 'null',
                          '-monitor', 'unix:' + sock + ',server,nowait', '-kernel', 'build/arm64/kernel.bin',
                          '-dtb', dtb, '-device',
                          f'loader,file={ram_file(name, words)},addr={PRNG_PA:#x},force-raw=on'],
                         cwd=ROOT, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    time.sleep(seconds)
    m = socket.socket(socket.AF_UNIX)
    m.connect(sock)
    con, tree = os.path.join(TMP, name + '.console'), os.path.join(TMP, name + '.tree')
    m.sendall(f'pmemsave 0x5ca01000 0x40000 "{con}"\n'.encode())
    m.sendall(f'pmemsave {DTB_PA:#x} 0x100000 "{tree}"\n'.encode())
    time.sleep(1)
    m.close()
    q.terminate()
    q.wait()
    text = open(con, 'rb').read().split(b'\0')[0].decode(errors='replace')
    open(os.path.join(TMP, name + '.log'), 'w').write(text)
    expect(name, text, wanted, unwanted)
    return open(tree, 'rb').read()


def pro1x_all():
    global fails
    gcc_on = {0x100000 + 0x21004: 0}              # CBCR: the clock on
    tree = pro1x('pro1x-seed', {**gcc_on, 0: 0x12345678, 4: 1}, 25, [
        'rng: the SoC\'s PRNG',
        'rng: seeded from the boot loader\'s seed (128 bytes)',
        'rng: the SoC\'s PRNG gives the same word again and again: not used',
        'rngtest: the SoC\'s PRNG: 0 of 8 blocks pass'], ['WEAK', 'seeded from the SoC'])
    if tree[:4] != bytes.fromhex('d00dfeed'):
        fails += 1
        print(f'FAIL pro1x-seed: no device tree at {DTB_PA:#x}')
    elif SEED[:16] in tree or SEED[64:80] in tree:
        fails += 1
        print('FAIL pro1x-seed: the seed is still in the device tree')
    else:
        print('ok pro1x-seed: the seed wiped in the device tree')
    pro1x('pro1x-nodata', {**gcc_on, 0: 0x12345678, 4: 0}, 25, [
        'rng: the SoC\'s PRNG', 'pro1x: the PRNG: no word for 10 ms',
        'rng: seeded from the boot loader\'s seed (128 bytes)'])
    pro1x('pro1x-clock', {0x100000 + 0x21004: 0x80000000, 0: 0x12345678, 4: 1}, 25, [
        'rng: the SoC\'s PRNG', 'pro1x: the PRNG: its clock does not come on'])


def messengers():
    # Telegram: a login at once with a number of Telegram's reserved test
    # range (99966..., not anyone's), so that TDLib starts and connects
    hosts = [('PURPLE_HOST', 'purple-host', 'telegram,+9996610000'), ('SIGNAL_HOST', 'signal-host', 'signal'),
             ('YAPPARI_WA', 'yappari-wa', 'whatsapp')]
    files = [f'bin/{n}={os.environ[e]}' for e, n, _ in hosts if os.environ.get(e)]
    if not files:
        print('messengers: PURPLE_HOST, SIGNAL_HOST, YAPPARI_WA not set')
        return
    files.append('etc/ssl/certs/ca-certificates.crt=/etc/ssl/certs/ca-certificates.crt')
    build(' '.join(files))
    for e, n, service in hosts:
        if not os.environ.get(e):
            continue
        line = f'lx: {n}: getrandom from the kernel\'s generator, seeded from virtio-rng (64 bytes)'
        t = qemu64('messenger-' + n, f'kolibri.nomodem kolibri.run=/sys/messenger kolibri.args={service}', 120,
                   extra=['-device', 'virtio-rng-device', '-device', 'virtio-net-device,netdev=n0',
                          '-netdev', 'user,id=n0'], until=[line])
        expect('messenger-' + n, t, ['rng: seeded from virtio-rng (64 bytes)', f'started {n}', line],
               ['WEAK', 'NOT seeded'])


def main():
    want = sys.argv[1:] or ['virt', 'virtio', 'jitter', 'noseed', 'realview', 'pro1x']
    print('qemu: logs in', TMP)
    if set(want) & {'virt', 'virtio', 'jitter', 'noseed'}:
        subprocess.run(['go', 'build', '-trimpath', '-ldflags=-s', '-o', 'rngtest', '.'],
                       cwd=os.path.join(ROOT, 'tests/linux/rngtest'), check=True,
                       env=dict(os.environ, CGO_ENABLED='0', GOOS='linux', GOARCH='arm64'))
        build('bin/rngtest=tests/linux/rngtest/rngtest')
    for w in want:
        if w == 'pro1x':
            build()
            pro1x_all()
        else:
            globals()[w]()
    print('qemu: %s' % ('all passed' if not fails else f'{fails} failed'))
    return 1 if fails else 0


sys.exit(main())

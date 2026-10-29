#!/usr/bin/env python3
"""The AArch64 kernel's power test in QEMU (make -C tests/power):

    qemu.py KERNEL NAME SECONDS CHECK... [-- QEMU ARGUMENTS]

Runs the kernel with kolibri.powertest (a line every 10 s), reads its
console (QEMU virt: the serial port; a Pro1X device tree, --pro1x DTB:
the RAM console at 0x5ca01000 through QEMU's monitor) and checks the
lines. CHECK: max-rate=R (every interval below R wake-ups/s),
min-rate=R, has=TEXT (a console line contains it), cluster (an idle in
the cluster state), core (... the core state)."""
import os
import re
import socket
import subprocess
import sys
import tempfile
import time

args = sys.argv[1:]
qargs = []
if '--' in args:
    i = args.index('--')
    args, qargs = args[:i], args[i + 1:]
dtb = None
if args and args[0] == '--pro1x':
    dtb = args[1]
    args = args[2:]
kernel, name, seconds, checks = args[0], args[1], float(args[2]), args[3:]

tmp = tempfile.mkdtemp(prefix='kos-power-')
log = os.path.join(tmp, 'serial.log')
sock = os.path.join(tmp, 'mon.sock')
cmd = ['qemu-system-aarch64', '-M', 'virt,gic-version=3', '-cpu', 'cortex-a53', '-m', '8G' if dtb else '1G',
       '-device', 'ramfb', '-display', 'none', '-serial', 'file:' + log,
       '-monitor', 'unix:' + sock + ',server,nowait', '-kernel', kernel] + qargs
if dtb:
    cmd += ['-dtb', dtb]
q = subprocess.Popen(cmd, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
time.sleep(seconds)
text = ''
if dtb:
    m = socket.socket(socket.AF_UNIX)
    m.connect(sock)
    out = os.path.join(tmp, 'console.bin')
    m.sendall(f'pmemsave 0x5ca01000 0x40000 "{out}"\n'.encode())
    time.sleep(1)
    m.close()
    text = open(out, 'rb').read().split(b'\0')[0].decode(errors='replace')
q.terminate()
q.wait()
if not dtb:
    text = open(log, errors='replace').read()

lines = [l for l in text.splitlines() if l.startswith(('power:', 'psci:', 'timer:', 'rpm', 'mpm'))]
for l in lines:
    print(f'{name}: {l}')
rates = [float(r) for r in re.findall(r'^power: \d+ s: \d+ wake-ups \(([\d.]+)/s\)', text, re.M)]
fail = []
if not rates:
    fail.append('no power lines')
for c in checks:
    k, _, v = c.partition('=')
    if k == 'max-rate' and rates and max(rates) > float(v):
        fail.append(f'{max(rates)} wake-ups/s > {v}')
    elif k == 'min-rate' and rates and min(rates) < float(v):
        fail.append(f'{min(rates)} wake-ups/s < {v}')
    elif k == 'has' and v not in text:
        fail.append(f'no "{v}"')
    elif k in ('core', 'cluster'):
        n = sum(int(x) for x in re.findall(rf'^power: \d+ s:.* {k} (\d+),', text, re.M))
        if not n:
            fail.append(f'never in the {k} state')
if fail:
    print(f'{name}: FAILED: ' + '; '.join(fail))
    sys.exit(1)
print(f'{name}: ok')

#!/usr/bin/env python3
"""The AArch64 kernel's power test in QEMU (make -C tests/power):

    qemu.py KERNEL NAME SECONDS CHECK... [-- QEMU ARGUMENTS]

Runs the kernel with kolibri.powertest (a line every 10 s), reads its
console (QEMU virt: the serial port; a Pro1X device tree, --pro1x DTB:
the RAM console at 0x5ca01000 through QEMU's monitor) and checks the
lines. CHECK: max-rate=R (every interval below R wake-ups/s; the checks
after skip-first leave the first interval, the start, out), min-rate=R,
has=TEXT (a console line contains it), cluster (an idle in the cluster
state), core (... the core state).

--net: a virtio network card on QEMU's user networking (DHCP, the host
as 10.0.2.2); --hold PORT: a TCP server on the host's PORT that accepts
connections, reads them and keeps them open (tests/linux/tcphold)."""
import os
import re
import socket
import subprocess
import sys
import tempfile
import threading
import time

args = sys.argv[1:]
qargs = []
if '--' in args:
    i = args.index('--')
    args, qargs = args[:i], args[i + 1:]
dtb = None
net = False
hold = None
while args and args[0].startswith('--'):
    if args[0] == '--pro1x':
        dtb = args[1]
        args = args[2:]
    elif args[0] == '--net':
        net = True
        args = args[1:]
    elif args[0] == '--hold':
        hold = int(args[1])
        args = args[2:]
    else:
        sys.exit(f'qemu.py: {args[0]}?')
kernel, name, seconds, checks = args[0], args[1], float(args[2]), args[3:]

tmp = tempfile.mkdtemp(prefix='kos-power-')
log = os.path.join(tmp, 'serial.log')
sock = os.path.join(tmp, 'mon.sock')
cmd = ['qemu-system-aarch64', '-M', 'virt,gic-version=3', '-cpu', 'cortex-a53', '-m', '8G' if dtb else '1G',
       '-device', 'ramfb', '-display', 'none', '-serial', 'file:' + log,
       '-monitor', 'unix:' + sock + ',server,nowait', '-kernel', kernel] + qargs
if dtb:
    cmd += ['-dtb', dtb]
if net:
    cmd += ['-global', 'virtio-mmio.force-legacy=false', '-device', 'virtio-net-device,netdev=n0',
            '-netdev', 'user,id=n0', '-device', 'virtio-rng-device']
held = []
if hold:
    srv = socket.socket()
    srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    srv.bind(('127.0.0.1', hold))
    srv.listen(4)

    def serve():
        while True:
            try:
                c, _ = srv.accept()
            except OSError:
                return
            held.append(c)
            threading.Thread(target=lambda c=c: [None for _ in iter(lambda: c.recv(256), b'')],
                             daemon=True).start()
    threading.Thread(target=serve, daemon=True).start()
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

lines = [l for l in text.splitlines() if l.startswith(('power:', 'psci:', 'timer:', 'rpm', 'mpm', 'net:', 'tcphold:', 'home:', 'pro1x: DSI', 'pro1x: display', 'pro1x: standby', 'pro1x: awake'))]
for l in lines:
    print(f'{name}: {l}')
rates = [float(r) for r in re.findall(r'^power: \d+ s: \d+ wake-ups \(([\d.]+)/s\)', text, re.M)]
fail = []
if not rates:
    fail.append('no power lines')
for c in checks:
    k, _, v = c.partition('=')
    if k == 'skip-first':          # the checks after it: without the first interval
        rates = rates[1:]
        continue
    if k == 'max-rate' and rates and max(rates) > float(v):
        fail.append(f'{max(rates)} wake-ups/s > {v}')
    elif k == 'min-rate' and rates and min(rates) < float(v):
        fail.append(f'{min(rates)} wake-ups/s < {v}')
    elif k == 'has' and v not in text:
        fail.append(f'no "{v}"')
    elif k == 'hold' and hold and not held:
        fail.append('no connection to the host')
    elif k in ('core', 'cluster', 'resumed'):
        n = sum(int(x) for x in re.findall(rf'^power: \d+ s:.* {k} (\d+)', text, re.M))
        if not n:
            fail.append(f'never {k}' if k == 'resumed' else f'never in the {k} state')
if fail:
    print(f'{name}: FAILED: ' + '; '.join(fail))
    sys.exit(1)
print(f'{name}: ok')

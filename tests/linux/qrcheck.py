#!/usr/bin/env python3
"""The messenger's linking QR code in QEMU, checked (docs/messengers.md):

    qrcheck.py KERNEL SECONDS PREFIX [-- QEMU ARGUMENTS]

Runs the AArch64 kernel in QEMU's virt (the network, virtio-rng, ramfb)
with the given arguments (-append "kolibri.run=/sys/messenger ..."),
waits up to SECONDS for the messenger's debug line with the backend's QR
code (a line from the backend with a string beginning with PREFIX, e.g.
sgnl://linkdevice), takes a screenshot through QEMU's monitor, decodes it
with zbarimg and compares: the code on the screen must be the backend's
latest. Exit status 0 when they match. The serial log and the screenshot
stay in a temporary directory (printed)."""
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
kernel, seconds, prefix = args[0], float(args[1]), args[2]

tmp = tempfile.mkdtemp(prefix='kos-qr-')
log = os.path.join(tmp, 'serial.log')
sock = os.path.join(tmp, 'mon.sock')
shot = os.path.join(tmp, 'screen.ppm')
cmd = ['qemu-system-aarch64', '-M', 'virt,gic-version=3', '-cpu', 'cortex-a53', '-m', '1024',
       '-device', 'ramfb', '-display', 'none', '-global', 'virtio-mmio.force-legacy=false',
       '-device', 'virtio-net-device,netdev=n0', '-netdev', 'user,id=n0', '-device', 'virtio-rng-device',
       '-serial', 'file:' + log, '-monitor', 'unix:' + sock + ',server,nowait', '-kernel', kernel] + qargs
print('qrcheck:', tmp)
q = subprocess.Popen(cmd, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
pat = re.compile(r'"(' + re.escape(prefix) + r'[^"]*)"')
code, t0 = None, time.time()
while time.time() - t0 < seconds and code is None:
    time.sleep(1)
    text = open(log, errors='replace').read() if os.path.exists(log) else ''
    found = pat.findall(text)
    if found:
        code = found[-1]
if code:
    time.sleep(3)                      # the window redrawn
    text = open(log, errors='replace').read()
    code = pat.findall(text)[-1]
    m = socket.socket(socket.AF_UNIX)
    m.connect(sock)
    m.sendall(f'screendump {shot}\n'.encode())
    time.sleep(2)
    m.close()
q.terminate()
q.wait()
if not code:
    print('qrcheck: no QR code from the backend in', seconds, 's')
    sys.exit(1)
print('qrcheck: the backend\'s code:', code)
r = subprocess.run(['zbarimg', '-q', '--raw', shot], capture_output=True, text=True)
seen = r.stdout.strip()
print('qrcheck: on the screen:      ', seen or '(none)')
ok = seen == code
print('qrcheck:', 'the same' if ok else 'DIFFERENT')
sys.exit(0 if ok else 1)

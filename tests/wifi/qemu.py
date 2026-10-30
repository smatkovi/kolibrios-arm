#!/usr/bin/env python3
"""The Wi-Fi's kernel side in QEMU (make -C tests/wifi qemu): the Pro1X's
code paths (tests/power/mkdtb.py --wifi: a qcom,wcn3990-wifi node whose
register window is plain RAM), the fake modem's WLFW service, the
network's configuration in the initrd (tools/mkefs.py's bundle, as
pro1x-kexec.sh -w makes it):

    qemu.py KERNEL DTB INITRD SECONDS TEXT...

Reads the RAM console at 0x5ca01000 through QEMU's monitor after SECONDS
and checks that every TEXT is in it, in that order. With no firmware
behind the window the driver gets as far as the copy engines and waits
for HTC's READY: what the phone has to show next."""
import os
import socket
import subprocess
import sys
import tempfile
import time

kernel, dtb, initrd, seconds, texts = sys.argv[1], sys.argv[2], sys.argv[3], float(sys.argv[4]), sys.argv[5:]
tmp = tempfile.mkdtemp(prefix='kos-wifi-')
sock = os.path.join(tmp, 'mon.sock')
q = subprocess.Popen(['qemu-system-aarch64', '-M', 'virt,gic-version=3', '-cpu', 'cortex-a53', '-m', '8G',
                      '-device', 'ramfb', '-display', 'none', '-serial', 'null',
                      '-monitor', 'unix:' + sock + ',server,nowait', '-kernel', kernel, '-dtb', dtb,
                      '-initrd', initrd], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
time.sleep(seconds)
m = socket.socket(socket.AF_UNIX)
m.connect(sock)
out = os.path.join(tmp, 'console.bin')
m.sendall(f'pmemsave 0x5ca01000 0x40000 "{out}"\n'.encode())
time.sleep(1)
m.close()
q.terminate()
q.wait()
text = open(out, 'rb').read().split(b'\0')[0].decode(errors='replace')
for l in text.splitlines():
    if l.startswith(('wifi:', 'wlfw:', 'pro1x: Wi-Fi', 'pro1x: initrd', 'net:')):
        print('qemu:', l)
pos, missing = 0, []
for t in texts:
    i = text.find(t, pos)
    if i < 0:
        missing.append(t)
    else:
        pos = i + len(t)
if missing:
    print('qemu: FAILED: not (in order): ' + '; '.join(f'"{t}"' for t in missing))
    sys.exit(1)
print(f'qemu: {len(texts)} lines in order')

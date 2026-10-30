#!/usr/bin/env python3
"""Run the kernel in QEMU without a window, send it input and take
screenshots through QEMU's monitor (for tests):

    qemu-shot.py KERNEL OUTDIR STEP...

STEP: wait:SECONDS | shot:NAME | key:QEMUKEY (e.g. key:a, key:ret) |
      mouse:DX,DY | click | drag:DX,DY (left button held) |
      mem:ADDR,SIZE,NAME (guest physical memory to OUTDIR/NAME)
The serial output goes to OUTDIR/serial.log, screenshots to OUTDIR/NAME.png.
QEMU_MACHINE=virt runs the AArch64 kernel on QEMU's virt, QEMU_MEM sets
its memory (512M), QEMU_DTB a device tree of its own (a Pro1X test),
QEMU_INITRD an initrd (the Pro1X's EFS images, tools/mkefs.py),
QEMU_APPEND the kernel's command line, QEMU_EXTRA more of QEMU's arguments
(e.g. a profile: -plugin tools/qemu-prof/prof.so,out=FILE).
"""
import os
import shlex
import socket
import subprocess
import sys
import time

kernel, out, steps = sys.argv[1], sys.argv[2], sys.argv[3:]
os.makedirs(out, exist_ok=True)
sock = os.path.join(out, 'monitor.sock')
if os.path.exists(sock):
    os.unlink(sock)
# QEMU_MACHINE=virt: the AArch64 kernel on QEMU's virt (GICv3, ramfb)
if os.environ.get('QEMU_MACHINE') == 'virt':
    machine = ['qemu-system-aarch64', '-M', 'virt,gic-version=3', '-cpu', 'cortex-a53',
               '-m', os.environ.get('QEMU_MEM', '512M'), '-device', 'ramfb']
    if os.environ.get('QEMU_DTB'):
        machine += ['-dtb', os.environ['QEMU_DTB']]
    if os.environ.get('QEMU_INITRD'):
        machine += ['-initrd', os.environ['QEMU_INITRD']]
else:
    machine = ['qemu-system-arm', '-M', 'realview-pb-a8', '-m', '256']
if os.environ.get('QEMU_APPEND'):
    machine += ['-append', os.environ['QEMU_APPEND']]
# QEMU_EXTRA after the console's -serial: another -serial is the second UART
q = subprocess.Popen(machine + ['-rtc', 'base=localtime',
                      '-display', 'none', '-serial', 'file:' + os.path.join(out, 'serial.log'),
                      '-monitor', 'unix:' + sock + ',server,nowait', '-kernel', kernel] +
                     shlex.split(os.environ.get('QEMU_EXTRA', '')))
for _ in range(50):
    if os.path.exists(sock):
        break
    time.sleep(0.1)
m = socket.socket(socket.AF_UNIX)
m.connect(sock)
m.settimeout(0.5)


def cmd(c):
    m.sendall((c + '\n').encode())
    time.sleep(0.15)
    try:
        while m.recv(65536):
            pass
    except socket.timeout:
        pass


try:
    for s in steps:
        kind, _, arg = s.partition(':')
        if kind == 'wait':
            time.sleep(float(arg))
        elif kind == 'shot':
            ppm = os.path.join(out, arg + '.ppm')
            cmd('screendump ' + ppm)
            time.sleep(0.3)
            subprocess.run(['magick', ppm, os.path.join(out, arg + '.png')], check=False)
            os.unlink(ppm)
        elif kind == 'mem':
            addr, size, name = arg.split(',')
            cmd(f'pmemsave {addr} {size} "{os.path.join(out, name)}"')
            time.sleep(0.5)
        elif kind == 'key':
            cmd('sendkey ' + arg)
        elif kind == 'mouse':
            dx, dy = arg.split(',')
            cmd(f'mouse_move {dx} {dy}')
        elif kind == 'click':
            cmd('mouse_button 1')
            time.sleep(0.1)
            cmd('mouse_button 0')
        elif kind == 'drag':
            dx, dy = arg.split(',')
            cmd('mouse_button 1')
            for _ in range(4):
                cmd(f'mouse_move {int(dx) // 4} {int(dy) // 4}')
            cmd('mouse_button 0')
finally:
    cmd('quit')
    q.wait(timeout=5)

#!/usr/bin/env python3
"""tests/threads: threads (51), IPC (60), named shared memory (68.22,
68.23) and libraries (68.19) in QEMU (make -C tests/threads).

    run.py [KERNEL...]      realview (the ARMv7 kernel), arm64 (the AArch64
                            kernel with AArch32 programs); both when none given

apps/threadtest runs from the autorun and writes its checks to the debug
board; the static build (DLL=0, the libraries linked in) is checked
too, on the ARMv7 kernel.
"""
import os
import shutil
import subprocess
import sys

TOP = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
HERE = os.path.join(TOP, 'tests', 'threads')
OUT = '/tmp/kolibri-threads-test'
failed = []


def make(platform, *extra):
    args = ['make', '-C', TOP, '-j8', 'AUTORUN=' + os.path.join(HERE, 'autorun.threadtest'), *extra]
    if platform != 'realview':
        args.append('PLATFORM=' + platform)
    subprocess.run(args, check=True, stdout=subprocess.DEVNULL)


def qemu(platform, out, seconds):
    env = dict(os.environ)
    if platform == 'arm64':
        env['QEMU_MACHINE'] = 'virt'
        env['QEMU_EXTRA'] = ('-global virtio-mmio.force-legacy=false -device virtio-keyboard-device '
                             '-device virtio-tablet-device')
        kernel = os.path.join(TOP, 'build/arm64/kernel.bin')
    else:
        kernel = os.path.join(TOP, 'build/realview/kernel.elf')
    shutil.rmtree(out, ignore_errors=True)
    subprocess.run([sys.executable, os.path.join(TOP, 'scripts/qemu-shot.py'), kernel, out,
                    f'wait:{seconds}', 'shot:end'], env=env, check=True,
                   stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    return open(os.path.join(out, 'serial.log'), 'rb').read().decode('latin-1')


def run(platform, what, *extra):
    make(platform, *extra)
    log = qemu(platform, f'{OUT}/{what}', 25)
    n = 0
    for line in log.splitlines():
        if 'threadtest: FAIL' in line or 'panic' in line:
            print('     ' + line)
        n += 'threadtest: ok' in line
    ok = 'threadtest: done 0 failed' in log
    print(('ok   ' if ok else 'FAIL ') + f'threadtest on {what} ({n} checks){" (DLL=0)" if extra else ""}')
    if not ok:
        failed.append(what)


kernels = sys.argv[1:] or ['realview', 'arm64']
for k in kernels:
    run(k, k)
if 'realview' in kernels:
    run('realview', 'realview-static', 'DLL=0')
    make('realview')      # back to the libraries loaded
sys.exit(1 if failed else 0)

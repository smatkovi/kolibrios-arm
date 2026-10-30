#!/usr/bin/env python3
"""tests/apps: KolibriOS's ported programs in QEMU (make -C tests/apps).

    run.py [--img DIR] TEST...     TEST: fstest, shell, terminal, kfar, t_edit (all: none given)

Each builds the kernel with an autorun of its own (tests/apps/autorun.*),
runs it through scripts/qemu-shot.py, types with keys.py's steps and
checks the debug output (fstest's lines; the shell runs fstest's
"expect" to look at the files it made) or what reaches a socket (the
terminal on realview's UART1). Screenshots go to the output directory
(/tmp/kolibri-apps-test), or with --img to the documentation's images.
"""
import os
import shutil
import socket
import subprocess
import sys
import threading
import time

TOP = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
OUT = '/tmp/kolibri-apps-test'
HERE = os.path.join(TOP, 'tests', 'apps')
failed = []


def make(platform, autorun):
    args = ['make', '-C', TOP, '-j8', 'AUTORUN=' + os.path.join(HERE, autorun)]
    if platform != 'realview':
        args.append('PLATFORM=' + platform)
    subprocess.run(args, check=True, stdout=subprocess.DEVNULL)


def keys(*a):
    return subprocess.run([sys.executable, os.path.join(HERE, 'keys.py'), *a], check=True,
                          capture_output=True, text=True).stdout.split()


def qemu(platform, out, steps, extra='', append=''):
    env = dict(os.environ)
    if platform == 'arm64':
        env['QEMU_MACHINE'] = 'virt'
        extra = ('-global virtio-mmio.force-legacy=false -device virtio-keyboard-device '
                 '-device virtio-tablet-device ' + extra)
        kernel = os.path.join(TOP, 'build/arm64/kernel.bin')
    else:
        kernel = os.path.join(TOP, 'build/realview/kernel.elf')
    env['QEMU_EXTRA'] = extra
    if append:
        env['QEMU_APPEND'] = append
    shutil.rmtree(out, ignore_errors=True)
    subprocess.run([sys.executable, os.path.join(TOP, 'scripts/qemu-shot.py'), kernel, out, *steps],
                   env=env, check=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    return open(os.path.join(out, 'serial.log'), 'rb').read().decode('latin-1')


def check(ok, what):
    print(('ok   ' if ok else 'FAIL ') + what)
    if not ok:
        failed.append(what)


def test_fstest(img):
    for platform in ('realview', 'arm64'):
        make(platform, 'autorun.fstest')
        log = qemu(platform, f'{OUT}/fstest-{platform}', ['wait:8'])
        for line in log.splitlines():
            if 'fstest: FAIL' in line:
                print('     ' + line)
        check('fstest: done 0 failed' in log, f'fstest on {platform}')


def test_shell(img):
    make('realview', 'autorun.shell')
    steps = keys('wait:3', 'mkdir /tmp0/1/w/sub', 'ret', 'cd /tmp0/1/w', 'ret', 'echo hello > a.txt', 'ret',
                 'cp a.txt b.txt', 'ret', 'ren b.txt c.txt', 'ret', 'mv c.txt sub/d.txt', 'ret', 'touch e',
                 'ret', 'ls', 'ret', 'more sub/d.txt', 'ret', 'wait:1', 'shot:shell-files',
                 'fstest expect /tmp0/1/w/a.txt hello', 'ret', 'fstest expect /tmp0/1/w/sub/d.txt hello', 'ret',
                 'fstest expect /tmp0/1/w/c.txt -', 'ret', 'rm a.txt', 'ret', 'fstest expect /tmp0/1/w/a.txt -',
                 'ret', 'clear', 'ret', 'kfetch', 'ret', 'wait:2', 'shot:shell-kfetch')
    log = qemu('realview', f'{OUT}/shell', steps)
    n_ok = log.count('fstest: expect ok')
    check(n_ok == 4 and 'expect FAIL' not in log, f'shell: mkdir, cd, echo >, cp, ren, mv, rm ({n_ok} of 4 files as expected)')
    if img:
        for n in ('shell-files', 'shell-kfetch'):
            shutil.copy(f'{OUT}/shell/{n}.png', os.path.join(img, n + '.png'))


def test_terminal(img):
    make('realview', 'autorun.terminal')
    port, got = 18000 + os.getpid() % 1000, []

    def peer():
        for _ in range(100):
            try:
                s = socket.create_connection(('127.0.0.1', port))
                break
            except OSError:
                time.sleep(0.2)
        else:
            return
        s.settimeout(0.5)
        t0 = time.time()
        sent = False
        while time.time() - t0 < 30:
            if not sent and time.time() - t0 > 10:
                s.sendall(b'Hello from the other end\r\n')
                sent = True
            try:
                d = s.recv(4096)
                if d:
                    got.append(d)
            except socket.timeout:
                pass
            except OSError:
                break

    t = threading.Thread(target=peer)
    t.start()
    steps = keys('wait:4', 'shot:terminal-settings', 'backspace', '1', 'ret', 'wait:2', 'typed on ARM', 'ret',
                 'wait:8', 'shot:terminal-session')
    qemu('realview', f'{OUT}/terminal', steps, extra=f'-serial tcp:127.0.0.1:{port},server=on,wait=off')
    t.join()
    check(b'typed on ARM\r' in b''.join(got), 'terminal: keys reach UART1')
    if img:
        for n in ('terminal-settings', 'terminal-session'):
            shutil.copy(f'{OUT}/terminal/{n}.png', os.path.join(img, n + '.png'))


def test_kfar(img):
    # the left panel /sys (icons/, settings/, @hildon, @hstatus, @taskbar,
    # ...), the right one /tmp0/1: two copies (F5), a folder (F7), a
    # delete (F8); fstest (autorun.kfar) looks at /tmp0/1 at 45 s
    make('realview', 'autorun.kfar')
    steps = keys('wait:3', 'down', 'down', 'down', 'f5', 'wait:1', 'ret', 'wait:2', 'down', 'f5', 'wait:1', 'ret',
                 'wait:2', 'shot:kfar-copied', 'tab', 'f7', 'wait:1', 'newdir', 'ret', 'wait:2', 'home', 'down', 'f8',
                 'wait:1', 'shot:kfar-delete', 'ret', 'wait:2', 'shot:kfar-done', 'f3', 'wait:2', 'shot:kfar-view',
                 'esc', 'wait:25')
    log = qemu('realview', f'{OUT}/kfar', steps)
    n_ok = log.count('fstest: expect ok')
    check(n_ok == 3 and 'expect FAIL' not in log, f'kfar: F5 copy, F7 folder, F8 delete ({n_ok} of 3 as expected)')
    if img:
        shutil.copy(f'{OUT}/kfar/kfar-delete.png', os.path.join(img, 'kfar-delete.png'))


class Mouse:
    """qemu-shot.py's relative mouse steps to screen positions (realview's
    PS/2 mouse: steps of at most 100 pixels, the first move to the corner)"""

    def __init__(self):
        self.x = self.y = None

    def at(self, x, y):
        out = []
        if self.x is None:
            out += ['mouse:-200,-200'] * 6
            self.x = self.y = 0
        dx, dy = x - self.x, y - self.y
        while dx or dy:
            sx, sy = max(-100, min(100, dx)), max(-100, min(100, dy))
            out.append(f'mouse:{sx},{sy}')
            dx, dy = dx - sx, dy - sy
        self.x, self.y = x, y
        return out


def test_t_edit(img):
    # t_edit with t_edit.ini (Hildon's look: the window from y 56): typed
    # text, undo, replace all in the replace panel, a word selected,
    # copied and pasted, saved as /tmp0/1/t.ini through the save dialog;
    # fstest (autorun.t_edit) looks at the file at 75 s
    make('realview', 'autorun.t_edit')
    m = Mouse()
    steps = (keys('wait:5', 'ab', 'xyz', 'ctrl-z', 'ctrl-z', 'ctrl-z', 'ctrl-h', 'wait:1') +
             m.at(60, 133) + ['click'] + keys('Window') + m.at(60, 226) + ['click'] + keys('Fenster') +
             m.at(11, 188) + ['click'] +          # the option "At first": the cursor is after the undone text
             m.at(40, 275) + ['click', 'wait:2', 'shot:t_edit-replace'] + keys('ret', 'wait:1') +
             m.at(182, 139) + ['click', 'wait:0.5'] +
             keys('shift-right', 'shift-right', 'ctrl-c', 'end', 'ctrl-v', 'wait:1', 'shot:t_edit-pasted',
                  'ctrl-shift-s', 'wait:2', 'tab', 'ctrl-a', '/tmp0/1', 'ret', 'wait:1', 'tab', 'ctrl-a', 't.ini',
                  'wait:0.5', 'shot:t_edit-save', 'ret', 'wait:2', 'shot:t_edit-saved', 'wait:40'))
    log = qemu('realview', f'{OUT}/t_edit', steps)
    check('fstest: expect ok' in log and 'expect FAIL' not in log,
          't_edit: typing, undo, replace all, copy and paste, save as /tmp0/1/t.ini')
    if img:
        shutil.copy(f'{OUT}/t_edit/t_edit-replace.png', os.path.join(img, 't_edit-replace.png'))


args = sys.argv[1:]
img = None
if args[:1] == ['--img']:
    img = os.path.join(TOP, args[1])
    args = args[2:]
tests = {'fstest': test_fstest, 'shell': test_shell, 'terminal': test_terminal, 'kfar': test_kfar,
         't_edit': test_t_edit}
for name in args or tests:
    tests[name](img)
make('realview', '../../apps/autorun')
make('arm64', '../../apps/autorun')
print('tests/apps: ' + ('all passed' if not failed else f'{len(failed)} failed'))
sys.exit(1 if failed else 0)

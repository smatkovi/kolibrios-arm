#!/usr/bin/env python3
"""The messenger's chats in QEMU against the fake backend, checked by
screenshots (docs/messengers.md):

    chatcheck.py KERNEL SERVICE [OUTDIR]

KERNEL: build/arm64/kernel.bin with tests/messenger/fakepeer as the
backends (make test-chats builds it); SERVICE: whatsapp, telegram or
signal. SERVICE telegram-purple instead: the real purple-host (libpurple,
tdlib-purple) with its test protocol, the kernel built with
/etc/purple-host-test on its ramdisk (make test-chats PURPLE_HOST=...):
its first messages, sending through libpurple (the message back with
SendText's id, its Receipt, the reply), the chats kept and the account
logged in again by itself after the restart. QEMU's virt runs it with a keyboard (virtio-keyboard-device, typed
through the monitor's sendkey) and the messenger started for SERVICE; the
screenshots (OUTDIR, a temporary directory by default) are searched for
the texts that must be there, drawn here from the kernel's own fonts
(kernel/fonts/font8x16.c, font6x10.c) in the colours the messenger uses:

  1. the chat list: the fixtures' names (from GetContacts, GetGroups), the
     live message of the third contact with its unread count;
  2. a chat opened (End, Enter): its messages (History), a "\\u003c"
     escape decoded;
  3. a message typed and sent (Enter): the fake backend's console line for
     its SendText, the bubble, its receipts up to "read", the reply that
     comes live into the open chat;
  4. back in the list (Escape): the unread count of the third contact gone
     up meanwhile, the open chat read;
  5. the messenger ended (Ctrl+Q) and started again through the Hildon
     shell's launcher (Enter, typing "mes", Enter, the service's number):
     the chat list as it was kept (the reply, which the new backend does
     not know).

Exit status 0 when everything was found."""
import os
import re
import socket
import subprocess
import sys
import tempfile
import time

import numpy as np

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
kernel, service = sys.argv[1], sys.argv[2]
# telegram-purple: the real purple-host with its test protocol (the kernel
# built with /etc/purple-host-test on its ramdisk), logging in as "tester"
real = service == 'telegram-purple'
if real:
    service = 'telegram'

out = sys.argv[3] if len(sys.argv) > 3 else tempfile.mkdtemp(prefix='kos-chats-' + service + '-')
os.makedirs(out, exist_ok=True)
NUMBER = {'whatsapp': '1', 'telegram': '2', 'signal': '3'}[service]
TAG = {'whatsapp': '[wa]', 'telegram': '[tg]', 'signal': '[sg]'}[service]

WHITE, DIM, STAMP = (255, 255, 255), (0x9a, 0xa0, 0xa8), (0xb8, 0xc4, 0xd0)


# --- the fonts, as gfx_text draws them ------------------------------------------

def font(name):
    src = open(os.path.join(ROOT, 'kernel', 'fonts', name)).read()
    return bytes(int(h, 16) for h in re.findall(r'0x([0-9a-fA-F]{2})', src.split('{', 1)[1]))


F16, F10 = font('font8x16.c'), font('font6x10.c')
HI437 = [0xc7, 0xfc, 0xe9, 0xe2, 0xe4, 0xe0, 0xe5, 0xe7, 0xea, 0xeb, 0xe8, 0xef, 0xee, 0xec, 0xc4, 0xc5,
         0xc9, 0xe6, 0xc6, 0xf4, 0xf6, 0xf2, 0xfb, 0xf9, 0xff, 0xd6, 0xdc, 0xa2, 0xa3, 0xa5, 0x20a7, 0x192,
         0xe1, 0xed, 0xf3, 0xfa, 0xf1, 0xd1, 0xaa, 0xba, 0xbf, 0x2310, 0xac, 0xbd, 0xbc, 0xa1, 0xab, 0xbb]


def cp437(ch):
    u = ord(ch)
    if u < 128:
        return u
    if u in HI437:
        return 128 + HI437.index(u)
    return {0xdf: 0xe1, 0xb0: 0xf8}.get(u, ord('?'))


def mask(text, size=16, scale=1):
    f, fw, fh = (F16, 8, 16) if size == 16 else (F10, 6, 10)
    m = np.zeros((fh, fw * len(text)), bool)
    for i, ch in enumerate(text):
        g = f[cp437(ch) * fh:(cp437(ch) + 1) * fh]
        for r in range(fh):
            for c in range(fw):
                m[r, i * fw + c] = bool(g[r] & (0x80 >> c))
    return np.kron(m, np.ones((scale, scale), bool)) if scale > 1 else m


def find(img, text, color, size=16, scale=1, xmin=0):
    """where text is on the screen in that colour: its pixels the colour,
    the others not (the messenger draws on plain backgrounds)"""
    m = mask(text, size, scale)
    fg = np.all(img == color, axis=2)
    h, w = m.shape
    ys, xs = np.nonzero(m)
    ay, ax = ys[0], xs[0]
    hits = []
    for y, x in zip(*np.nonzero(fg)):
        y0, x0 = y - ay, x - ax
        if x0 < xmin or y0 < 0 or x0 < 0 or y0 + h > fg.shape[0] or x0 + w > fg.shape[1]:
            continue
        if np.array_equal(fg[y0:y0 + h, x0:x0 + w], m):
            hits.append((int(x0), int(y0)))
    return hits


# --- QEMU -------------------------------------------------------------------------

log = os.path.join(out, 'serial.log')
sock = os.path.join(out, 'mon.sock')
for p in (log, sock):
    if os.path.exists(p):
        os.unlink(p)
cmd = ['qemu-system-aarch64', '-M', 'virt,gic-version=3', '-cpu', 'cortex-a53', '-m', '1024',
       '-device', 'ramfb', '-display', 'none', '-global', 'virtio-mmio.force-legacy=false',
       '-device', 'virtio-rng-device', '-device', 'virtio-keyboard-device',
       '-serial', 'file:' + log, '-monitor', 'unix:' + sock + ',server,nowait', '-kernel', kernel,
       '-append', 'kolibri.run=/sys/messenger kolibri.args=' + service + (',tester' if real else '')]
print('chatcheck:', service + (' (purple-host, its test protocol)' if real else ''), out)
q = subprocess.Popen(cmd, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
for _ in range(100):
    if os.path.exists(sock):
        break
    time.sleep(0.1)
mon = socket.socket(socket.AF_UNIX)
mon.connect(sock)
mon.settimeout(0.3)


def monitor(c):
    mon.sendall((c + '\n').encode())
    time.sleep(0.12)
    try:
        while mon.recv(65536):
            pass
    except socket.timeout:
        pass


def serial():
    return open(log, errors='replace').read() if os.path.exists(log) else ''


def wait(pattern, seconds):
    t0 = time.time()
    while time.time() - t0 < seconds:
        m = re.search(pattern, serial())
        if m:
            return m
        time.sleep(0.5)
    return None


KEYS = {' ': 'spc', '.': 'dot', ',': 'comma', '-': 'minus', '/': 'slash', '\n': 'ret'}


def type_text(s):
    for ch in s:
        k = KEYS.get(ch, ch.lower())
        monitor('sendkey ' + ('shift-' + k if ch.isupper() else k))


shots = 0


def shot(name):
    global shots
    shots += 1
    ppm = os.path.join(out, f'{shots}-{name}.ppm')
    monitor('screendump ' + ppm)
    for _ in range(20):
        time.sleep(0.25)
        if os.path.exists(ppm) and os.path.getsize(ppm) > 800 * 480 * 3:
            break
    data = open(ppm, 'rb').read()
    parts = data.split(b'\n', 3)
    w, h = map(int, parts[1].split())
    img = np.frombuffer(parts[3][:w * h * 3], np.uint8).reshape(h, w, 3)
    subprocess.run(['magick', ppm, ppm[:-4] + '.png'], check=False)
    os.unlink(ppm)
    return img


failed = 0


def check(ok, what):
    global failed
    print('chatcheck:', 'ok  ' if ok else 'FAIL', what)
    if not ok:
        failed += 1


def has(img, text, color=WHITE, size=16, scale=1, xmin=0):
    return bool(find(img, text, color, size, scale, xmin))


def lives():
    return len(set(re.findall(r'Live message (\d+)', serial())))


def restart():
    """Ctrl+Q, then through the launcher: Enter, "mes", Enter, the service"""
    monitor('sendkey ctrl-q')
    check(wait(r'messenger: \d+ chats kept in /home/user/.local/share/messenger/' + service, 15) is not None,
          'the chats written when it ended')
    time.sleep(2)
    monitor('sendkey ret')
    time.sleep(1.5)
    type_text('mes')
    time.sleep(0.5)
    monitor('sendkey ret')
    time.sleep(3)
    img = shot('again')
    check(has(img, 'Which service?', DIM), 'started again from the launcher: it asks which service')
    monitor('sendkey ' + NUMBER)
    m = wait(r'messenger: (\d+) chats from /home/user/.local/share/messenger/' + service, 15)
    check(m is not None and int(m.group(1)) >= 2, 'the chats read back (%s)' % (m.group(1) if m else 'none'))
    time.sleep(2)
    return shot('kept')


def purple():
    # 1. the list: the test protocol's first messages through libpurple
    check(wait(r'\[tg\] .*"Hallo from the test protocol"', 120) is not None, 'logged in, the first messages')
    time.sleep(2)
    img = shot('list')
    for name in ('Anna Berger', 'Chor Probe'):
        check(has(img, name, scale=2), f'the list: "{name}"')
    check(has(img, 'Hallo from the test protocol'), 'the list: Anna\'s message, unread')
    check(has(img, 'Bernhard Koller: Probe morgen 19:00'), 'the list: the group\'s, with its sender')
    check(has(img, '1', xmin=700), 'the list: an unread count')
    # 2. Anna's chat, the newest
    monitor('sendkey home')
    monitor('sendkey ret')
    time.sleep(2)
    img = shot('chat')
    check(has(img, 'Anna Berger', scale=2) and has(img, 'Hallo from the test protocol'), 'the chat')
    # 3. sending through libpurple: back with SendText's id, the Receipt, the reply
    type_text('hello kolibri')
    monitor('sendkey ret')
    m = wait(r'\[tg\] \{"id":\d+,"result":\["(tg-[0-9a-f-]+)"\]\}', 20)
    check(m is not None, 'SendText: purple-host\'s id')
    mid = m.group(1) if m else 'none'
    check(wait(r'\[tg\] \{"signal":"Message","args":\["%s","id1001","tester","",\d+,true,"text","hello kolibri"' % mid,
               20) is not None, 'the message back from libpurple with that id')
    check(wait(r'\[tg\] \{"signal":"Receipt","args":\["id1001","",\["%s"\],"server"' % mid, 20) is not None,
          'its Receipt')
    check(wait(r'\[tg\] .*"Re: hello kolibri"', 20) is not None, 'the reply')
    time.sleep(2)
    img = shot('sent')
    check(has(img, 'hello kolibri') and has(img, 'Re: hello kolibri'), 'the chat: sent, the reply')
    check(has(img, ' sent', STAMP), 'the chat: the message "sent" (once, not twice)')
    check(len(find(img, 'hello kolibri', WHITE)) == 2, 'the chat: "hello kolibri" in two bubbles only (%d)' %
          len(find(img, 'hello kolibri', WHITE)))
    # 4. back
    monitor('sendkey esc')
    time.sleep(1.5)
    img = shot('back')
    check(has(img, 'Re: hello kolibri', DIM), 'the list: Anna\'s chat read')
    # 5. again: kept, and logged in again by itself (accounts.xml)
    img = restart()
    check(has(img, 'Anna Berger', scale=2) and has(img, 'Chor Probe', scale=2), 'the list kept')
    monitor('sendkey home')
    monitor('sendkey ret')
    time.sleep(2)
    img = shot('kept-chat')
    check(has(img, 'hello kolibri') and has(img, 'Re: hello kolibri'),
          'Anna\'s chat kept: what was sent and the reply (the test protocol\'s new greeting after them)')
    check(wait(r'\[tg\] \{"id":\d+,"result":\["(connecting|connected)","tester"\]\}', 20) is not None,
          'purple-host started again: the account logged in by itself')


def fake():
    # 1. the chat list
    check(wait(r'Live message 1"', 90) is not None, 'the first live message from the backend')
    time.sleep(2)
    img = shot('list')
    for name in ('Anna Berger', 'Bernhard Koller', 'Chor Probe', 'Clara'):
        check(has(img, name, scale=2), f'the list: "{name}"')
    n = lives()
    check(has(img, 'Live message %d' % n) or has(img, 'Live message %d' % (n - 1)),
          'the list: the live message, unread (white)')
    check(has(img, 'Super, bis dann <3', DIM), 'the list: Anna\'s last message, read (dim), "\\u003c" decoded')
    badge = [k for k in range(1, n + 2) if has(img, str(k), xmin=700)]
    check(bool(badge), f'the list: an unread count ({badge})')

    # 2. a chat: Anna's, the last in the list
    monitor('sendkey end')
    monitor('sendkey ret')
    time.sleep(2)
    img = shot('chat')
    check(has(img, 'Anna Berger', scale=2), 'the chat: its name')
    for t in ('Hallo! Kommst du morgen zur Probe?', 'Ja, um 19 Uhr.', 'Super, bis dann <3'):
        check(has(img, t), f'the chat: "{t}"')

    # 3. sending
    type_text('hello kolibri')
    time.sleep(0.5)
    img = shot('typed')
    check(has(img, 'hello kolibri_'), 'the line to write in')
    monitor('sendkey ret')
    check(wait(r'fakepeer: SendText \S+ "hello kolibri"', 20) is not None, 'the backend got SendText')
    check(wait(re.escape(TAG) + r'.*"Re: hello kolibri"', 20) is not None, 'the reply from the backend')
    time.sleep(2)
    img = shot('sent')
    check(has(img, 'hello kolibri'), 'the chat: the message sent')
    check(has(img, 'Re: hello kolibri'), 'the chat: the reply, live')
    check(has(img, ' read', STAMP), 'the chat: the message sent "read" (its receipts)')

    # 4. back: the third contact's unread count has gone up
    before = lives()
    wait(r'Live message %d"' % (before + 1), 30)
    monitor('sendkey esc')
    time.sleep(2)
    lo = lives()
    img = shot('back')
    hi = lives()
    counts = [k for k in range(lo - 1, hi + 1) if has(img, str(k), xmin=700)]
    check(any(k >= 2 for k in counts), f'the list: Clara\'s unread count up to {counts} ({lo}-{hi} sent)')
    check(has(img, 'Re: hello kolibri', DIM), 'the list: Anna\'s chat read, its last message')

    # 5. the messenger again: the chats kept
    img = restart()
    check(has(img, 'Re: hello kolibri', DIM), 'the list: the reply kept (the new backend does not know it)')
    check(has(img, 'Anna Berger', scale=2) and has(img, 'Clara', scale=2), 'the list: the names kept')


try:
    purple() if real else fake()
finally:
    q.terminate()
    q.wait()

print('chatcheck: %s: %s, screenshots in %s' % (service, 'all found' if not failed else '%d FAILED' % failed, out))
sys.exit(1 if failed else 0)

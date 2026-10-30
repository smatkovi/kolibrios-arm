#!/usr/bin/env python3
"""The messenger's backends on the PC, without an account
(docs/messengers.md): each speaks the JSON lines of yappari-wa -stdio,
in the shapes the messenger reads.

    hosttest.py KIND PROGRAM

KIND: fakepeer-whatsapp, fakepeer-telegram, fakepeer-signal (the fake
backend: everything, from its fixtures); purple-test (purple-host with
its test protocol, PURPLE_HOST_TEST=1: libpurple's conversations, the
buddy list, the first messages of a person and a group, a message sent
coming back with SendText's id, its Receipt, the other side's reply); purple, signal,
yappari (the real backends logged out: Status, the empty lists, SendText
refused, nothing reaching the network). PROGRAM runs under qemu-aarch64
unless the PC is an AArch64 one, with HOME in a temporary directory."""
import json
import os
import platform
import queue
import subprocess
import sys
import tempfile
import threading
import time

kind, program = sys.argv[1], sys.argv[2]
home = tempfile.mkdtemp(prefix='kos-host-')
env = dict(os.environ, HOME=home, XDG_DATA_HOME=home + '/data', XDG_CACHE_HOME=home + '/cache')
if kind == 'purple-test':
    env['PURPLE_HOST_TEST'] = '1'
cmd = [program, '-stdio']
if kind.startswith('fakepeer'):
    cmd += ['-service', kind.split('-')[1], '-every', '2']
if platform.machine() != 'aarch64':
    cmd = ['qemu-aarch64'] + cmd
seen = []                                   # every object that came, in order
lines = queue.Queue()


def start():
    global p
    p = subprocess.Popen(cmd, stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=open(home + '/stderr', 'a'),
                         env=env, text=True, bufsize=1)
    threading.Thread(target=lambda: [lines.put(l) for l in p.stdout], daemon=True).start()


def stop():
    p.stdin.close()
    try:
        p.wait(timeout=10)
    except subprocess.TimeoutExpired:
        p.kill()


start()
next_id = [0]
failed = 0


def check(ok, what):
    global failed
    print('hosttest: %s: %s %s' % (kind, 'ok  ' if ok else 'FAIL', what))
    if not ok:
        failed += 1


def pump(seconds):
    t0 = time.time()
    while time.time() - t0 < seconds:
        try:
            seen.append(json.loads(lines.get(timeout=0.2)))
        except queue.Empty:
            pass
        except ValueError as e:
            check(False, 'a line that is not JSON: %s' % e)


def call(method, *args, seconds=15):
    next_id[0] += 1
    i = next_id[0]
    p.stdin.write(json.dumps({'id': i, 'call': method, 'args': list(args)}) + '\n')
    p.stdin.flush()
    t0 = time.time()
    while time.time() - t0 < seconds:
        for o in seen:
            if o.get('id') == i:
                return o
        pump(0.3)
    return {}


def signals(name):
    return [o['args'] for o in seen if o.get('signal') == name]


def wait_signal(name, pred, seconds=10):
    t0 = time.time()
    while time.time() - t0 < seconds:
        for a in signals(name):
            if pred(a):
                return a
        pump(0.3)
    return None


def is_message(a):
    """id, chat, sender, pushname, time, fromMe, kind, text, mime, size"""
    types = (str, str, str, str, int, bool, str, str, str, int)
    return isinstance(a, list) and len(a) == 10 and all(isinstance(v, t) for v, t in zip(a, types))


def is_receipt(a):
    return (isinstance(a, list) and len(a) == 5 and isinstance(a[0], str) and isinstance(a[1], str) and
            isinstance(a[2], list) and all(isinstance(x, str) for x in a[2]) and isinstance(a[3], str) and
            isinstance(a[4], int))


def list_of(r, n):
    v = r.get('result')
    return (isinstance(v, list) and len(v) == 1 and isinstance(v[0], list) and
            all(isinstance(e, list) and len(e) == n and all(isinstance(x, str) for x in e) for e in v[0]))


try:
    if kind.startswith('fakepeer'):
        r = call('Status')
        check(r.get('result', [''])[0] == 'connected', 'Status: connected %s' % r.get('result'))
        pump(1)
        h = signals('History')
        check(len(h) == 6 and all(is_message(a) for a in h), 'six History signals of ten arguments')
        r = call('GetContacts')
        check(list_of(r, 3) and len(r['result'][0]) == 3, 'GetContacts: [[id, name, push name]...]')
        r = call('GetGroups')
        check(list_of(r, 2) and r['result'][0][0][1] == 'Chor Probe', 'GetGroups: [[id, subject]]')
        chat = h[0][1]
        r = call('SendText', '', chat, 'hi')
        mid = (r.get('result') or [None])[0]
        check(isinstance(mid, str) and mid, 'SendText: its id (%s)' % mid)
        a = wait_signal('Receipt', lambda a: is_receipt(a) and a[2] == [mid] and a[3] == 'read', 8)
        check(a is not None and [x[3] for x in signals('Receipt') if x[2] == [mid]] == ['server', 'delivered', 'read'],
              'its Receipts: server, delivered, read')
        a = wait_signal('Message', lambda a: is_message(a) and a[7] == 'Re: hi', 8)
        check(a is not None and a[1] == chat and not a[5], 'the reply as a Message in that chat')
        check(wait_signal('Message', lambda a: a[7].startswith('Live message'), 8) is not None, 'a live Message')
        check(call('MarkRead', chat, '', [mid]).get('result') == [], 'MarkRead: accepted')
        check('error' in call('SendText', 'x', 'y'), 'SendText with two arguments: refused (the fake wants three)')
    elif kind == 'purple-test':
        check(call('Login', 'tester').get('result') == [], 'Login: accepted')
        check(wait_signal('StateChanged', lambda a: a[0] == 'connected') is not None, 'StateChanged: connected')
        r = call('Status')
        check(r.get('result') == ['connected', 'tester'], 'Status: connected, the account (%s)' % r.get('result'))
        r = call('GetContacts')
        check(list_of(r, 3) and sorted(e[0] for e in r['result'][0]) == ['id1001', 'id1002'] and
              'Anna Berger' in [e[1] for e in r['result'][0]], 'GetContacts: the buddy list (%s)' % r.get('result'))
        r = call('GetGroups')
        check(list_of(r, 2) and r['result'][0] == [['chat-100', 'Chor Probe']], 'GetGroups: the chat (%s)' % r.get('result'))
        text = 'a < b & "c"'
        r = call('SendText', '', 'id1001', text)
        mid = (r.get('result') or [None])[0]
        check(isinstance(mid, str) and mid.startswith('tg-'), 'SendText: an id made here (%s)' % mid)
        a = wait_signal('Message', lambda a: is_message(a) and a[5] and a[1] == 'id1001')
        check(a is not None and a[0] == mid and a[7] == text, 'the message back from the protocol: its id, its text unescaped')
        check(wait_signal('Receipt', lambda a: is_receipt(a) and a[2] == [mid] and a[3] == 'server') is not None,
              'its Receipt: server')
        a = wait_signal('Message', lambda a: is_message(a) and not a[5] and a[7] == 'Re: ' + text)
        check(a is not None and a[2] == 'id1001' and a[3] == 'Anna Berger' and a[0] != mid,
              'the reply: sender, name, an id of its own (%s)' % a)
        a = wait_signal('Message', lambda a: is_message(a) and a[1] == 'chat-100' and not a[5])
        check(a is not None and a[2] == 'Bernhard Koller' and a[7] == 'Probe morgen 19:00',
              'the group\'s first message: its conversation, the sender')
        a = wait_signal('Message', lambda a: is_message(a) and a[7] == 'Hallo from the test protocol')
        check(a is not None and a[1] == 'id1001' and a[3] == 'Anna Berger', 'Anna\'s first message')
        r = call('SendText', '', 'chat-100', 'hi group')
        gid = (r.get('result') or [None])[0]
        a = wait_signal('Message', lambda a: is_message(a) and a[1] == 'chat-100' and a[7] == 'Re: hi group')
        check(gid and a is not None and a[2] == 'Anna Berger', 'the group: sent, replied')
        check(wait_signal('Message', lambda a: a[0] == gid and a[5] and a[1] == 'chat-100') is not None,
              'the group: the message back with SendText\'s id')
        r = call('SendText', 'id1002', 'two arguments')
        check(isinstance((r.get('result') or [None])[0], str), 'SendText(chat, text) too')
        check(call('MarkRead', 'id1001', '', [mid]).get('result') == [], 'MarkRead: accepted (an array argument)')
        check(all(is_message(a) for a in signals('Message')), 'every Message of ten arguments')
        # started again on the same ~/.purple: the account logs in by itself
        stop()
        seen.clear()
        start()
        r = call('Status')
        check(wait_signal('StateChanged', lambda a: a[0] == 'connected') is not None and
              call('Status').get('result') == ['connected', 'tester'],
              'started again: the account from accounts.xml logged in by itself')
    else:
        r = call('Status', seconds=30)
        res = r.get('result') or ['']
        check(res[0] == 'logged-out', 'Status: logged-out (%s)' % res)
        if kind in ('purple', 'yappari'):
            check(len(res) == 2 and isinstance(res[1], str), 'Status: state and account')
        r = call('GetContacts')
        check(list_of(r, 3) and r['result'][0] == [], 'GetContacts: an empty list')
        r = call('GetGroups')
        check(list_of(r, 2) and r['result'][0] == [] or 'error' in r, 'GetGroups: an empty list or not linked')
        r = call('SendText', '', 'someone', 'hi')
        check('error' in r, 'SendText(id, chat, text): refused (%s)' % r.get('error'))
        r = call('MarkRead', 'someone', '', ['1'])
        check('result' in r or 'error' in r, 'MarkRead: answered')
        r = call('NoSuchMethod')
        check('error' in r, 'an unknown method: an error')
finally:
    stop()

print('hosttest: %s: %s' % (kind, 'all ok' if not failed else '%d FAILED (stderr: %s/stderr)' % (failed, home)))
sys.exit(1 if failed else 0)

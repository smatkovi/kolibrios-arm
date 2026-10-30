#!/usr/bin/env python3
"""Text as scripts/qemu-shot.py's key steps (QEMU's sendkey names):

    keys.py "ls /sys" ret "echo hi" ret   ->  key:l key:s key:spc ... key:ret

An argument that is a known key name alone (ret, esc, tab, f1-f12, up,
down, left, right, home, end, pgup, pgdn, delete, backspace, insert, or
a combination like ctrl-s, shift-f6, alt-f1) is sent as it is; anything
else is typed character by character.
"""
import sys

NAMES = {' ': 'spc', '.': 'dot', '/': 'slash', '-': 'minus', '=': 'equal', ',': 'comma',
         ';': 'semicolon', "'": 'apostrophe', '\\': 'backslash', '[': 'bracket_left',
         ']': 'bracket_right', '`': 'grave_accent', '\t': 'tab', '\n': 'ret'}
SHIFTED = {'_': 'minus', '+': 'equal', ':': 'semicolon', '"': 'apostrophe', '?': 'slash',
           '<': 'comma', '>': 'dot', '|': 'backslash', '{': 'bracket_left', '}': 'bracket_right',
           '~': 'grave_accent', '!': '1', '@': '2', '#': '3', '$': '4', '%': '5', '^': '6',
           '&': '7', '*': '8', '(': '9', ')': '0'}
SPECIAL = {'ret', 'esc', 'tab', 'up', 'down', 'left', 'right', 'home', 'end', 'pgup', 'pgdn',
           'delete', 'backspace', 'insert', 'spc'} | {f'f{i}' for i in range(1, 13)}


def key(c):
    if c.isalpha() and c.isupper():
        return 'shift-' + c.lower()
    if c in SHIFTED:
        return 'shift-' + SHIFTED[c]
    return NAMES.get(c, c)


out = []
for a in sys.argv[1:]:
    parts = a.split('-')
    if a in SPECIAL or (len(parts) > 1 and parts[-1] and all(p in ('ctrl', 'shift', 'alt') for p in parts[:-1])):
        out.append('key:' + a)
    elif a.startswith(('wait:', 'shot:', 'mouse:', 'click', 'drag:', 'mem:')):
        out.append(a)
    else:
        out += ['key:' + key(c) for c in a]
print(' '.join(out))

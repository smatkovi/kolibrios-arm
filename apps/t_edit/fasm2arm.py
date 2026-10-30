#!/usr/bin/env python3
"""A FASM source of data made one for FASMARM (apps/t_edit/app.mk builds
t_edit's syntax files with it): FASMARM sizes the data directives as ARM
does (dw 32 bits, dd 64, dh 16), so x86's dw, dd, rw, rd become dh, dw,
rh, rw; and FASMARM knows f0-f7 as registers, so the label f1 (and its
local labels f1.N) becomes te_f1. Quoted strings and comments are left
as they are.

    fasm2arm.py IN OUT
"""
import re
import sys

DIRECTIVES = {'dw': 'dh', 'dd': 'dw', 'rw': 'rh', 'rd': 'rw', 'dq': 'dd', 'rq': 'rd'}
TOKEN = re.compile(r"[A-Za-z0-9_.@?$]+")


def convert_line(line):
    out, i, n = [], 0, len(line)
    while i < n:
        c = line[i]
        if c in '\'"':                          # a string: as it is
            j = line.find(c, i + 1)
            j = n if j < 0 else j + 1
            out.append(line[i:j])
            i = j
        elif c == ';':                          # a comment
            out.append(line[i:])
            break
        else:
            m = TOKEN.match(line, i)
            if m:
                t = m.group(0)
                low = t.lower()
                if low in DIRECTIVES:
                    t = DIRECTIVES[low]
                elif t == 'f1' or t.startswith('f1.'):
                    t = 'te_' + t
                out.append(t)
                i = m.end()
            else:
                out.append(c)
                i += 1
    return ''.join(out)


src, dst = sys.argv[1], sys.argv[2]
data = open(src, 'rb').read().decode('latin-1')
# lines split at LF only (str.splitlines would split at latin-1's 0x85 too)
open(dst, 'wb').write('\n'.join(convert_line(l) for l in data.split('\n')).encode('latin-1'))

#!/usr/bin/env python3
"""The profile of prof.so by function:

    report.py PROF.OUT KERNEL.ELF BASE [TOP] [--user]

PROF.OUT: prof.so's output (block address, instructions, runs); KERNEL.ELF:
the kernel that ran (linked at 0, position independent); BASE: where it
ran (the kernel's "boot arguments" line, the second value). Prints the
TOP functions by executed instructions, the kernel's and the programs'
totals (the programs run below 1 GiB, AArch32), and how often the
exception vectors ran (the system calls and interrupts from AArch32
programs, from the idle loop, from Linux programs). --user lists the
programs' hottest blocks too."""
import bisect
import subprocess
import sys

args = [a for a in sys.argv[1:] if not a.startswith('--')]
prof, elf, base = args[0], args[1], int(args[2], 16)
top = int(args[3]) if len(args) > 3 else 40
user = '--user' in sys.argv

syms = []
for line in subprocess.run(['llvm-nm', '-n', '--defined-only', elf], capture_output=True,
                           text=True, check=True).stdout.splitlines():
    p = line.split()
    if len(p) == 3 and p[1] in 'tTwW':
        syms.append((int(p[0], 16), p[2]))
syms.sort()
addrs = [a for a, _ in syms]
vectors = dict(syms)
vbase = next((a for a, n in syms if n == 'vectors'), None)
kinds = ['EL1t sync', 'EL1t irq', 'EL1t fiq', 'EL1t serror', 'EL1 sync', 'EL1 irq (idle)', 'EL1 fiq',
         'EL1 serror', 'A64 sync (Linux svc)', 'A64 irq', 'A64 fiq', 'A64 serror', 'A32 sync (svc #0x40)',
         'A32 irq', 'A32 fiq', 'A32 serror']

by_fn, ukern, uuser, vec = {}, 0, 0, {}
ublocks = []
for line in open(prof):
    va, n, runs = line.split()
    va, n, runs = int(va, 16), int(n), int(runs)
    insns = n * runs
    if va >= base and va < base + (64 << 20):
        off = va - base
        i = bisect.bisect_right(addrs, off) - 1
        name = syms[i][1] if i >= 0 else '?'
        by_fn[name] = by_fn.get(name, 0) + insns
        ukern += insns
        if vbase is not None and vbase <= off < vbase + 0x800 and (off - vbase) % 0x80 == 0:
            vec[(off - vbase) // 0x80] = vec.get((off - vbase) // 0x80, 0) + runs
    else:
        uuser += insns
        ublocks.append((insns, va, n, runs))

total = ukern + uuser
print(f'instructions: {total:,} (kernel {ukern:,}, {100 * ukern / max(total, 1):.1f} %; '
      f'programs {uuser:,})')
for k in sorted(vec):
    print(f'  vector {kinds[k]}: {vec[k]:,} entries')
print(f'{"function":32} {"instructions":>16} {"% kernel":>9}')
for name, c in sorted(by_fn.items(), key=lambda x: -x[1])[:top]:
    print(f'{name:32} {c:16,} {100 * c / max(ukern, 1):8.1f}')
if user:
    print('programs\' hottest blocks:')
    for insns, va, n, runs in sorted(ublocks, reverse=True)[:top]:
        print(f'  {va:#10x} {n:3} x {runs:12,} = {insns:14,}')

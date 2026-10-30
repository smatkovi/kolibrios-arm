#!/bin/sh
# The kernel's ways in and out counted in QEMU: apps/sysbench under the
# AArch64 kernel (KERNEL, build/arm64/kernel.bin) with tools/qemu-prof's
# marks: the kernel's instructions per system call, per yield alone and
# per yield with a peer (two system calls and two task switches).
set -e
K=${1:-build/arm64/kernel.bin}
make -s -C "$(dirname "$0")"
T=$(mktemp -d /tmp/kos-sysbench.XXXXXX)
QEMU_MACHINE=virt QEMU_APPEND="kolibri.run=/sys/sysbench" \
	QEMU_EXTRA="-plugin build/qemu-prof/prof.so,out=$T/prof.out,kbase=40200000" \
	python3 scripts/qemu-shot.py "$K" "$T" wait:30
grep '^sysbench:' "$T/serial.log"
python3 - "$T/prof.out.marks" <<'PY'
import sys
m = [tuple(int(x) for x in l.split()[3::2]) for l in open(sys.argv[1])]
for name, n, a, b in (('function 26.9', 100000, 0, 1), ('yield alone', 20000, 2, 3),
                      ('yield with a peer (round trip)', 20000, 4, 5)):
    print(f'sysbench: {name}: {(m[b][0] - m[a][0]) / n:.0f} kernel instructions each, '
          f'{(m[b][1] - m[a][1]) / n:.0f} in the programs')
PY

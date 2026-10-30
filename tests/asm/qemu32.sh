#!/bin/sh
# The ARMv7 kernel's side in QEMU's realview (make -C tests/asm qemu32):
# the programs' VFP/NEON registers kept across switches and the kernel's
# NEON (apps/fptest), apps/asmclock's NEON running, the screens of the C
# drawing (kolibri.simd=c) and of NEON the same; then the kernel built
# with SIMD=c (C only) and fptest again. The command line goes in through
# QEMU's -append (the raw kernel.bin: its ATAGs).
set -e
cd "$(dirname "$0")/../.."
T=$(mktemp -d /tmp/kos-asm32.XXXXXX)
K=build/realview/kernel.bin
fptest() {
	QEMU_APPEND="kolibri.run=/sys/fptest" python3 scripts/qemu-shot.py $K $T/$1 wait:15
	grep "^fptest" $T/$1/serial.log
	[ "$(grep -c 'fptest.*: ok' $T/$1/serial.log)" = 2 ]
}
make -s PLATFORM=realview
fptest fp
grep -q "drawing: NEON" $T/fp/serial.log
QEMU_APPEND="kolibri.run=/sys/asmclock kolibri.look=classic" python3 scripts/qemu-shot.py $K $T/clock wait:8 shot:clock
grep -q "started asmclock" $T/clock/serial.log
if grep "asmclock.*ended" $T/clock/serial.log; then
	echo "asmclock ended"
	exit 1
fi
echo "asmclock: runs (its NEON)"
for look in hildon classic; do
	QEMU_APPEND="kolibri.simd=c kolibri.look=$look" python3 scripts/qemu-shot.py $K $T/c-$look wait:8 shot:home
	QEMU_APPEND="kolibri.look=$look" python3 scripts/qemu-shot.py $K $T/neon-$look wait:8 shot:home
	grep -q "drawing: C" $T/c-$look/serial.log
	grep -q "drawing: NEON" $T/neon-$look/serial.log
	[ "$(magick compare -metric AE $T/c-$look/home.png $T/neon-$look/home.png /dev/null 2>&1)" = "0 (0)" ]
	echo "the screens of C and NEON, $look: the same"
done
make -s PLATFORM=realview SIMD=c
fptest fp-c
grep -q "drawing: C" $T/fp-c/serial.log
make -s PLATFORM=realview
echo "realview: all well ($T)"

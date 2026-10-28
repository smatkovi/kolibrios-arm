#!/bin/sh
# KolibriOS for ARM: start the AArch64 kernel on the F(x)tec Pro1X from a
# running Linux (Maemo Leste), with kexec, the way pro1x-maemo-leste's
# boot menu starts FreeBSD:
#
#   pro1x-kexec.sh [-n] [KERNEL] [COMMAND LINE]
#
# KERNEL is build/arm64/kernel.bin (default /host/kolibri/kernel.bin), the
# command line e.g. "kolibri.lifetime=60": KolibriOS stops feeding the
# watchdog after 60 s, which resets the phone about 32 s later, back into
# the boot menu (Linux's default system). -n only loads the kernel and
# unloads it again (a check that kexec takes it).
#
# Linux ends here: its filesystems are synced and remounted read-only, then
# the watchdog is armed and kexec jumps. After the next boot,
# pro1x-read.py shows what KolibriOS left in memory.
set -eu

check=
[ "${1:-}" = -n ] && { check=1; shift; }
kernel=${1:-/host/kolibri/kernel.bin}
args=${2:-}

[ "$(id -u)" = 0 ] || { echo "pro1x-kexec: as root" >&2; exit 1; }
command -v kexec > /dev/null || { echo "pro1x-kexec: no kexec (kexec-tools)" >&2; exit 1; }
[ -f "$kernel" ] || { echo "pro1x-kexec: no $kernel" >&2; exit 1; }

# kexec_file: the device tree this kernel booted with goes along
kexec -s -l "$kernel" --command-line="$args"
if [ -n "$check" ]; then
	echo "pro1x-kexec: $kernel loaded, unloaded again"
	kexec -u
	exit 0
fi

# The display must be on: KolibriOS takes it over as it runs, and an
# access to the display controller with its clocks off hangs the bus
disp=/sys/bus/platform/devices/5e00000.display-subsystem/power/runtime_status
if [ "$(cat $disp 2> /dev/null)" != active ]; then
	dbus-send --system --type=method_call --dest=com.nokia.mce /com/nokia/mce/request \
	    com.nokia.mce.request.req_display_state_on 2> /dev/null || true
	sleep 2
fi
if [ "$(cat $disp 2> /dev/null)" != active ]; then
	echo "pro1x-kexec: the display is off ($disp), not started" >&2
	kexec -u
	exit 1
fi

# KolibriOS drives the keyboard's I2C engine as Linux leaves it: its
# clocks must stay on (runtime PM off)
for d in 4a84000.i2c; do
	pm=/sys/bus/platform/devices/$d/power/control
	[ ! -e "$pm" ] || echo on > "$pm"
done

sync
# The APSS watchdog (0x0f017000, kpss layout, 32 kHz) armed for its
# longest time, about 32 s, as the boot menu does: KolibriOS feeds it, and
# a hang before or after that (here too) resets the phone.
busybox devmem 0x0f017008 32 0
busybox devmem 0x0f017004 32 1
busybox devmem 0x0f017010 32 0xfffff
busybox devmem 0x0f017014 32 0xfffff
busybox devmem 0x0f017008 32 1
# the filesystems: written out and read-only (sysrq-trigger works whatever
# kernel.sysrq says)
echo s > /proc/sysrq-trigger
echo u > /proc/sysrq-trigger
kexec -e

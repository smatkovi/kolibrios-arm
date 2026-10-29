#!/bin/sh
# KolibriOS for ARM: start the AArch64 kernel on the F(x)tec Pro1X from a
# running Linux (Maemo Leste), with kexec, the way pro1x-maemo-leste's
# boot menu starts FreeBSD:
#
#   pro1x-kexec.sh [-n] [-m] [KERNEL] [COMMAND LINE]
#
# KERNEL is build/arm64/kernel.bin (default /host/kolibri/kernel.bin), the
# command line e.g. "kolibri.lifetime=60": KolibriOS stops feeding the
# watchdog after 60 s, which resets the phone about 32 s later, back into
# the boot menu (Linux's default system). -n only loads the kernel and
# unloads it again (a check that kexec takes it).
#
# -m: the modem goes along (docs/pro1x-modem.md). It keeps running through
# the kexec; its EFS partitions go along as the initrd (tools/mkefs.py,
# for KolibriOS's rmtfs service), ofono is stopped and Linux's IPCRTR
# channel to the modem closed, so that KolibriOS can open it anew. rmtfs
# keeps running until the jump: stopped normally it would stop the modem.
#
# Linux ends here: its filesystems are synced and remounted read-only, then
# the watchdog is armed and kexec jumps. After the next boot,
# pro1x-read.py shows what KolibriOS left in memory.
set -eu

check= modem=
while [ $# -gt 0 ]; do
	case $1 in
	-n) check=1; shift ;;
	-m) modem=1; shift ;;
	*) break ;;
	esac
done
kernel=${1:-/host/kolibri/kernel.bin}
args=${2:-}

[ "$(id -u)" = 0 ] || { echo "pro1x-kexec: as root" >&2; exit 1; }
command -v kexec > /dev/null || { echo "pro1x-kexec: no kexec (kexec-tools)" >&2; exit 1; }
[ -f "$kernel" ] || { echo "pro1x-kexec: no $kernel" >&2; exit 1; }

busybox devmem 0x0f017008 32 > /dev/null ||
    { echo "pro1x-kexec: no busybox devmem (for the watchdog)" >&2; exit 1; }

# the modem's EFS images (read only), for the initrd
initrd=
if [ -n "$modem" ]; then
	here=$(cd "$(dirname "$0")" && pwd)
	efs=${EFS:-/host/kolibri/efs.img}
	set --
	for p in modemst1 modemst2 fsg fsc; do
		d=/dev/disk/by-partlabel/$p
		[ -e "$d" ] || { echo "pro1x-kexec: no $d" >&2; exit 1; }
		set -- "$@" "$p=$d"
	done
	python3 "$here/mkefs.py" "$efs" "$@" || exit 1
	initrd="--initrd=$efs"
fi

# kexec_file: the device tree this kernel booted with goes along; any
# failure from here on unloads it again
# shellcheck disable=SC2086
kexec -s -l "$kernel" $initrd --command-line="$args"
trap 'kexec -u' EXIT
if [ -n "$check" ]; then
	echo "pro1x-kexec: $kernel loaded, unloaded again"
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
	exit 1
fi

# KolibriOS drives the keyboard's I2C engine as Linux leaves it: its
# clocks must stay on (runtime PM off)
for d in 4a84000.i2c; do
	pm=/sys/bus/platform/devices/$d/power/control
	[ ! -e "$pm" ] || echo on > "$pm"
done

# The modem: its clients gone (ofono), Linux's IPCRTR channel closed
# (unbound from qrtr-smd; autoprobe off, so that Linux does not take it
# again when the modem reopens it). rmtfs stays: its SIGTERM would stop
# the modem.
ch=6080000.remoteproc:glink-edge.IPCRTR.-1.-1
# (an abort after this gives Linux its channel and ofono back)
restore_modem() {
	echo 1 > /sys/bus/rpmsg/drivers_autoprobe
	[ -e /sys/bus/rpmsg/devices/$ch/driver ] ||
	    echo -n "$ch" > /sys/bus/rpmsg/drivers/qcom_smd_qrtr/bind 2> /dev/null
	rc-service ofono start > /dev/null 2>&1 || true
}
if [ -n "$modem" ]; then
	trap 'kexec -u; restore_modem' EXIT
	rc-service ofono stop > /dev/null 2>&1 || true
	echo 0 > /sys/bus/rpmsg/drivers_autoprobe
	if [ -e /sys/bus/rpmsg/devices/$ch/driver ]; then
		echo -n "$ch" > /sys/bus/rpmsg/drivers/qcom_smd_qrtr/unbind
	fi
	sleep 1
fi

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
sleep 3                        # both are queued work: let them finish
trap - EXIT
kexec -e

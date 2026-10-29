#!/bin/sh
# KolibriOS for ARM: start the AArch64 kernel on the F(x)tec Pro1X from a
# running Linux (Maemo Leste), with kexec, the way pro1x-maemo-leste's
# boot menu starts FreeBSD:
#
#   pro1x-kexec.sh [-n] [-m [-a]] [KERNEL] [COMMAND LINE]
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
# -a (with -m): the sound of calls goes along (docs/pro1x-audio.md).
# PulseAudio's devices are suspended, the routes of UCM's Earpiece and Mic
# devices set, and two streams started that run through the jump, zeros
# played into MultiMedia1 and a recording from MultiMedia2: DAPM keeps the
# codec on and the ADSP its codec DMA ports. KolibriOS (kolibri.callaudio,
# added to the command line) takes over Linux's APR channel and starts the
# voice session for each call.
#
# Linux ends here: its filesystems are synced and remounted read-only, then
# the watchdog is armed and kexec jumps. After the next boot,
# pro1x-read.py shows what KolibriOS left in memory.
set -eu

check= modem= audio=
while [ $# -gt 0 ]; do
	case $1 in
	-n) check=1; shift ;;
	-m) modem=1; shift ;;
	-a) audio=1; shift ;;
	*) break ;;
	esac
done
kernel=${1:-/host/kolibri/kernel.bin}
args=${2:-}
[ -z "$audio" ] || [ -n "$modem" ] || { echo "pro1x-kexec: -a needs -m (the calls)" >&2; exit 1; }

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

# the sound card and PulseAudio's user (its devices are suspended for the
# streams below)
if [ -n "$audio" ]; then
	for t in amixer aplay arecord pactl su; do
		command -v $t > /dev/null || { echo "pro1x-kexec: no $t" >&2; exit 1; }
	done
	card=$(awk '/\[Pro1X/ { print $1; exit }' /proc/asound/cards)
	[ -n "$card" ] || { echo "pro1x-kexec: no Pro1X sound card" >&2; exit 1; }
	pa_user=$(ps -o user= -C pulseaudio | head -n 1)
	echo "pro1x-kexec: sound card $card, PulseAudio ${pa_user:-not running}"
	args="$args kolibri.callaudio"
fi
pa() {
	[ -n "$pa_user" ] || return 0
	su "$pa_user" -s /bin/sh -c "XDG_RUNTIME_DIR=/run/user/$(id -u "$pa_user") pactl $*"
}
pa_suspend() {
	for s in $(pa list short sinks | cut -f2); do pa suspend-sink "$s" "$1"; done
	for s in $(pa list short sources | cut -f2); do pa suspend-source "$s" "$1"; done
}

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

# The call's sound: UCM's Earpiece and Mic devices (Pro1X/HiFi.conf), then
# the two streams; their front ends' routes keep the codec DMA ports of
# the voice session (RX_CODEC_DMA_RX_0, VA_CODEC_DMA_TX_0) running.
keepers=
restore_audio() {
	[ -z "$keepers" ] || kill $keepers 2> /dev/null
	pa_suspend 0 || true
}
cs() { amixer -q -c "$card" cset "name=$1" "$2"; }
if [ -n "$audio" ]; then
	trap 'kexec -u; restore_audio; restore_modem' EXIT
	pa_suspend 1
	cs 'RX_CODEC_DMA_RX_0 Audio Mixer MultiMedia1' 1
	cs 'RX_MACRO RX0 MUX' AIF1_PB
	cs 'RX_MACRO RX1 MUX' AIF1_PB
	cs 'RX INT0_1 MIX1 INP0' RX0
	cs 'RX INT1_1 MIX1 INP0' RX1
	cs 'RX INT0_1 INTERP' 'RX INT0_1 MIX1'
	cs 'RX INT1_1 INTERP' 'RX INT1_1 MIX1'
	cs 'RX INT0 DEM MUX' CLSH_DSM_OUT
	cs 'RX INT1 DEM MUX' CLSH_DSM_OUT
	cs 'HPHL_RDAC Switch' 1
	cs 'HPHR_RDAC Switch' 0
	cs 'CLSH PA Switch' 1
	cs 'RX HPH Mode' CLS_AB
	cs 'HPHR Switch' 0
	cs 'HPHL Volume' 12
	cs 'RDAC3_MUX' RX1
	cs 'EAR_RDAC Switch' 1
	cs 'EAR_PA Volume' 8
	cs 'Spk0 OUT Switch' 0
	cs 'Spk1 OUT Switch' 0
	cs 'HPHL Switch' 1             # last (UCM: found off when set before EAR_RDAC)
	cs 'MultiMedia2 Mixer VA_CODEC_DMA_TX_0' 1
	cs 'VA DEC0 MUX' VA_DMIC
	cs 'VA DMIC MUX0' DMIC3
	cs 'VA_AIF1_CAP Mixer DEC0' 1
	cs 'VA_DEC0 Volume' 84
	aplay -q -D "hw:$card,0" -t raw -f S16_LE -r 48000 -c 2 /dev/zero &
	keepers=$!
	arecord -q -D "hw:$card,1" -t raw -f S16_LE -r 48000 -c 1 /dev/null &
	keepers="$keepers $!"
	sleep 1
	for p in $keepers; do
		kill -0 "$p" 2> /dev/null ||
		    { echo "pro1x-kexec: a stream of -a did not start (hw:$card,0 or ,1 busy?)" >&2; exit 1; }
	done
	echo "pro1x-kexec: the call's sound prepared (earpiece, microphone)"
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

# KolibriOS on the F(x)tec Pro1X

The AArch64 kernel (`make PLATFORM=arm64`, `build/arm64/kernel.bin`) on
the Pro1X (Qualcomm SM6115), with its platform in `kernel/platform/pro1x.c`.
It follows the FreeBSD port in
[pro1x-maemo-leste](https://github.com/smatkovi/pro1x-maemo-leste)
(`freebsd/`): Linux sets up the hardware, the kernel takes it over as it
is.

## Starting it

The kernel is an arm64 Image, started with `kexec -s` from Linux, with
Linux's device tree. From a running Maemo Leste (as root):

```sh
cp build/arm64/kernel.bin /host/kolibri/kernel.bin     # on the phone
tools/pro1x-kexec.sh -n                                  # check: kexec takes it, no start
tools/pro1x-kexec.sh /host/kolibri/kernel.bin "kolibri.lifetime=60"
```

`pro1x-kexec.sh` keeps the keyboard's I2C engine clocked (runtime PM
off), writes out and remounts the filesystems read-only, arms the SoC's
watchdog (about 32 s, as the boot menu does for FreeBSD) and jumps. Linux
ends there.

The command line:

| | |
| --- | --- |
| `kolibri.lifetime=N` | stop feeding the watchdog after N s, with a screenshot first: the phone resets about 32 s later, into the boot menu (an unattended test) |
| `kolibri.noinput` | leave the keyboard and the touch screen alone |
| `kolibri.look=classic` | KolibriOS's own panel and movable windows instead of the Hildon shell (`kolibri.look=hildon`, the default) |

Without a lifetime the kernel feeds the watchdog for as long as it runs;
a hang, or a panic, resets the phone about 32 s later.

First runs in steps, so that one failure does not hide another (an
access to a block whose clocks are off hangs the bus without a trace,
and the watchdog's reset ends the run):

1. `kolibri.noinput kolibri.lifetime=45`: the display, the RAM, the
   console, the watchdog. The console's `VIG0`/`DMA0` lines show what the
   display scanned out before; a black screen with a complete console
   is the display takeover, not the kernel.
2. `kolibri.lifetime=60`: the keyboard and the touch screen as well.

Linux's own shutdown on the way (kexec) already puts the SMMU in front of
the display into bypass, so the panel may show garbage for a moment
before the pipes are pointed at the framebuffer.

## What it left

The phone has no serial port KolibriOS can drive and no network there.
The kernel writes into the spare part of the bootloader's splash memory
(reserved, behind the framebuffer), which survives the reset:

| Address | |
| --- | --- |
| 0x5ca00000 | `PRO1X-KOS:ENTERED`, written first thing |
| 0x5ca01000 | the kernel's output (the RAM console, 1 MiB) |
| 0x5cc00000 | a screenshot: `PRO1X-SHOT`, the length at offset 12, a PPM (1080x540) from 16 |

Back in Linux:

```sh
tools/pro1x-read.py --shot /tmp/kolibri.ppm     # as root; --clear wipes it afterwards
```

## The hardware

| Part | How |
| --- | --- |
| RAM | `/memory` without `/reserved-memory` (and the device tree), cut at 2 MiB: about 7.8 GiB in some ten banks. Only these are mapped; the firmware's memory must not even be read speculatively |
| display | the pipes VIG0 and DMA0 pointed at `/chosen`'s simple-framebuffer, the display's SMMU context banks bypassed (`SCTLR = 0`), a flush of the control path. A blue stripe on the panel's top (the left edge in landscape) when this worked |
| screen | 1080x540 XRGB in RAM; the flush doubles each pixel and turns it clockwise, panel line by panel line, 64-bit stores into the framebuffer (Device-GRE) |
| interrupts, timer | GICv3 (what Linux left enabled turned off), the virtual timer at 100 Hz |
| watchdog | the APSS watchdog (`qcom,kpss-wdt`), fed every second from the timer tick |
| keyboard | an 8x8 matrix behind an AW9523B on the GENI I2C engine at 0x4a84000, polled through its FIFOs; scanned when the expander's INT (TLMM GPIO 107) is low and every 30 ms while keys are held, two scans in a row agreeing; the key map from the device tree; Shift, Ctrl, Alt, Fn and Home as TLMM GPIOs (gpio-keys). Fn with 1-0 gives F1-F10, with the arrows Page Up/Down, Home, End |
| touch screen | a Goodix GT9286 on bit-banged I2C (TLMM GPIOs 6 and 7), its interrupt pulses latched by the TLMM, as the mouse (a touch is the left button) |
| clock | not driven (the PMIC's RTC is behind SPMI): the time starts at 0 |

## In QEMU

The Pro1X's code paths without its devices: QEMU's virt with a device
tree that has the Pro1X's compatible string, its `/chosen` framebuffer and
its reserved memory. The kernel then draws into the framebuffer at
0x5c000000 (not shown by QEMU) and writes the RAM console there; both are
read with `scripts/qemu-shot.py`'s `mem:` steps:

```sh
qemu-system-aarch64 -M virt,gic-version=3,dumpdtb=virt.dtb -cpu cortex-a53 -m 8G
# virt.dts: compatible = "fxtec,pro1x", "qcom,sm6115"; /chosen with
# #address-cells, #size-cells, bootargs and framebuffer@5c000000
# (simple-framebuffer, 1080x2160, stride 4320); /reserved-memory as on the phone
QEMU_MACHINE=virt QEMU_MEM=8G QEMU_DTB=pro1x-qemu.dtb scripts/qemu-shot.py \
    build/arm64/kernel.bin out wait:4 mem:0x5ca01000,65536,console.txt \
    mem:0x5c000000,0x8e6200,fb.raw
magick -size 1080x2160 -depth 8 bgra:out/fb.raw -alpha off -rotate -90 fb.png
```

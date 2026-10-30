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

With `-m` the modem goes along (calls and SMS, `docs/pro1x-modem.md`):
the EFS partitions as the initrd, ofono stopped, Linux's IPCRTR channel
closed before the jump.

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
| `kolibri.nomodem` | leave the modem alone |
| `kolibri.fakemodem` | a modem in RAM instead (QEMU with the phone's device tree) |
| `kolibri.ipcrtr=adopt` | Linux's `IPCRTR` channel taken over as it is (`pro1x-kexec.sh -m` passes it; `docs/pro1x-modem.md`) |
| `kolibri.wifi` | the Wi-Fi (with `-m -w FILE`; `docs/pro1x-network.md`): the WLAN part's rails on through the RPM, the WLAN firmware's QMI service, then the driver: the station joins FILE's network, lwIP takes a lease |
| `kolibri.wifi=irq` | the same, the copy engines by their interrupts instead of polled from the tick |
| `kolibri.powertest=N` | N minutes of power measurement as a standby (after 10 s the display off, the SoC's sleep), a console line a minute with the battery's current and the RPM's sleep counts, then the reset (as a lifetime of N minutes and 30 s); `docs/power.md` |
| `kolibri.tick=periodic` | the 100 Hz tick also in idle (no tickless idle), the input polled as before |
| `kolibri.rpmsleep` | the RPM's sleep set at 0 and the MPM handed the next wake-up before the cluster's collapse: the SoC may sleep (vlow, vmin; `docs/power.md`) |
| `kolibri.displayon` | the power test without the standby: the display stays on, the sleep set as it is |
| `kolibri.battery` | the PMIC's battery readings also without a power test |
| `kolibri.standby` | the programs' timeouts on whole seconds, as while the display is off |
| `kolibri.psci=off` | the CPU idles with `wfi` only, no PSCI power-down states (`core`: no cluster state; `docs/power.md`) |
| `kolibri.bench` | the drawing loops and the string functions timed at the start, C against assembler (NEON), the flush to the panel too (`docs/asm.md`) |
| `kolibri.simd=c` | the drawing in C instead of the NEON assembler |
| `kolibri.rngtest` | the random numbers' sources and generator checked at the start (FIPS 140-2, the jitter's estimate; `docs/rng.md`) |
| `kolibri.storage=off` | the UFS left alone (`docs/storage.md`) |
| `kolibri.home=rw` | the container on userdata (`tools/kolibri-home`) written; without it nothing on the UFS is written |

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
| interrupts, timer | GICv3 (what Linux left enabled turned off), the virtual timer at the device tree's PPI (INTID 19 on the SM6115, `/timer`'s third interrupt), 100 Hz while tasks run, one interrupt at the next deadline when idle (`docs/power.md`) |
| watchdog | the APSS watchdog (`qcom,kpss-wdt`), fed every third of its bark time (10 s) from the timer tick |
| keyboard | an 8x8 matrix behind an AW9523B on the GENI I2C engine at 0x4a84000, polled through its FIFOs; scanned when the expander's INT (TLMM GPIO 107) is low and every 30 ms while keys are held, two scans in a row agreeing; INT, the modifier keys and the touch screen's INT as TLMM interrupts (the summary interrupt, SPI 227), so that an idle kernel sleeps until a key; the key map from the device tree; Shift, Ctrl, Alt, Fn and Home as TLMM GPIOs (gpio-keys). Fn with 1-0 gives F1-F10, with the arrows Page Up/Down, Home, End |
| touch screen | a Goodix GT9286 on bit-banged I2C (TLMM GPIOs 6 and 7), its interrupt pulses latched by the TLMM, as the mouse (a touch is the left button) |
| clock | not driven (the PMIC's RTC is behind SPMI): the time starts at 0 |
| UFS | the controller as Linux left it (kexec, its link up), its SMMU context's translation off; the LUNs as `/ufs0`...`/ufs5`, userdata as `/ufs0/13` (ext4, read only); the container on it as `/hd0/1` (`docs/storage.md`) |

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

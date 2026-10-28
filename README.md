# KolibriOS for ARM

A kernel for 32-bit ARM (ARMv7-A, Cortex-A8) that runs
[KolibriOS](https://kolibrios.org) programs: the same system functions,
the same windows and events, the same way of drawing. Programs are
recompiled for ARM. The target is the **Nokia N950** (TI OMAP3630);
development runs in QEMU's `realview-pb-a8`, a Cortex-A8 board with a
simple display, keyboard and mouse.

KolibriOS itself is x86 assembler (FASM), including its kernel, so this
is not a port of its code but a new kernel written in C. It keeps
KolibriOS's interface to programs, as documented in its `sysfuncs.txt`:

- `int 0x40` with `eax, ebx, ecx, edx, esi, edi` becomes `svc #0x40` with
  `r0-r5`; function numbers, arguments and return values are KolibriOS's.
- A program's header is KolibriOS's `MENUET01` header with ARM code, so
  its magic is `MENUETA1`. The program gets its memory from address 0, as
  on KolibriOS.
- The window manager is in the kernel. Programs draw directly to the
  screen, and a map of the screen says which window owns each pixel.
  When a window is uncovered, its program gets a redraw event and draws
  itself (functions 12.1, 0, the primitives, 12.2).
- Events (redraw, key, button, mouse), the masks of function 40, the key
  codes of function 2 (ASCII and set 1 scan code), buttons (function 8,
  17) and the skinned window's close and minimize buttons work as there.

## Status

Runs in QEMU (realview-pb-a8): the panel (`@taskbar`) with a window list
and the clock, the start menu (`menu`), the debug board, a process
manager, an example program and an analog clock, in windows that move
with the mouse; keyboard input.

| Part | State |
| --- | --- |
| boot, MMU (1 MiB sections, a page table per program), exceptions | runs (QEMU) |
| scheduler: preemptive, 100 Hz, the tasks' waits (functions 5, 10, 23) | runs (QEMU) |
| windows, skins, buttons, drawing, text (6x10 and 8x16 fonts), lines, images | runs (QEMU) |
| keyboard (PS/2 set 2 → set 1), mouse | runs (QEMU) |
| ramdisk `/sys` (= `/rd/1`): function 70 read, folder, information, start | runs (QEMU) |
| function 68 heap | allocates, does not free yet |
| threads (function 51), IPC (60), shared memory, drivers, files that write | not yet |
| N950: UART3, interrupts, timer, the display as the boot loader left it | written, **not tried on the device** |
| N950: touch screen (maXTouch), keyboard (TWL4030), clock | not yet |

The system functions done are in `kernel/syscall.c`; the others return
-1 and say so on the debug output.

## Programs

In C, against `lib/kos.h` (the system functions as inline functions)
and `lib/crt0.S` (the header and the start). KolibriOS's own programs
are mostly assembler or C--, which only produce x86 code; programs in C
can be recompiled.

| Program | What it is |
| --- | --- |
| `@taskbar` | the panel: menu button, a button per window, the clock |
| `menu` | the start menu: the programs on the ramdisk |
| `example` | KolibriOS's example program: text, buttons, keys |
| `clock` | an analog clock, drawn with lines |
| `board` | the debug board: what the kernel and programs write (function 63) |
| `cpu` | the process manager: the threads, their memory and state; ends one |

`apps/autorun` lists what the kernel starts.

## Building and running

Needs the Arm GNU toolchain (`arm-none-eabi-gcc`), python3, and QEMU
(`qemu-system-arm`) to run it.

```sh
make            # build/realview/kernel.elf, the programs and the ramdisk
make run        # in QEMU
make PLATFORM=n950   # build/n950/kernel.bin, a raw image for 0x80008000
```

`scripts/qemu-shot.py` runs it without a window and takes screenshots
(input with QEMU's `sendkey` and mouse commands), for tests.

## The N950

`kernel/platform/n950.c` takes over the display from the boot loader:
NOLO (or Linux, when started with kexec from ubiboot) has set up the
DSI panel (Himalaya, command mode, 480x864, used in landscape), and the
kernel copies its own landscape buffer, turned, into the graphics
layer's buffer and has the display controller send a frame. The image
is meant to be started without writing the flash, from the PC with
NOLO's `flasher -k build/n950/kernel.bin -l -b`, or from ubiboot with
kexec as a third entry beside MeeGo and Leste. Still to do there: the
touch screen and the keyboard (I2C), the real-time clock, and whatever
the display needs beyond what is written.

## Licence

GPL 2 (`LICENSE`), as KolibriOS. The fonts are the Linux kernel's 8x16
and 6x10 console fonts (GPL 2).

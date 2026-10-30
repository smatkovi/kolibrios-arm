# KolibriOS's own programs: shell, terminal, kfar, t_edit

Four programs of KolibriOS itself (its repository's `programs/`, GPL-2.0)
run on KolibriOS for ARM:

| Program | Original | Language there | Here |
| --- | --- | --- | --- |
| `shell` | `programs/system/shell` (Pavel "Albom" Mamontov and the KolibriOS team: Siemargl, Coldy, Leency, rgimad, ...) | C | built from its own sources, a small patch (`apps/shell`) |
| `terminal` | `programs/system/terminal` (hidnplayr) | FASM | translated to C (`apps/terminal`) |
| `kfar` | `programs/fs/kfar` (diamond, Evgeny Grechnikov, and the KolibriOS team) | FASM | translated to C (`apps/kfar`) |
| `t_edit` | `programs/other/t_edit`, `t_edit_synt` (IgorA, Igor Afanasiev, and the KolibriOS team) | FASM | translated to C (`apps/t_edit`) |

and the libraries they use: KolibriOS's console library (`console.obj`),
box_lib's widgets, libini, and in t_edit's case more (below).

## The sources

`tools/fetch-kolibrios.sh` fetches them, file by file, from KolibriOS's
own host at commit `855f02d2` (main, 2026-10-28), each checked by its
SHA-256 (`tools/kolibrios-files.txt`, 203 files, 2.3 MB), into
`third_party/kolibrios` (not kept in the repository, as lwIP isn't); the
Makefile runs it when they are missing. Gitea's API serves the files of a
commit (`/api/v1/repos/KolibriOS/kolibrios/raw/PATH?ref=COMMIT`). The
files come as they are there: CR LF, comments in CP866. Fetched besides
the four programs: `console_coff`, `box_lib`, `libini`, `msgbox`,
`proc_lib`, `opendial` (the libraries and what the translations follow),
`KOSfuncs.inc`, `macros.inc`, `proc32.inc` and the like, ktcc's
`conio.h`, `shell_api.h`, `sys/ksys.h`, `icons32.png`, the distributed
`kfar.ini`, `sysfuncs.txt`, and `COPYING.TXT` (the GPL).

## C or assembler

KolibriOS's x86 assembler cannot run on ARM, so the FASM programs are
translated. All three are translated to C, not to FASMARM:

- terminal is calls into box_lib and the console on their structures;
  kfar and t_edit are large (kfar ~19 000 lines, t_edit with its box_lib
  parts ~10 000) and use the same libraries. In C the structures are
  shared through headers with the libraries, which are C too, and the
  programs build for both kernels without an assembler.
- The translations keep the originals' structure and names (labels as
  functions, the same globals and structures), with the original file
  and label at each function, so they can be followed side by side.
- FASMARM stays for programs written in ARM assembler from the start
  (`docs/asm.md`).

The shell is C already: it is compiled from its own sources. Its
`system/kolibri.c` does every system call with x86's `int 0x40`;
`apps/shell/kolibri.c` replaces it with the same functions through
`svc #0x40`. `apps/shell/arm.patch` changes three places: `ver cpu` and
`kfetch` used `cpuid` (now "ARM"), `lsmod` x86 inline assembler for
68.31 (now says there are no loadable drivers), and starting a program
waited 0.6 s for a console program's named shared memory, which our
kernel lacks. The parts of ktcc's `sys/ksys.h` and `shell_api.h` it
uses are in `apps/shell/include`. It is built with `-fpermissive`
(GCC 14 makes the original's pointer mismatches errors) and
`-fsigned-char` (x86's `char`).

## The libraries: linked in

KolibriOS loads `console.obj`, `box_lib.obj`, `libini.obj` and the
others as shared libraries (function 68.19, COFF objects with an export
table). Here they are C libraries linked into each program
(`lib/NAME/*.c` become `build/apps/lib/NAME.a`, a program names them in
`LIBS_name`): no loader in the kernel, no format to define, and each
KolibriOS process had its own copy of a library anyway. The functions
keep the originals' names and arguments (`con_init`, `con_printf`,
`edit_box_draw(edit_box *)`, `ini_get_str`, ...), so a loader for an
ARM library format could export the same symbols later.

| Library | From | What |
| --- | --- | --- |
| `lib/libc` | (new) | the C library of the ported programs: string, ctype, stdlib (a malloc that frees, over 68.12), stdio's formatted output (printf to the debug board), `getcwd`/`chdir` (function 30), `main(argc, argv)` |
| `lib/console` | `console_coff/console.asm` (diamond) | the console window, all of `console.obj`'s functions |
| `lib/boxlib` | `box_lib` (Lrz, Mario79, Maxxxx32, diamond, hidnplayr, IgorA) | edit_box, check_box2, option_box, scrollbar, tree_list, the TED editor component (`box_lib.h`: box_lib.mac's structures, field for field) |
| `lib/libini` | `libs-dev/libini` (mike.dld) | settings files: `ini_get_str`, `ini_set_str`, `ini_get_int`, `ini_get_color`, `ini_get_shortcut`, `ini_enum_sections`, ... |

The console differs in one way: `console.obj` runs its window in a
thread of its own, which the program's calls wake by IPC. Our kernel has
neither threads (function 51) nor IPC for KolibriOS programs, so the
library runs the window in the program's thread whenever the program
calls it: while it writes (drawn at most every 50 ms), waits in
`con_getch`, `con_gets`, `con_kbhit`, or after `con_exit(0)` until the
window is closed. Its look is console.obj's: 80 x 25 characters of its
8x16 font (`font8x16.bmp`), 300 lines kept, the scroll bar from
`conscrlv.bmp`, its 16 colours, its Esc sequences and key codes. In the
Hildon look the window is maximised and shows as many lines as fit.

## The files

KolibriOS's file systems, as function 70 knows them (`kernel/kfs.c`):

| Path | What | Writable | Kept |
| --- | --- | --- | --- |
| `/` | the devices: `rd`, `tmp0` (and `linux` on AArch64) | no | |
| `/rd/1` = `/sys` | the ramdisk: the files built into the kernel image (programs, settings, icons, syntax files), as folders and files | yes: copy on write over the kernel's bytes | until the next reset |
| `/tmp0/1` | an empty RAM disk (KolibriOS's tmpdisk) | yes | until the next reset |
| `/linux/...`, `/home/user/...`, `/tmp/...` (AArch64) | the Linux personality's tree (Yappari's and the messengers' data) | yes, in RAM; its ramdisk files read-only | until the next reset |

All of function 70's subfunctions 0-10 work on them (read, folder,
create/rewrite, write, set the size, information, attributes, start,
delete, make a folder, rename/move) with KolibriOS's error codes, and
function 30 gives every program a current folder (`/sys` at first,
inherited by the programs it starts). The RAM disks take their memory
in 4 KiB pages from blocks of 1 MiB and leave 16 MiB free for programs.

On the phones nothing is kept: the kernel has no driver for the Pro1X's
UFS or the N950's eMMC (nor their SD cards), so Leste's or Android's
`userdata`, the N950's MyDocs and any FAT partition are out of reach.
Everything written lives in RAM until the next reset; a file system on
storage (FAT on an SD card's partition, say) would be the next step for
keeping files. The same holds in QEMU.

## Kernel functions added for them

| Function | For |
| --- | --- |
| 70.0-70.10, 30 | all four (`kernel/kfs.c`) |
| 65 (images with a palette, 1-32 bits a pixel) | the console, kfar's screen |
| 66.1-66.3 (scan codes, every press and release; the control keys), 26.2 (KolibriOS's US layouts, from its kernel's `data32.inc`) | the console, kfar, t_edit |
| 37.1 relative to the client area, 37.3's events kept until the mouse changes again, its double click bit, 37.7 the wheel | box_lib, t_edit, the console's scroll bar |
| 54 (the clipboard) | t_edit, the shell |
| 48.3 (the system colours: KolibriOS's default skin's in the classic look, a dark set in Hildon's) | terminal, box_lib, t_edit |
| 68.16 "SERIAL", 68.17 (`kernel/serial.c`) | terminal |

The loader now starts the heap behind a program's `.bss` (the header's
end of the image is `__bss_end`), and reads programs through kfs, so a
program written to `/tmp0/1` or over one in `/sys` starts.

## shell

All of the original's commands: `about`, `alias`, `cd`, `clear`, `cp`,
`mv`, `ren`, `date`, `echo` (with `>` and `>>` into files), `exit`,
`free`, `help`, `history`, `kfetch` (its coloured logo), `kill`, `pkill`,
`ls` (folders in colour, `ls -1`), `lsmod`, `mkdir` (with the folders
above), `more`, `ps`, `pwd`, `reboot`, `rm`, `rmdir`, `shutdown`,
`sleep`, `touch`, `uptime`, `ver` (`ver kernel`, `ver cpu`), `waitfor`;
running programs (from the current folder or `/sys`) and `#SHS`
scripts; `/sys/settings/.shell` (the original's) runs at the start. The
original has no `cat`: `more` shows files. `lsmod`, `reboot` and
`shutdown` say what KolibriOS for ARM lacks; programs started from it
run in their own windows (no shell API without shared memory).

![shell](img/shell-files.png)
![kfetch](img/shell-kfetch.png)

## terminal

What it is: a terminal on a serial port (hidnplayr's, 2014). Its window
asks for the port, speed, data bits, stop bits, parity and flow control
and local echo; Open (or Enter) opens the port through KolibriOS's
serial driver service and gives the window to the console, which sends
every key to the port and shows what comes from it.

Here the kernel offers that service itself (`kernel/serial.c`: function
68.16 "SERIAL", then 68.17's requests, as `drivers/serial/common.inc`
makes them): port 0 is the kernel's console UART (realview's and virt's
PL011, receiving polled; on the N950 and the Pro1X only sending, the
Pro1X having no UART at all), ports 1-3 are realview's other PL011s. The
settings are checked as the original checks them, not applied (QEMU's
UARTs don't need them, the console UART keeps the boot loader's).

In QEMU, realview's UART1 on a TCP socket: what is typed in the
terminal arrives there, what is sent from there shows in the console:

```sh
QEMU_EXTRA="-serial tcp:127.0.0.1:18799,server=on,wait=off" \
    python3 scripts/qemu-shot.py build/realview/kernel.elf /tmp/t wait:4 ...
```

![terminal's settings](img/terminal-settings.png)
![a line from the other end](img/terminal-session.png)

## kfar

Kolibri Far 0.65, all of it: about 19 300 lines of assembler became
about 10 300 of C (`apps/kfar`: `kfar.c` the start, events, keys, mouse,
screens and drawing, `data.c` its tables and dialog templates,
`panels.c` and `paneldraw.c` the panels, `dialogs.c`, `viewer.c` with
`search.c`, `editor.c`, `filetool.c` and `filesearch.c`, `sort.c`,
`tools.c`; `kfar.h` the shared structures; the labels, names and
structures the original's, each function naming its label). Its screen
is a grid of character cells in its own bitmap fonts, made from its
`font*.bmp` at build time (`apps/kfar/mkfont.py`, `make KFAR_FONT=` one
of font6x9, font7x12, font8x9, font8x16, font10x20), drawn with function
65; its colours and keys from `/sys/kfar.ini` (KolibriOS's distributed
one). The left panel starts in `/sys`, the right one in `/tmp0/1` (the
original's `/hd0/1` does not exist here); Alt+F1/F2 list the volumes.

Tried in QEMU (classic and Hildon looks, both kernels): moving, Tab,
folders in and out, starting a program with Enter, F3 (the viewer: hex,
wrapping, the encodings, searching), F4 (the editor: typing, joining and
splitting lines, F2 saving, searching), F5 and Shift+F5 (copying, into a
new folder too), F6 and Shift+F6 (moving and renaming: 70.10 on one
volume, copy and delete between volumes), F7, F8 with its question, the
sort modes (Ctrl+F3 ... Ctrl+F6), the column modes, Alt+letter quick
search, grey + and * selection, Alt+F7 (searching files and their
contents), F12 (the screens), F10. In the Hildon look kfar fills the
window under the panel; mouse clicks, double clicks and the key bar
work.

What differs: no plugins (kfar_arc and the plugin interface of
`api.txt` need 68.19); memory from the C library (68.13 does not give
back, 68.20 is missing); the file search, a thread in the original, runs
in steps from the screen's idle handler; kfar 0.65 has F6 and Shift+F6
only on its key bar, here they move and rename. Fixed on the way, as
found: PgUp to the first file, the copy's destination under `/sys`, the
viewer with short paths, a few of the editor's block operations, the
file search's reads, the search automaton's fallback. Enter on a text
file starts the program `kfar.ini` associates with it, which is not on
this ramdisk (kfar says so).

![kfar](img/kfar-classic.png)
![F8 in kfar](img/kfar-delete.png)
![the viewer](img/kfar-viewer.png)
![kfar in the Hildon look](img/kfar-hildon-copy.png)
![the volumes on the AArch64 kernel](img/kfar-arm64-drives.png)
![Alt+F7](img/kfar-filesearch.png)

## Testing

`apps/fstest` checks the file systems and the other new functions from
a program (`fstest: done 0 failed` on the debug board); `tests/apps`
runs it on both kernels and drives the programs in QEMU through
`scripts/qemu-shot.py` (`tests/apps/keys.py` turns text into its key
steps): the shell's commands, the terminal against a socket, kfar's
and t_edit's operations, with screenshots.

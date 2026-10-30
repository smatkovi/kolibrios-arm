# Threads, IPC, shared memory and libraries

KolibriOS programs use four things of their kernel that KolibriOS for
ARM now has as well, with the original's numbers, arguments and answers
(KolibriOS's `kernel/trunk/docs/sysfuncs.txt`; the kernel sources
followed: `core/taskman.inc`, `core/sys32.inc`, `core/memory.inc`,
`core/heap.inc`, `core/dll.inc`, `programs/dll.inc`):

| Function | What | Here |
| --- | --- | --- |
| 51.1 | create a thread (entry, stack); its TID or -1 | `kernel/proc.c` |
| 51.2, 51.3, 51.4 | the current slot; a thread's priority (kept, not used by the scheduler) | `kernel/proc.c` |
| 60.1, 60.2 | IPC: the receiving buffer; send a message (event 7) | `kernel/proc.c` |
| 68.22, 68.23 | named shared memory: open/create, close | `kernel/proc.c` |
| 68.19, 68.18 | load a library: its export table | `kernel/dll.c` |
| 9, 18.2, 18.18, 18.21 | a thread's information, ending a thread by slot or TID, the slot of a TID | as before, now per thread |

## Processes and threads

A process is an address space (`struct proc`): the program's image from
VA 0 (one piece of physical memory, `mem_size`), its heap (68.11,
68.12: a bump allocator behind the image, now shared by its threads),
and what is mapped beyond the image (shared memory, libraries). A
thread is a task as before: its own slot, TID (function 9's PID), window,
events, keys and buttons, IPC buffer, current folder (copied from its
creator) and FP/SIMD registers; the same page tables as the other
threads of its process. A new thread starts at `entry` with its stack
pointer at `stack` (memory of the program's own, as in KolibriOS), all
other registers 0, the default event mask (7) and a redraw event first,
the process's name in function 9.

As in KolibriOS, a process ends with its last thread. Ending one thread
(function -1, 18.2 by its slot, 18.18 by its TID, a fault) leaves the
others running: console.obj's thread, for one, ends its program's main
thread by 18.18 when its window is closed and then itself. A thread
whose function returns runs into address 0 and ends with a fault: end
threads with -1.

Switching between threads of one process keeps the tables (no TLB
flush on ARMv7, the same ASID on AArch64). The FP/SIMD registers stay
lazy per thread, on both kernels: their save areas are kept by slot and
TID (`arch/*/fpu.c`), so a thread's first VFP or NEON instruction finds
zeros and its own values afterwards; on the AArch64 kernel the programs
and their threads run in AArch32 as before.

`MAX_TASKS` is 128 now (slots 1-127; the screen's owner map is a byte a
pixel, so at most 255). The C library's `malloc`, `free` and `realloc`
take a lock (ldrex/strex, 68.1 while another thread holds it).

## IPC

As `core/memory.inc`'s `sys_IPC`: 60.1 sets the thread's buffer (its
address and size); 60.2 sends a message to a thread by its TID. The
buffer: `+0` locked when not 0, `+4` the bytes used (the receiver starts
it at 8), then the messages, each the sender's TID, its length and the
data; 60.2 appends one and sends event 7. It answers 0 (sent), 1 (no
buffer), 2 (locked), 3 (no room), 4 (no such TID), and works between
threads of one program and between programs.

## Named shared memory

68.22 opens or creates an area by name (31 characters), as
`core/heap.inc`'s `shmem_open`: `SHM_OPEN`, `SHM_OPEN_ALWAYS`,
`SHM_CREATE`, with `SHM_READ` or `SHM_WRITE`; it returns the address
and in edx 0 (created) or the area's size (opened), or 0 and the error
(5 not found, 10 access, 30 no memory, 33 parameters). An area opened
read-only is mapped read-only. Every open maps the area again (another
address); 68.23 closes the thread's latest mapping of that name. The
memory goes when its last mapping is closed.

What differs: KolibriOS maps 4 KiB pages anywhere in the process's heap.
Here an area takes whole blocks of the kernel's programs' memory
(1 MiB sections on ARMv7, 2 MiB blocks on AArch64) and is mapped from
256 MiB on (`MAP_START`, up to the first GiB's end); its size (edx)
stays the pages asked for. A thread's mappings stay when it ends (the
process's then, until it ends or 68.23 closes them): KolibriOS drops
the thread's reference then and leaves the pages mapped.

## Libraries

KolibriOS's libraries are COFF objects for x86 with an export table;
`core/dll.inc` loads one into the process, relocates it and returns the
address of its symbol `EXPORTS`. KolibriOS for ARM's libraries are ARM
code, so they have a format of their own, one the GNU toolchain makes as
it is:

- an ELF32 shared object for ARM (`ET_DYN`, `EM_ARM`, little endian),
  linked with `-shared -Bsymbolic --hash-style=sysv -z max-page-size=4096
  --no-undefined` from code compiled `-fPIC -fvisibility=hidden`, the C
  library and libgcc inside;
- its `PT_LOAD` segments from VA 0, no `DT_NEEDED`, only REL
  relocations: `R_ARM_RELATIVE` (and `R_ARM_ABS32`, `GLOB_DAT`,
  `JUMP_SLOT` against its own symbols), which `tools/mkdll.py check`
  makes sure of when it is built;
- the export table KolibriOS's: the symbol `EXPORTS` (in the dynamic
  symbol table), `{ name, value }` pairs of words ended by `0, 0`; its
  first entry `lib_init`, then `version` and the functions under the
  original's names.

68.19 (and 68.18, the path's encoding aside: paths are ASCII here)
loads it into the process: a copy of its own (KolibriOS shares the pages
between processes when it can), packed into blocks of the map area,
relocated, the caches made coherent; the address of `EXPORTS`, or 0. A
library already in the process (the same path) gives the same table
again, to any of its threads. A path without `/` at its start is
relative to the current folder, as in function 70.

The program's side is `programs/dll.inc`'s, in the C library
(`lib/libc/dll.c`, `lib/kos_dll.h`): `dll_load` (dll.Load) takes an
import list of `{ table, file name }` pairs (ended by `0, 0`), loads
each file (from `/sys/lib` when the name has no `/`), calls `lib_init`
when the first export's name starts with `lib_` (with the program's
`malloc`, `free`, `realloc` and `dll_load`, as dll.Init passes
mem.Alloc, mem.Free, mem.ReAlloc and dll.Load) and fills each table: a
word per function, its name until `dll_link` (dll.Link) puts the
address there. The C library's start runs it on the program's own list
(the `.kos_import` section, `lib/app.ld`) before `main`.

### Building them

`lib/NAME/NAME.exp` lists a library's exports in the original's order
(`tools/mkdll.py`'s comment has the format). The Makefile's `DLLS`
(`console:console.obj boxlib:box_lib.obj libini:libini.obj`) are built
twice: as before into `build/apps/lib/NAME.a`, and position independent
into `build/apps/lib/FILE`, which goes onto the ramdisk as
`/sys/lib/FILE`. A library's `malloc` and its kin are `lib/dllrt.c`'s:
the program's, from `lib_init`, so memory may change hands between a
program and its libraries.

A program naming NAME in its `LIBS_name` links `build/apps/lib/NAME-imp.a`
then: its import list entry and table and a stub per function (`ldr ip,
=slot; ldr pc, [ip]`, made by `tools/mkdll.py imports`), and what
KolibriOS's programs have themselves (`lib/NAME/import/*.c`: ktcc's
`con_init()` without arguments). `make DLL=0` links the libraries into
the programs as before.

| Library | File | Exports |
| --- | --- | --- |
| `lib/console` | `/sys/lib/console.obj` | console.obj 2.9's: `START`, `version`, `con_init`, ... `con_get_input` |
| `lib/boxlib` | `/sys/lib/box_lib.obj` | box_lib's edit box, check box, option box, scroll bars, tree list and text editor, with their versions |
| `lib/libini` | `/sys/lib/libini.obj` | libini's `ini_*` functions |

## The programs that use them

- The console library (`lib/console`, `/sys/lib/console.obj`) runs its
  window in a thread of its own, woken by IPC, as console.obj does
  (`docs/apps.md`): the shell and the terminal.
- The shell serves a program's console through its named shared memory
  `PID-SHELL`, as in KolibriOS (`docs/apps.md`).
- The shell, the terminal, kfar and t_edit load console.obj, box_lib.obj
  and libini.obj with 68.19 when they start.

## Testing

`apps/threadtest` checks all of it from a program (`threadtest: done N
failed` on the debug board): threads created, ended by -1, 18.18, 18.2
and a fault, 44 of them at once (twice, the slots used again), the
VFP/NEON registers kept per thread, a window per thread; IPC ping-pong
between threads (200 rounds), the error codes, event 7; between two
processes (it starts itself again) IPC and shared memory, the access
rules, a process whose main thread ended while another runs on; 68.19
of libini.obj, the same table again, a function called through it.
`tests/threads` runs it on both kernels:

```sh
make -C tests/threads
```

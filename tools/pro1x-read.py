#!/usr/bin/env python3
"""KolibriOS for ARM on the F(x)tec Pro1X: what the kernel left in the
bootloader's splash memory, read from Linux after the next boot (as root):

    pro1x-read.py [--shot FILE] [--clear]

Prints whether the kernel was entered (PRO1X-KOS:ENTERED at 0x5ca00000)
and its output (the RAM console from 0x5ca01000); --shot writes the
screenshot (0x5cc00000, a PPM) to FILE; --clear wipes the marker, the
console's start and the screenshot's header afterwards, so that the next
read does not show this run again.

The region is reserved memory outside Linux's RAM: /dev/mem maps it, and
it is read in aligned 8-byte words (read() on /dev/mem fails there).
"""
import mmap
import os
import sys

BASE = 0x5ca00000
SIZE = 0x500000                  # to 0x5cf00000: marker, console, screenshot
MARKER, CONSOLE, SHOT = 0x0, 0x1000, 0x200000
CONSOLE_SIZE, SHOT_SIZE = 1 << 20, 3 << 20


def words(view, off, n):
    """n bytes from off (both 8-byte aligned), one 64-bit load at a time"""
    out = bytearray()
    for i in range(off // 8, (off + n + 7) // 8):
        out += view[i].to_bytes(8, 'little')
    return bytes(out[:n])


def main():
    args = sys.argv[1:]
    shot_file = None
    clear = '--clear' in args
    if '--shot' in args:
        shot_file = args[args.index('--shot') + 1]
    fd = os.open('/dev/mem', os.O_RDWR | os.O_SYNC)
    mm = mmap.mmap(fd, SIZE, mmap.MAP_SHARED, mmap.PROT_READ | mmap.PROT_WRITE, offset=BASE)
    view = memoryview(mm).cast('Q')

    marker = words(view, MARKER, 24).split(b'\n')[0]
    print('marker:', marker.decode(errors='replace') if marker.startswith(b'PRO1X-') else 'none')
    con = bytearray()
    for off in range(CONSOLE, CONSOLE + CONSOLE_SIZE, 4096):
        block = words(view, off, 4096)
        end = block.find(b'\0')
        con += block if end < 0 else block[:end]
        if end >= 0:
            break
    if con.startswith(b'PRO1X-'):
        sys.stdout.write(con.decode(errors='replace'))
    else:
        print('no console')

    head = words(view, SHOT, 16)
    if head.startswith(b'PRO1X-SHOT\n'):
        n = int.from_bytes(head[12:16], 'little')
        print(f'screenshot: {n} bytes')
        if shot_file and 0 < n <= SHOT_SIZE - 16:
            with open(shot_file, 'wb') as f:
                f.write(words(view, SHOT + 16, n))
            print('written to', shot_file)
    else:
        print('no screenshot')

    if clear:
        for off in (MARKER, CONSOLE, SHOT):
            for i in range(4):
                view[off // 8 + i] = 0
    view.release()
    mm.close()
    os.close(fd)


if __name__ == '__main__':
    main()

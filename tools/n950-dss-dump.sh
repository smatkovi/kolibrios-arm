#!/bin/sh
# On the N950 under Linux (Harmattan, Leste): the display subsystem's
# registers as the running system left them, for the kernel's display
# take-over (kernel/platform/n950.c). Read only; needs root and devmem
# (busybox devmem, devmem2) or python with /dev/mem.
#
#   sh n950-dss-dump.sh > dss.txt
#
# DSS 0x48050000, DISPC 0x48050400, DSI (protocol 0x4804fc00, PHY
# 0x4804fe00, PLL 0x4804ff00); TRM: OMAP36xx, chapter "Display Subsystem".

rd() {
	if command -v devmem > /dev/null 2>&1; then devmem "$1" 32
	elif command -v devmem2 > /dev/null 2>&1; then devmem2 "$1" w | sed -n 's/.*: //p'
	else python3 -c "
import mmap, os, struct, sys
a = int(sys.argv[1], 16); f = os.open('/dev/mem', os.O_RDONLY | os.O_SYNC)
m = mmap.mmap(f, 4096, mmap.MAP_SHARED, mmap.PROT_READ, offset=a & ~4095)
print(hex(struct.unpack('<I', m[a & 4095:(a & 4095) + 4])[0]))" "$1"
	fi
}

dump() { # NAME BASE COUNT (words)
	echo "== $1"
	i=0
	while [ $i -lt $3 ]; do
		a=$(printf '0x%08x' $(( $2 + i * 4 )))
		echo "$a $(rd $a)"
		i=$((i + 1))
	done
}

dump DSS 0x48050000 32
dump DISPC 0x48050400 120
dump DSI 0x4804fc00 64
dump DSI_PHY 0x4804fe00 8
dump DSI_PLL 0x4804ff00 8

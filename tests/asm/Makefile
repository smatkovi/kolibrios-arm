# The kernel's assembler against its C, bit for bit: make -C tests/asm
#   test    the comparisons (test.c): AArch64 (test64, qemu-aarch64) and
#           ARMv7 (test32, qemu-arm -cpu cortex-a8)
#   bench   the routines on the screen's sizes with tools/qemu-prof's
#           plugin: the instructions of each, C and assembler (bench64:
#           the Pro1X's 1080 x 540, bench32: the N950's 854 x 480; the
#           device's own times: kolibri.bench, docs/asm.md)
# Needs aarch64-linux-gnu-gcc (static glibc) and qemu-aarch64; for ARMv7
# zig (cc for arm-linux-musleabi, static), arm-none-eabi-gcc and qemu-arm.
TOP := ../..
B := $(TOP)/build/asm-test
CC64 ?= aarch64-linux-gnu-gcc
CFLAGS := -O2 -g -Wall -Wextra -Wno-unused-parameter -Wno-sign-compare -DBLIT_HOST -I$(TOP)/kernel/include
SRC := test.c $(TOP)/kernel/fonts/font8x16.c $(TOP)/kernel/fonts/font6x10.c
# blit.c compiled as the kernel compiles it (clang, no FP/SIMD registers,
# aligned accesses): its instruction counts are the kernel's
KCC := clang --target=aarch64-linux-gnu -mcpu=cortex-a53 -O2 -std=gnu11 -ffreestanding -fno-builtin \
	-fno-common -mgeneral-regs-only -mstrict-align -DBLIT_HOST -I$(TOP)/kernel/include
ASM := $(TOP)/kernel/arch/arm64/blit.S $(TOP)/kernel/arch/arm64/string.S
# string.S's names moved aside: the C library has its own
RENAME := -Dmemcpy=k_memcpy -Dmemmove=k_memmove -Dmemset=k_memset

test: test64 test32

test64: $(B)/test
	qemu-aarch64 $(B)/test

$(B)/test: $(SRC) $(ASM) $(TOP)/kernel/blit.c $(TOP)/kernel/include/blit.h cstring.c Makefile
	@mkdir -p $(B)
	$(KCC) -c $(TOP)/kernel/blit.c -o $(B)/blit_c.o
	$(KCC) -c cstring.c -o $(B)/cstring.o
	$(CC64) -c $(TOP)/kernel/arch/arm64/blit.S -o $(B)/blit.o
	$(CC64) -c $(RENAME) $(TOP)/kernel/arch/arm64/string.S -o $(B)/string.o
	$(CC64) $(CFLAGS) -static $(SRC) $(B)/blit_c.o $(B)/cstring.o $(B)/blit.o $(B)/string.o -o $@

bench: bench64 bench32

bench64: $(B)/test
	$(MAKE) -C $(TOP)/tools/qemu-prof
	qemu-aarch64 -plugin $(TOP)/build/qemu-prof/prof.so,out=$(B)/bench.prof $(B)/test bench
	python3 $(TOP)/tools/qemu-prof/report.py $(B)/bench.prof $(B)/test 0 60 | \
		grep -E "_c |_neon |k_mem|c_mem|^instructions"

# ARMv7: the C compiled as the kernel compiles it (arm-none-eabi-gcc,
# soft-float, -O2), the assembler as the kernel's; the test itself with
# zig's C library (soft-float EABI, as the objects)
CC32 ?= zig cc -target arm-linux-musleabi -mcpu=cortex_a8
KCC32 := arm-none-eabi-gcc -march=armv7-a -marm -mfloat-abi=soft -O2 -std=gnu11 -ffreestanding \
	-fno-builtin -fno-common -DBLIT_HOST -I$(TOP)/kernel/include
ASM32 := $(TOP)/kernel/arch/armv7/blit.S $(TOP)/kernel/arch/armv7/string.S
QEMU32 := qemu-arm -cpu cortex-a8

test32: $(B)/test32
	$(QEMU32) $(B)/test32

$(B)/test32: $(SRC) $(ASM32) $(TOP)/kernel/blit.c $(TOP)/kernel/include/blit.h cstring.c n950old.c Makefile
	@mkdir -p $(B)
	$(KCC32) -c $(TOP)/kernel/blit.c -o $(B)/blit_c32.o
	$(KCC32) -c cstring.c -o $(B)/cstring32.o
	$(KCC32) -c n950old.c -o $(B)/n950old32.o
	arm-none-eabi-gcc -march=armv7-a -marm -c $(TOP)/kernel/arch/armv7/blit.S -o $(B)/blit32.o
	arm-none-eabi-gcc -march=armv7-a -marm -c $(RENAME) $(TOP)/kernel/arch/armv7/string.S -o $(B)/string32.o
	$(CC32) -O2 -g -Wall -Wno-unused-parameter -Wno-sign-compare -DBLIT_HOST -I$(TOP)/kernel/include -static \
		$(SRC) $(B)/blit_c32.o $(B)/cstring32.o $(B)/n950old32.o $(B)/blit32.o $(B)/string32.o -o $@

bench32: $(B)/test32
	$(MAKE) -C $(TOP)/tools/qemu-prof
	$(QEMU32) -plugin $(TOP)/build/qemu-prof/prof.so,out=$(B)/bench32.prof $(B)/test32 bench
	python3 $(TOP)/tools/qemu-prof/report.py $(B)/bench32.prof $(B)/test32 0 60 | \
		grep -E "_c |_neon |k_mem|c_mem|_old|^instructions"

.PHONY: test test64 test32 bench bench64 bench32

qemu: qemu64 qemu32

# the ARMv7 kernel's side in QEMU's realview (qemu32.sh): fptest, asmclock,
# the C and NEON screens, SIMD=c
qemu32:
	./qemu32.sh

# the kernel's side in QEMU (virt): the programs' FP/SIMD registers kept
# across switches and the kernel's NEON (apps/fptest), the system call
# and the switch counted (tools/qemu-prof/sysbench.sh), the kernel with
# the C drawing (kolibri.simd=c) and with NEON showing the same screen
qemu64:
	$(MAKE) -C $(TOP) PLATFORM=arm64
	cd $(TOP) && T=$$(mktemp -d /tmp/kos-asm.XXXXXX) && \
	QEMU_MACHINE=virt QEMU_APPEND="kolibri.run=/sys/fptest" python3 scripts/qemu-shot.py \
		build/arm64/kernel.bin $$T/fp wait:15 && grep "^fptest" $$T/fp/serial.log && \
	[ "$$(grep -c 'fptest.*: ok' $$T/fp/serial.log)" = 2 ] && \
	QEMU_MACHINE=virt QEMU_APPEND="kolibri.simd=c" python3 scripts/qemu-shot.py \
		build/arm64/kernel.bin $$T/c wait:8 shot:home && \
	QEMU_MACHINE=virt python3 scripts/qemu-shot.py build/arm64/kernel.bin $$T/neon wait:8 shot:home && \
	grep -q "drawing: C" $$T/c/serial.log && grep -q "drawing: NEON" $$T/neon/serial.log && \
	[ "$$(magick compare -metric AE $$T/c/home.png $$T/neon/home.png /dev/null 2>&1)" = "0 (0)" ] && \
	echo "the screens of C and NEON: the same" && tools/qemu-prof/sysbench.sh

.PHONY: qemu qemu32 qemu64

# The AArch64 kernel's assembler against its C, bit for bit: make -C tests/asm
#   test    the comparisons (test.c) under qemu-aarch64
#   bench   the routines on the screen's sizes under qemu-aarch64 with
#           tools/qemu-prof's plugin: the instructions of each, C and
#           assembler (the device's own times: kolibri.bench, docs/asm.md)
# Needs aarch64-linux-gnu-gcc (static glibc) and qemu-aarch64.
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

test: $(B)/test
	qemu-aarch64 $(B)/test

$(B)/test: $(SRC) $(ASM) $(TOP)/kernel/blit.c $(TOP)/kernel/include/blit.h cstring.c Makefile
	@mkdir -p $(B)
	$(KCC) -c $(TOP)/kernel/blit.c -o $(B)/blit_c.o
	$(KCC) -c cstring.c -o $(B)/cstring.o
	$(CC64) -c $(TOP)/kernel/arch/arm64/blit.S -o $(B)/blit.o
	$(CC64) -c $(RENAME) $(TOP)/kernel/arch/arm64/string.S -o $(B)/string.o
	$(CC64) $(CFLAGS) -static $(SRC) $(B)/blit_c.o $(B)/cstring.o $(B)/blit.o $(B)/string.o -o $@

bench: $(B)/test
	$(MAKE) -C $(TOP)/tools/qemu-prof
	qemu-aarch64 -plugin $(TOP)/build/qemu-prof/prof.so,out=$(B)/bench.prof $(B)/test bench
	python3 $(TOP)/tools/qemu-prof/report.py $(B)/bench.prof $(B)/test 0 60 | \
		grep -E "_c |_neon |k_mem|c_mem|^instructions"

.PHONY: test bench

# KolibriOS for ARM
#
#   make                       the kernel for QEMU (realview-pb-a8) with its ramdisk
#   make run                   ... and start it in QEMU (a window; SDL/GTK)
#   make PLATFORM=n950         the N950's kernel (build/n950/kernel.bin, a raw image
#                              for NOLO's or kexec's 0x80008000)
#   make PLATFORM=arm64        the AArch64 kernel: QEMU's virt and the Pro1X
#                              (build/arm64/kernel.bin, an arm64 Image for kexec)
#
# Needs the Arm GNU toolchain (arm-none-eabi-gcc, its libgcc: the Cortex-A8
# divides in software) and python3; QEMU's qemu-system-arm to run it.

PLATFORM ?= realview
B := build/$(PLATFORM)
A := build/apps
CROSS ?= arm-none-eabi-
CC32 := $(CROSS)gcc
OBJCOPY := $(CROSS)objcopy

ARCH32 := -march=armv7-a -marm -mfloat-abi=soft
WARN := -Wall -Wextra -Wno-unused-parameter -Wno-sign-compare
CFLAGS := $(ARCH32) -O2 -std=gnu11 -ffreestanding -fno-builtin -fno-pic -fno-common $(WARN) -g
ACFLAGS := $(CFLAGS) -Ilib -fno-strict-aliasing
LDFLAGS := -nostdlib -Wl,--no-warn-rwx-segments
LIBGCC := $(shell $(CC32) $(ARCH32) -print-libgcc-file-name)

KCOMMON := lib.c mm.c sched.c syscall.c gfx.c wm.c input.c fs.c loader.c main.c \
	fonts/font8x16.c fonts/font6x10.c rd.S

ifeq ($(PLATFORM),arm64)
# the AArch64 kernel (QEMU's virt, the Pro1X: chosen by the device tree),
# position independent for kexec; its programs stay ARMv7 (AArch32 at EL0)
ARCH := arm64
KCC := clang --target=aarch64-none-elf -mcpu=cortex-a53
KCFLAGS := -O2 -std=gnu11 -ffreestanding -fno-builtin -fpie -fno-common -mgeneral-regs-only -mstrict-align \
	$(WARN) -g -Ikernel/include -Ikernel/arch/arm64
KSRC := $(KCOMMON) arch/arm64/start.S arch/arm64/mmu.c arch/arm64/trap.c arch/arm64/gic3.c \
	fdt.c platform/virt.c platform/pro1x.c
KLINK = ld.lld -pie --no-dynamic-linker -z notext -T kernel/arch/arm64/kernel.ld $(KOBJ) -o $@
else
ARCH := armv7
KCC := $(CC32)
KCFLAGS := $(CFLAGS) -Ikernel/include -Ikernel/arch/armv7
ifeq ($(PLATFORM),n950)
KERNEL_BASE := 0x80008000
KCFLAGS += -DPLATFORM_N950
else
KERNEL_BASE := 0x70100000
endif
KSRC := $(KCOMMON) arch/armv7/start.S arch/armv7/mmu.c arch/armv7/trap.c \
	platform/realview.c platform/n950.c
KLINK = $(CC32) $(LDFLAGS) -T kernel/arch/armv7/kernel.ld -Wl,--defsym,KERNEL_BASE=$(KERNEL_BASE) $(KOBJ) $(LIBGCC) -o $@
endif
KOBJ := $(addprefix $(B)/,$(addsuffix .o,$(basename $(KSRC))))

APPS := taskbar menu example clock board cpu
# names on the ramdisk (KolibriOS's system programs start with @)
RDNAME_taskbar := @taskbar
KEX := $(foreach a,$(APPS),$(A)/$(a).kex)

all: $(B)/kernel.elf $(B)/kernel.bin

$(B)/%.o: kernel/%.c kernel/include/*.h kernel/arch/$(ARCH)/arch.h | dirs
	$(KCC) $(KCFLAGS) -c $< -o $@
$(B)/%.o: kernel/%.S | dirs
	$(KCC) $(KCFLAGS) -c $< -o $@

$(B)/rd.o: kernel/rd.S $(A)/ramdisk.img | dirs
	$(KCC) $(KCFLAGS) -DRAMDISK='"$(A)/ramdisk.img"' -c $< -o $@

$(B)/kernel.elf: $(KOBJ) kernel/arch/$(ARCH)/kernel.ld
	$(KLINK)

$(B)/kernel.bin: $(B)/kernel.elf
	llvm-objcopy -O binary $< $@

# programs
$(A)/lib/%.o: lib/%.c lib/kos.h | dirs
	$(CC32) $(ACFLAGS) -c $< -o $@
$(A)/lib/%.o: lib/%.S | dirs
	$(CC32) $(ACFLAGS) -c $< -o $@
$(A)/%.o: apps/%/*.c lib/kos.h | dirs
	$(CC32) $(ACFLAGS) -c apps/$*/$*.c -o $@
$(A)/%.elf: $(A)/%.o $(A)/lib/crt0.o $(A)/lib/klib.o lib/app.ld
	$(CC32) $(LDFLAGS) -T lib/app.ld $(A)/lib/crt0.o $< $(A)/lib/klib.o $(LIBGCC) -o $@
$(A)/%.kex: $(A)/%.elf
	$(OBJCOPY) -O binary $< $@

$(A)/ramdisk.img: $(KEX) apps/autorun tools/mkrd.py
	python3 tools/mkrd.py $@ autorun=apps/autorun \
		$(foreach a,$(APPS),$(or $(RDNAME_$(a)),$(a))=$(A)/$(a).kex)

dirs:
	@mkdir -p $(B)/platform $(B)/fonts $(B)/arch/$(ARCH) $(A)/lib

QEMU := qemu-system-arm -M realview-pb-a8 -m 256 -rtc base=localtime -serial stdio
run: all
	$(QEMU) -kernel $(B)/kernel.elf

clean:
	rm -rf build

.PHONY: all run clean dirs
.SECONDARY:

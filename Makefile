# KolibriOS for ARM
#
#   make                       the kernel for QEMU (realview-pb-a8) with its ramdisk
#   make run                   ... and start it in QEMU (a window; SDL/GTK)
#   make PLATFORM=n950         the N950's kernel (build/n950/kernel.bin, a raw image
#                              for NOLO's or kexec's 0x80008000)
#
# Needs the Arm GNU toolchain (arm-none-eabi-gcc, its libgcc: the Cortex-A8
# divides in software) and python3; QEMU's qemu-system-arm to run it.

PLATFORM ?= realview
CROSS ?= arm-none-eabi-
CC := $(CROSS)gcc
OBJCOPY := $(CROSS)objcopy
B := build/$(PLATFORM)
A := build/apps

ARCH := -march=armv7-a -marm -mfloat-abi=soft
CFLAGS := $(ARCH) -O2 -std=gnu11 -ffreestanding -fno-builtin -fno-pic -fno-common \
	-Wall -Wextra -Wno-unused-parameter -Wno-sign-compare -g
KCFLAGS := $(CFLAGS) -Ikernel/include
ACFLAGS := $(CFLAGS) -Ilib -fno-strict-aliasing
LDFLAGS := -nostdlib -Wl,--no-warn-rwx-segments
LIBGCC := $(shell $(CC) $(ARCH) -print-libgcc-file-name)

ifeq ($(PLATFORM),n950)
KERNEL_BASE := 0x80008000
KCFLAGS += -DPLATFORM_N950
else
KERNEL_BASE := 0x70100000
endif

KSRC := start.S lib.c mm.c mmu.c sched.c syscall.c gfx.c wm.c input.c fs.c loader.c main.c \
	platform/realview.c platform/n950.c fonts/font8x16.c fonts/font6x10.c rd.S
KOBJ := $(addprefix $(B)/,$(addsuffix .o,$(basename $(KSRC))))

APPS := taskbar menu example clock
# names on the ramdisk (KolibriOS's system programs start with @)
RDNAME_taskbar := @taskbar
KEX := $(foreach a,$(APPS),$(A)/$(a).kex)

all: $(B)/kernel.elf $(B)/kernel.bin

$(B)/%.o: kernel/%.c kernel/include/*.h | dirs
	$(CC) $(KCFLAGS) -c $< -o $@
$(B)/%.o: kernel/%.S | dirs
	$(CC) $(KCFLAGS) -c $< -o $@

$(B)/rd.o: kernel/rd.S $(A)/ramdisk.img | dirs
	$(CC) $(KCFLAGS) -DRAMDISK='"$(A)/ramdisk.img"' -c $< -o $@

$(B)/kernel.elf: $(KOBJ) kernel/kernel.ld
	$(CC) $(LDFLAGS) -T kernel/kernel.ld -Wl,--defsym,KERNEL_BASE=$(KERNEL_BASE) $(KOBJ) $(LIBGCC) -o $@

$(B)/kernel.bin: $(B)/kernel.elf
	$(OBJCOPY) -O binary $< $@

# programs
$(A)/lib/%.o: lib/%.c lib/kos.h | dirs
	$(CC) $(ACFLAGS) -c $< -o $@
$(A)/lib/%.o: lib/%.S | dirs
	$(CC) $(ACFLAGS) -c $< -o $@
$(A)/%.o: apps/%/*.c lib/kos.h | dirs
	$(CC) $(ACFLAGS) -c apps/$*/$*.c -o $@
$(A)/%.elf: $(A)/%.o $(A)/lib/crt0.o $(A)/lib/klib.o lib/app.ld
	$(CC) $(LDFLAGS) -T lib/app.ld $(A)/lib/crt0.o $< $(A)/lib/klib.o $(LIBGCC) -o $@
$(A)/%.kex: $(A)/%.elf
	$(OBJCOPY) -O binary $< $@

$(A)/ramdisk.img: $(KEX) apps/autorun tools/mkrd.py
	python3 tools/mkrd.py $@ autorun=apps/autorun \
		$(foreach a,$(APPS),$(or $(RDNAME_$(a)),$(a))=$(A)/$(a).kex)

dirs:
	@mkdir -p $(B)/platform $(B)/fonts $(A)/lib

QEMU := qemu-system-arm -M realview-pb-a8 -m 256 -rtc base=localtime -serial stdio
run: all
	$(QEMU) -kernel $(B)/kernel.elf

clean:
	rm -rf build

.PHONY: all run clean dirs
.SECONDARY:

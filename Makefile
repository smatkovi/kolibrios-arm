# KolibriOS for ARM
#
#   make                       the kernel for QEMU (realview-pb-a8) with its ramdisk
#   make run                   ... and start it in QEMU (a window; SDL/GTK)
#   make PLATFORM=n950         the N950's kernel (build/n950/kernel.bin, a raw image
#                              for NOLO's or kexec's 0x80008000)
#   make PLATFORM=arm64        the AArch64 kernel: QEMU's virt and the Pro1X
#                              (build/arm64/kernel.bin, an arm64 Image for kexec)
#   make LOOK=classic          KolibriOS's own panel and menu instead of the
#                              Hildon shell
#   make SIMD=c                the kernel without its assembler's drawing loops
#                              and string functions (NEON; docs/asm.md)
#   make CMDLINE="..."         arguments built into the kernel, before the boot
#                              loader's (e.g. PLATFORM=n950 CMDLINE=kolibri.bench)
#
# Needs the Arm GNU toolchain (arm-none-eabi-gcc, its libgcc: the Cortex-A8
# divides in software) and python3; QEMU's qemu-system-arm to run it.
# The programs in assembler need FASMARM (fetched: tools/fetch-fasmarm.sh).

PLATFORM ?= realview
.DEFAULT_GOAL := all
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

# storage (docs/storage.md): the block layer, the file systems, the
# container; the drivers by kernel below
STORAGE := storage/blk.c storage/ext4.c storage/fat.c storage/home.c storage/dev.c storage/mmc.c storage/sdhci.c
KCOMMON := lib.c mm.c sched.c syscall.c gfx.c blit.c wm.c input.c fs.c kfs.c serial.c loader.c main.c \
	proc.c dll.c $(STORAGE) \
	fonts/font8x16.c fonts/font6x10.c rd.S \
	modem/smem.c modem/glink.c modem/qrtr.c modem/qmi.c modem/rmtfs.c modem/sms.c \
	modem/phone.c modem/fake.c modem/kmodem.c modem/apr.c modem/callaudio.c modem/wlfw.c modem/ce.c

ifeq ($(PLATFORM),arm64)
# the AArch64 kernel (QEMU's virt, the Pro1X: chosen by the device tree),
# position independent for kexec; its programs stay ARMv7 (AArch32 at EL0)
ARCH := arm64
KCC := clang --target=aarch64-none-elf -mcpu=cortex-a53
KCFLAGS := -O2 -std=gnu11 -ffreestanding -fno-builtin -fpie -fno-common -mgeneral-regs-only -mstrict-align \
	$(WARN) -g -Ikernel/include -Ikernel/arch/arm64
KSRC := $(KCOMMON) arch/arm64/start.S arch/arm64/mmu.c arch/arm64/trap.c arch/arm64/gic3.c \
	arch/arm64/timer.c arch/arm64/psci.c arch/arm64/powerdt.c arch/arm64/suspend.S arch/arm64/fpu.c power.c \
	fdt.c platform/virt.c platform/virtio_input.c platform/pro1x.c net/net.c net/virtio_net.c net/virtio_rng.c net/wifi_dev.c rng.c modem/rpm.c modem/rpmsleep.c modem/pmic.c \
	modem/htc.c modem/wmi.c modem/htt.c modem/wlan.c modem/wpa.c modem/crypto.c modem/wifi.c modem/regd.c modem/regdb.c modem/ecc.c modem/sae.c \
	linux/lx.c linux/vm.c linux/sys.c linux/file.c linux/sock.c linux/bridge.c linux/fp.S \
	storage/virtio_blk.c storage/pci.c storage/ufs.c
# the hot loops in assembler (arch/arm64/blit.S with NEON, string.S);
# SIMD=c builds the kernel with their C versions only (kernel/blit.c,
# lib.c); kolibri.simd=c on the command line chooses C at the start
SIMD ?= neon
ifeq ($(SIMD),c)
KCFLAGS += -DSIMD_C
else
KSRC += arch/arm64/blit.S arch/arm64/string.S bench.c
endif
# the network: lwIP (tools/fetch-lwip.sh puts it in third_party/lwip)
LWIP := third_party/lwip/src
LWIPSRC := core/init.c core/def.c core/dns.c core/inet_chksum.c core/ip.c core/mem.c core/memp.c \
	core/netif.c core/pbuf.c core/tcp.c core/tcp_in.c core/tcp_out.c core/timeouts.c core/udp.c \
	core/ipv4/dhcp.c core/ipv4/etharp.c core/ipv4/icmp.c core/ipv4/ip4.c core/ipv4/ip4_addr.c \
	core/ipv4/ip4_frag.c core/ipv4/acd.c netif/ethernet.c
LWIPOBJ := $(addprefix $(B)/lwip/,$(LWIPSRC:.c=.o))
KCFLAGS += -Ikernel/net -Ikernel/net/include -I$(LWIP)/include
KLINK = ld.lld -pie --no-dynamic-linker -z notext -T kernel/arch/arm64/kernel.ld $(KOBJ) $(LWIPOBJ) -o $@
else
ARCH := armv7
KCC := $(CC32)
KCFLAGS := $(CFLAGS) -Ikernel/include -Ikernel/arch/armv7
ifeq ($(PLATFORM),n950)
KERNEL_BASE := 0x80008000
KCFLAGS += -DPLATFORM_N950
else
# QEMU loads a raw image (kernel.bin) 64 KiB into the RAM, as a Linux
# zImage, with its ATAGs (-append: the command line); kernel.elf there too
KERNEL_BASE := 0x70010000
endif
KSRC := $(KCOMMON) arch/armv7/start.S arch/armv7/mmu.c arch/armv7/trap.c arch/armv7/fpu.c arch/armv7/vfp.S \
	platform/realview.c platform/n950.c storage/pl181.c
# the hot loops in assembler with NEON (arch/armv7/blit.S, string.S;
# kolibri.bench's clock, arch/armv7/cycles.c), as on AArch64: SIMD=c
# builds the kernel with their C versions only, kolibri.simd=c chooses C
# at the start; the programs' VFP and NEON are lazy either way (fpu.c)
SIMD ?= neon
ifeq ($(SIMD),c)
KCFLAGS += -DSIMD_C
else
KSRC += arch/armv7/blit.S arch/armv7/string.S arch/armv7/cycles.c bench.c
endif
KLINK = $(CC32) $(LDFLAGS) -T kernel/arch/armv7/kernel.ld -Wl,--defsym,KERNEL_BASE=$(KERNEL_BASE) $(KOBJ) $(LIBGCC) -o $@
endif
KOBJ := $(addprefix $(B)/,$(addsuffix .o,$(basename $(KSRC))))

APPS := taskbar menu example clock board cpu hildon hstatus phone messenger sysbench fptest
# programs of several files or with the C library (below: SRC_name ...),
# each described by its apps/NAME/app.mk
CAPPS :=
# KolibriOS's sources of the ported programs (tools/fetch-kolibrios.sh)
KOS := third_party/kolibrios
include $(sort $(wildcard apps/*/app.mk))
# names on the ramdisk (KolibriOS's system programs start with @)
RDNAME_taskbar := @taskbar
RDNAME_hildon := @hildon
RDNAME_hstatus := @hstatus
# memory beyond the default 1 MiB (the shell keeps the task switcher's pictures)
MEM_hildon := 0x400000
MEM_phone := 0x400000
MEM_messenger := 0x800000
# programs in assembler (FASMARM, the flat assembler for ARM, with
# lib/kos.inc: docs/asm.md); tools/fetch-fasmarm.sh puts it in
# third_party/fasmarm (x86 Linux hosts); make ASM_APPS= leaves them out
ASM_APPS ?= hello asmclock
FASMARM := third_party/fasmarm/$(if $(filter x86_64,$(shell uname -m)),fasmarm.x64,fasmarm)
# the look the kernel starts with: hildon (the Hildon shell) or classic
# (KolibriOS's panel and menu): apps/autorun or apps/autorun.classic
LOOK ?= hildon
AUTORUN := $(if $(filter classic,$(LOOK)),apps/autorun.classic,apps/autorun)
KEX := $(foreach a,$(APPS) $(CAPPS) $(ASM_APPS),$(A)/$(a).kex)
# the programs' data files on the ramdisk (PATH=FILE)
RD_FILES := $(foreach a,$(APPS) $(CAPPS),$(RD_$(a)))

all: $(B)/kernel.elf $(B)/kernel.bin

$(B)/%.o: kernel/%.c kernel/include/*.h kernel/modem/*.h kernel/storage/*.h lib/kos_modem.h kernel/arch/$(ARCH)/arch.h $(B)/flags | dirs
	$(KCC) $(KCFLAGS) -c $< -o $@
$(B)/%.o: kernel/%.S $(B)/flags | dirs
	$(KCC) $(KCFLAGS) -c $< -o $@

# the compiler's flags in a file, written when they change (SIMD=c and
# back): the kernel's objects follow them
$(B)/flags: FORCE | dirs
	@[ "$$(cat $@ 2> /dev/null)" = "$(KCFLAGS)" ] || echo "$(KCFLAGS)" > $@

$(LWIP)/core/init.c:
	tools/fetch-lwip.sh
$(B)/linux/%.o: kernel/linux/%.c kernel/linux/*.h kernel/include/*.h kernel/arch/arm64/frame.h $(LWIP)/core/init.c | dirs
	$(KCC) $(KCFLAGS) -c $< -o $@
$(B)/linux/%.o: kernel/linux/%.S | dirs
	$(KCC) $(KCFLAGS) -c $< -o $@
$(B)/net/%.o: kernel/net/%.c kernel/net/*.h kernel/net/arch/cc.h kernel/include/*.h $(LWIP)/core/init.c | dirs
	$(KCC) $(KCFLAGS) -c $< -o $@
$(B)/lwip/%.o: $(LWIP)/%.c kernel/net/lwipopts.h kernel/net/arch/cc.h | dirs
	@mkdir -p $(dir $@)
	$(KCC) $(KCFLAGS) -Wno-unused-but-set-variable -c $< -o $@

# the arguments built in (CMDLINE): a header, written when they change
CMDLINE ?=
$(B)/cmdline.h: FORCE | dirs
	@printf '#define KOLIBRI_CMDLINE "%s"\n' '$(subst ',,$(subst ",\",$(CMDLINE)))' > $@.new
	@cmp -s $@.new $@ && rm $@.new || mv $@.new $@
$(B)/main.o: kernel/main.c $(B)/cmdline.h kernel/include/*.h kernel/arch/$(ARCH)/arch.h $(B)/flags | dirs
	$(KCC) $(KCFLAGS) -include $(B)/cmdline.h -c $< -o $@

$(B)/rd.o: kernel/rd.S $(A)/ramdisk.img | dirs
	$(KCC) $(KCFLAGS) -DRAMDISK='"$(A)/ramdisk.img"' -c $< -o $@

$(B)/kernel.elf: $(KOBJ) $(LWIPOBJ) kernel/arch/$(ARCH)/kernel.ld
	$(KLINK)

$(B)/kernel.bin: $(B)/kernel.elf
	llvm-objcopy -O binary $< $@

# programs
$(A)/lib/%.o: lib/%.c lib/kos.h | dirs
	$(CC32) $(ACFLAGS) -c $< -o $@
$(A)/lib/%.o: lib/%.S | dirs
	$(CC32) $(ACFLAGS) -c $< -o $@
$(A)/%.o: apps/%/*.c lib/kos.h lib/kos_modem.h lib/qr.h lib/json.h | dirs
	$(CC32) $(ACFLAGS) -c apps/$*/$*.c -o $@
$(A)/%.elf: $(A)/%.o $(A)/lib/crt0.o $(A)/lib/klib.o lib/app.ld
	$(CC32) $(LDFLAGS) $(if $(MEM_$*),-Wl$(comma)--defsym$(comma)MEMORY_SIZE=$(MEM_$*)) \
		-T lib/app.ld $(A)/lib/crt0.o $< $(A)/lib/klib.o $(LIBGCC) -o $@
$(A)/%.kex: $(A)/%.elf
	$(OBJCOPY) -O binary $< $@

# the libraries of programs, lib/NAME/*.c into build/apps/lib/libNAME.a:
# libc (a C library for the ported programs: lib/libc/include), console
# (console.obj's functions: a console window), libini (libini.obj's:
# settings files), boxlib (box_lib.obj's widgets), msgbox, proc_lib,
# kmenu. KolibriOS loads them as shared libraries (function 68.19): DLLS
# below are built so too, and the programs load them (DLL=0: they are
# linked into each program instead; docs/threads.md)
LIBS := $(sort $(patsubst lib/%/,%,$(dir $(wildcard lib/*/*.c))))
LIBC_INC := -nostdinc -isystem $(shell $(CC32) -print-file-name=include) -Ilib/libc/include -Ilib \
	$(foreach l,$(LIBS),-Ilib/$(l)) -I$(A)/gen
# a library's own rules (generated headers; LIBCFLAGS_NAME, its flags
# besides these): lib/NAME/lib.mk
include $(sort $(wildcard lib/*/lib.mk))
define CLIB
$(1)_LOBJ := $$(patsubst lib/$(1)/%.c,$(A)/lib/$(1)/%.o,$$(wildcard lib/$(1)/*.c))
$(A)/lib/$(1)/%.o: lib/$(1)/%.c | dirs
	@mkdir -p $$(dir $$@)
	$(CC32) $(ACFLAGS) $(LIBC_INC) $$(LIBCFLAGS_$(1)) -MMD -MP -c $$< -o $$@
$(A)/lib/$(1).a: $$($(1)_LOBJ)
	@rm -f $$@
	$(CROSS)ar rcs $$@ $$^
-include $$($(1)_LOBJ:.o=.d)
endef
$(foreach l,$(LIBS),$(eval $(call CLIB,$(l))))
$(A)/lib/crt0-libc.o: lib/crt0.S | dirs
	$(CC32) $(ACFLAGS) -DLIBC -c $< -o $@

# the libraries as KolibriOS loads them (68.19, kernel/dll.c), NAME:FILE:
# lib/NAME as /sys/lib/FILE, an ARM ELF shared object, position
# independent, with the C library and libgcc in it, its export table
# from lib/NAME/NAME.exp (tools/mkdll.py; docs/threads.md). A program
# links lib/NAME's import table and stubs instead of lib/NAME itself
# (lib/NAME/import/*.c with them: what KolibriOS's programs have
# themselves, macros and the like); DLL=0 links the libraries in
DLL ?= 1
DLLS := console:console.obj boxlib:box_lib.obj libini:libini.obj kfar_arc:kfar_arc.obj
DLL_NAMES := $(foreach d,$(DLLS),$(firstword $(subst :, ,$(d))))
P := $(A)/pic
PICFLAGS := -fPIC -fvisibility=hidden
DLL_LDFLAGS := -shared -nostdlib -Wl,-Bsymbolic -Wl,--hash-style=sysv -Wl,-z,max-page-size=4096 \
	-Wl,--no-undefined -Wl,-s -Wl,--no-warn-rwx-segments
# a library of a program: its import side or the library itself
libfile = $(if $(and $(filter 1,$(DLL)),$(filter $(1),$(DLL_NAMES))),$(A)/lib/$(1)-imp.a,$(A)/lib/$(1).a)
LIBC_PIC_OBJ := $(patsubst lib/libc/%.c,$(P)/libc/%.o,$(filter-out lib/libc/start.c lib/libc/malloc.c \
	lib/libc/dll.c,$(wildcard lib/libc/*.c)))
$(P)/libc/%.o: lib/libc/%.c | dirs
	@mkdir -p $(dir $@)
	$(CC32) $(ACFLAGS) $(PICFLAGS) $(LIBC_INC) -MMD -MP -c $< -o $@
$(P)/libc.a: $(LIBC_PIC_OBJ)
	@rm -f $@
	$(CROSS)ar rcs $@ $^
$(P)/dllrt.o: lib/dllrt.c lib/kos_dll.h lib/kos.h | dirs
	@mkdir -p $(dir $@)
	$(CC32) $(ACFLAGS) $(PICFLAGS) $(LIBC_INC) -c $< -o $@
-include $(LIBC_PIC_OBJ:.o=.d)
define DLL_LIB
$(1)_POBJ := $$(patsubst lib/$(1)/%.c,$(P)/$(1)/%.o,$$(wildcard lib/$(1)/*.c))
$(P)/$(1)/%.o: lib/$(1)/%.c | dirs
	@mkdir -p $$(dir $$@)
	$(CC32) $(ACFLAGS) $(PICFLAGS) $(LIBC_INC) $$(LIBCFLAGS_$(1)) -MMD -MP -c $$< -o $$@
$(P)/$(1)/exports.S: lib/$(1)/$(1).exp tools/mkdll.py | dirs
	@mkdir -p $$(dir $$@)
	python3 -I tools/mkdll.py exports $$< $$@
$(P)/$(1)/exports.o: $(P)/$(1)/exports.S
	$(CC32) $(ACFLAGS) -c $$< -o $$@
$(A)/lib/$(2): $$($(1)_POBJ) $(P)/$(1)/exports.o $(P)/dllrt.o $(P)/libc.a tools/mkdll.py
	$(CC32) $(ARCH32) $(DLL_LDFLAGS) $$($(1)_POBJ) $(P)/$(1)/exports.o $(P)/dllrt.o $(P)/libc.a $(LIBGCC) -o $$@
	python3 -I tools/mkdll.py check $$@
$(A)/lib/$(1)-imp.S: lib/$(1)/$(1).exp tools/mkdll.py | dirs
	python3 -I tools/mkdll.py imports $$< $(2) $$@
$(A)/lib/$(1)-imp.o: $(A)/lib/$(1)-imp.S
	$(CC32) $(ACFLAGS) -c $$< -o $$@
$(1)_IOBJ := $(A)/lib/$(1)-imp.o $$(patsubst lib/$(1)/import/%.c,$(A)/lib/$(1)/import/%.o,$$(wildcard lib/$(1)/import/*.c))
$(A)/lib/$(1)/import/%.o: lib/$(1)/import/%.c | dirs
	@mkdir -p $$(dir $$@)
	$(CC32) $(ACFLAGS) $(LIBC_INC) -MMD -MP -c $$< -o $$@
$(A)/lib/$(1)-imp.a: $$($(1)_IOBJ)
	@rm -f $$@
	$(CROSS)ar rcs $$@ $$^
-include $$($(1)_POBJ:.o=.d)
endef
$(foreach d,$(DLLS),$(eval $(call DLL_LIB,$(firstword $(subst :, ,$(d))),$(lastword $(subst :, ,$(d))))))
# DLL in a file, written when it changes: the programs follow it
$(A)/dll-mode: FORCE | dirs
	@[ "$$(cat $@ 2> /dev/null)" = "$(DLL)" ] || echo "$(DLL)" > $@
# on the ramdisk in /sys/lib, as in KolibriOS
DLL_FILES := $(foreach d,$(DLLS),$(A)/lib/$(lastword $(subst :, ,$(d))))
RD_DLLS := $(foreach d,$(DLLS),lib/$(lastword $(subst :, ,$(d)))=$(A)/lib/$(lastword $(subst :, ,$(d))))

# programs of several files, or with the C library: CAPPS, each with
# SRC_name (its C files: apps/name/..., third_party/kolibrios/...),
# CFLAGS_name, LIBS_name (besides libc), MEM_name, and RD_name, its data
# files on the ramdisk (PATH=FILE, PATH under /sys). They start with
# main(argc, argv) (lib/libc/start.c)
define CAPP
$(1)_OBJ := $$(patsubst %.c,$(A)/$(1)/%.o,$$(SRC_$(1)))
$(A)/$(1)/%.o: %.c | dirs
	@mkdir -p $$(dir $$@)
	$(CC32) $(ACFLAGS) $(LIBC_INC) $$(CFLAGS_$(1)) -MMD -MP -c $$< -o $$@
$(A)/$(1).elf: $$($(1)_OBJ) $(A)/lib/crt0-libc.o $$(foreach l,$$(LIBS_$(1)) libc,$$(call libfile,$$(l))) lib/app.ld $(A)/dll-mode
	$(CC32) $(LDFLAGS) $$(if $$(MEM_$(1)),-Wl$$(comma)--defsym$$(comma)MEMORY_SIZE=$$(MEM_$(1))) \
		-T lib/app.ld $(A)/lib/crt0-libc.o $$($(1)_OBJ) \
		-Wl,--start-group $$(foreach l,$$(LIBS_$(1)) libc,$$(call libfile,$$(l))) -Wl,--end-group $(LIBGCC) -o $$@
-include $$($(1)_OBJ:.o=.d)
endef
$(foreach a,$(CAPPS),$(eval $(call CAPP,$(a))))

# KolibriOS's sources (tools/fetch-kolibrios.sh): the ported programs'
$(KOS)/%:
	tools/fetch-kolibrios.sh

# programs in assembler: apps/NAME/NAME.asm, FASMARM straight to the .kex
$(FASMARM):
	tools/fetch-fasmarm.sh
define ASM_APP
$(A)/$(1).kex: apps/$(1)/$(1).asm lib/kos.inc | $(FASMARM) dirs
	INCLUDE=$(CURDIR)/lib $(FASMARM) $$< $$@
endef
$(foreach a,$(ASM_APPS),$(eval $(call ASM_APP,$(a))))

# Linux programs and files for the AArch64 kernel's Linux personality,
# NAME=FILE with NAME the path under / (bin/hello=tests/linux/hello/hello):
# on the ramdisk as linux/NAME
LINUX_FILES ?=
LINUX_DEPS := $(foreach f,$(LINUX_FILES),$(lastword $(subst =, ,$(f))))

comma := ,
$(A)/ramdisk.img: $(KEX) $(AUTORUN) apps/autorun apps/autorun.classic tools/mkrd.py $(A)/look Makefile $(LINUX_DEPS) $(A)/linux-files \
		$(foreach f,$(RD_FILES),$(lastword $(subst =, ,$(f)))) $(DLL_FILES)
	python3 tools/mkrd.py $@ autorun=$(AUTORUN) \
		autorun.classic=apps/autorun.classic autorun.hildon=apps/autorun \
		$(foreach a,$(APPS) $(CAPPS) $(ASM_APPS),$(or $(RDNAME_$(a)),$(a))=$(A)/$(a).kex) \
		$(RD_FILES) $(RD_DLLS) \
		$(foreach f,$(LINUX_FILES),linux/$(f))

# the list in a file, written when it changes: the ramdisk follows it
$(A)/linux-files: FORCE | dirs
	@[ "$$(cat $@ 2> /dev/null)" = "$(LINUX_FILES)" ] || echo "$(LINUX_FILES)" > $@

# the look (and the autorun file, AUTORUN=... for tests) in a file,
# written when it changes: the ramdisk follows it
$(A)/look: FORCE | dirs
	@[ "$$(cat $@ 2> /dev/null)" = "$(LOOK) $(AUTORUN)" ] || echo "$(LOOK) $(AUTORUN)" > $@
FORCE:

dirs:
	@mkdir -p $(B)/platform $(B)/fonts $(B)/modem $(B)/net $(B)/linux $(B)/storage $(B)/arch/$(ARCH) $(A)/lib

QEMU := qemu-system-arm -M realview-pb-a8 -m 256 -rtc base=localtime -serial stdio
run: all
	$(QEMU) -kernel $(B)/kernel.elf

# the AArch64 kernel in QEMU's virt with a network card (user networking:
# DHCP, DNS, the host as 10.0.2.2) and the kernel's network self test,
# answered by a small HTTP server on the host (NETTEST_PORT)
QEMU64 := qemu-system-aarch64 -M virt,gic-version=3 -cpu cortex-a53 -m 1024 -device ramfb \
	-global virtio-mmio.force-legacy=false -device virtio-net-device,netdev=n0 -netdev user,id=n0 \
	-device virtio-rng-device -device virtio-keyboard-device -device virtio-tablet-device \
	-serial stdio
NETTEST_PORT ?= 18765
run-net: all
	@echo "the host's test server: python3 -m http.server --bind 127.0.0.1 $(NETTEST_PORT)"
	$(QEMU64) -kernel $(B)/kernel.bin -append "kolibri.nettest=$(NETTEST_PORT)"

# the Linux personality's test: a static Go program (tests/linux/hello,
# built by Go for linux/arm64) started by the kernel
tests/linux/hello/hello: tests/linux/hello/hello.go
	cd tests/linux/hello && CGO_ENABLED=0 GOOS=linux GOARCH=arm64 go build -trimpath -ldflags=-s -o hello .
run-linux: tests/linux/hello/hello
	$(MAKE) PLATFORM=arm64 LINUX_FILES="bin/hello=tests/linux/hello/hello" all
	$(QEMU64) -kernel build/arm64/kernel.bin -append "kolibri.run=/bin/hello"

# the messenger with Yappari's daemon: YAPPARI_WA, yappari-wa built for
# linux/arm64 (CGO_ENABLED=0 GOOS=linux GOARCH=arm64 go build in
# yappari-leste's backend/), and a CA bundle
YAPPARI_WA ?=
CA_BUNDLE ?= /etc/ssl/certs/ca-certificates.crt
run-messenger:
	@[ -f "$(YAPPARI_WA)" ] || { echo "YAPPARI_WA=<yappari-wa for linux/arm64>"; exit 1; }
	$(MAKE) PLATFORM=arm64 LINUX_FILES="bin/yappari-wa=$(YAPPARI_WA) etc/ssl/certs/ca-certificates.crt=$(CA_BUNDLE)" all
	$(QEMU64) -kernel build/arm64/kernel.bin -append "kolibri.run=/sys/messenger"

# the messenger with Telegram: PURPLE_HOST, tools/purple-host/build.sh's
# purple-host; TG_NUMBER: a phone number to log in with at once (tests)
PURPLE_HOST ?=
TG_NUMBER ?=
run-telegram:
	@[ -f "$(PURPLE_HOST)" ] || { echo "PURPLE_HOST=<tools/purple-host/build.sh's purple-host>"; exit 1; }
	$(MAKE) PLATFORM=arm64 LINUX_FILES="bin/purple-host=$(PURPLE_HOST) etc/ssl/certs/ca-certificates.crt=$(CA_BUNDLE)" all
	$(QEMU64) -kernel build/arm64/kernel.bin \
		-append "kolibri.run=/sys/messenger kolibri.args=telegram$(if $(TG_NUMBER),$(comma)$(TG_NUMBER))"

# the messenger with Signal: SIGNAL_HOST, tools/signal-host/build.sh's
# signal-host (presage); it shows the linking QR code from Signal's server
SIGNAL_HOST ?=
run-signal:
	@[ -f "$(SIGNAL_HOST)" ] || { echo "SIGNAL_HOST=<tools/signal-host/build.sh's signal-host>"; exit 1; }
	$(MAKE) PLATFORM=arm64 LINUX_FILES="bin/signal-host=$(SIGNAL_HOST)" all
	$(QEMU64) -kernel build/arm64/kernel.bin -append "kolibri.run=/sys/messenger kolibri.args=signal"

# the messenger's chats against a fake backend (tests/messenger/fakepeer:
# fixtures, no network, no account) as all three services' programs;
# MESSENGER_SERVICE: whatsapp, telegram or signal (none: it asks)
FAKEPEER := tests/messenger/fakepeer/fakepeer
FAKEPEER_FILES := bin/yappari-wa=$(FAKEPEER) bin/purple-host=$(FAKEPEER) bin/signal-host=$(FAKEPEER)
MESSENGER_SERVICE ?= whatsapp
$(FAKEPEER): tests/messenger/fakepeer/main.go
	cd tests/messenger/fakepeer && CGO_ENABLED=0 GOOS=linux GOARCH=arm64 go build -trimpath -ldflags=-s -o fakepeer .
run-chats: $(FAKEPEER)
	$(MAKE) PLATFORM=arm64 LINUX_FILES="$(FAKEPEER_FILES)" all
	$(QEMU64) -kernel build/arm64/kernel.bin \
		-append "kolibri.run=/sys/messenger$(if $(MESSENGER_SERVICE), kolibri.args=$(MESSENGER_SERVICE))"
# the same, checked by screenshots (tests/messenger/chatcheck.py), and the
# JSON reader and the real backends' protocol on the PC (tests/messenger/Makefile)
# (PURPLE_HOST=...: also the real purple-host with its test protocol)
test-chats: $(FAKEPEER)
	$(MAKE) -C tests/messenger PURPLE_HOST=$(PURPLE_HOST) SIGNAL_HOST=$(SIGNAL_HOST) YAPPARI_WA=$(YAPPARI_WA)
	$(MAKE) PLATFORM=arm64 LINUX_FILES="$(FAKEPEER_FILES)" all
	python3 tests/messenger/chatcheck.py build/arm64/kernel.bin whatsapp
	python3 tests/messenger/chatcheck.py build/arm64/kernel.bin telegram
	python3 tests/messenger/chatcheck.py build/arm64/kernel.bin signal
	$(if $(PURPLE_HOST),$(MAKE) PLATFORM=arm64 LINUX_FILES="bin/purple-host=$(PURPLE_HOST) etc/purple-host-test=tests/messenger/purple-host-test" all)
	$(if $(PURPLE_HOST),python3 tests/messenger/chatcheck.py build/arm64/kernel.bin telegram-purple)

clean:
	rm -rf build

.PHONY: all run run-net run-linux run-messenger run-telegram run-signal run-chats test-chats clean dirs FORCE
.SECONDARY:

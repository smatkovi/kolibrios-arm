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
	fonts/font8x16.c fonts/font6x10.c rd.S \
	modem/smem.c modem/glink.c modem/qrtr.c modem/qmi.c modem/rmtfs.c modem/sms.c \
	modem/phone.c modem/fake.c modem/kmodem.c modem/apr.c modem/callaudio.c modem/wlfw.c

ifeq ($(PLATFORM),arm64)
# the AArch64 kernel (QEMU's virt, the Pro1X: chosen by the device tree),
# position independent for kexec; its programs stay ARMv7 (AArch32 at EL0)
ARCH := arm64
KCC := clang --target=aarch64-none-elf -mcpu=cortex-a53
KCFLAGS := -O2 -std=gnu11 -ffreestanding -fno-builtin -fpie -fno-common -mgeneral-regs-only -mstrict-align \
	$(WARN) -g -Ikernel/include -Ikernel/arch/arm64
KSRC := $(KCOMMON) arch/arm64/start.S arch/arm64/mmu.c arch/arm64/trap.c arch/arm64/gic3.c \
	fdt.c platform/virt.c platform/pro1x.c net/net.c net/virtio_net.c net/virtio_rng.c rng.c modem/rpm.c \
	linux/lx.c linux/vm.c linux/sys.c linux/file.c linux/sock.c linux/bridge.c linux/fp.S
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
KERNEL_BASE := 0x70100000
endif
KSRC := $(KCOMMON) arch/armv7/start.S arch/armv7/mmu.c arch/armv7/trap.c \
	platform/realview.c platform/n950.c
KLINK = $(CC32) $(LDFLAGS) -T kernel/arch/armv7/kernel.ld -Wl,--defsym,KERNEL_BASE=$(KERNEL_BASE) $(KOBJ) $(LIBGCC) -o $@
endif
KOBJ := $(addprefix $(B)/,$(addsuffix .o,$(basename $(KSRC))))

APPS := taskbar menu example clock board cpu hildon hstatus phone messenger
# names on the ramdisk (KolibriOS's system programs start with @)
RDNAME_taskbar := @taskbar
RDNAME_hildon := @hildon
RDNAME_hstatus := @hstatus
# memory beyond the default 1 MiB (the shell keeps the task switcher's pictures)
MEM_hildon := 0x400000
MEM_phone := 0x400000
MEM_messenger := 0x200000
# the look the kernel starts with: hildon (the Hildon shell) or classic
# (KolibriOS's panel and menu): apps/autorun or apps/autorun.classic
LOOK ?= hildon
AUTORUN := $(if $(filter classic,$(LOOK)),apps/autorun.classic,apps/autorun)
KEX := $(foreach a,$(APPS),$(A)/$(a).kex)

all: $(B)/kernel.elf $(B)/kernel.bin

$(B)/%.o: kernel/%.c kernel/include/*.h kernel/modem/*.h lib/kos_modem.h kernel/arch/$(ARCH)/arch.h | dirs
	$(KCC) $(KCFLAGS) -c $< -o $@
$(B)/%.o: kernel/%.S | dirs
	$(KCC) $(KCFLAGS) -c $< -o $@

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
$(A)/%.o: apps/%/*.c lib/kos.h lib/kos_modem.h lib/qr.h | dirs
	$(CC32) $(ACFLAGS) -c apps/$*/$*.c -o $@
$(A)/%.elf: $(A)/%.o $(A)/lib/crt0.o $(A)/lib/klib.o lib/app.ld
	$(CC32) $(LDFLAGS) $(if $(MEM_$*),-Wl$(comma)--defsym$(comma)MEMORY_SIZE=$(MEM_$*)) \
		-T lib/app.ld $(A)/lib/crt0.o $< $(A)/lib/klib.o $(LIBGCC) -o $@
$(A)/%.kex: $(A)/%.elf
	$(OBJCOPY) -O binary $< $@

# Linux programs and files for the AArch64 kernel's Linux personality,
# NAME=FILE with NAME the path under / (bin/hello=tests/linux/hello/hello):
# on the ramdisk as linux/NAME
LINUX_FILES ?=
LINUX_DEPS := $(foreach f,$(LINUX_FILES),$(lastword $(subst =, ,$(f))))

comma := ,
$(A)/ramdisk.img: $(KEX) $(AUTORUN) apps/autorun apps/autorun.classic tools/mkrd.py $(A)/look Makefile $(LINUX_DEPS) $(A)/linux-files
	python3 tools/mkrd.py $@ autorun=$(AUTORUN) \
		autorun.classic=apps/autorun.classic autorun.hildon=apps/autorun \
		$(foreach a,$(APPS),$(or $(RDNAME_$(a)),$(a))=$(A)/$(a).kex) \
		$(foreach f,$(LINUX_FILES),linux/$(f))

# the list in a file, written when it changes: the ramdisk follows it
$(A)/linux-files: FORCE | dirs
	@[ "$$(cat $@ 2> /dev/null)" = "$(LINUX_FILES)" ] || echo "$(LINUX_FILES)" > $@

# the look in a file, written when it changes: the ramdisk follows it
$(A)/look: FORCE | dirs
	@[ "$$(cat $@ 2> /dev/null)" = "$(LOOK)" ] || echo "$(LOOK)" > $@
FORCE:

dirs:
	@mkdir -p $(B)/platform $(B)/fonts $(B)/modem $(B)/net $(B)/linux $(B)/arch/$(ARCH) $(A)/lib

QEMU := qemu-system-arm -M realview-pb-a8 -m 256 -rtc base=localtime -serial stdio
run: all
	$(QEMU) -kernel $(B)/kernel.elf

# the AArch64 kernel in QEMU's virt with a network card (user networking:
# DHCP, DNS, the host as 10.0.2.2) and the kernel's network self test,
# answered by a small HTTP server on the host (NETTEST_PORT)
QEMU64 := qemu-system-aarch64 -M virt,gic-version=3 -cpu cortex-a53 -m 1024 -device ramfb \
	-global virtio-mmio.force-legacy=false -device virtio-net-device,netdev=n0 -netdev user,id=n0 \
	-device virtio-rng-device \
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

clean:
	rm -rf build

.PHONY: all run run-net run-linux run-messenger run-telegram clean dirs FORCE
.SECONDARY:

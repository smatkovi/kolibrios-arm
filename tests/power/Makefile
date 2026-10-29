# The AArch64 kernel's power management: make -C tests/power
#
#   host   powerdt.c on the PC against the SM6115's tree (sm6115-power.dts,
#          and the phone's own with PRO1X_DTB=...) and QEMU's; pmic.c (the
#          SPMI arbiter, the battery) against an arbiter in RAM
#   qemu   the kernel in QEMU (builds it first); each run prints the
#          kernel's power lines and checks them (qemu.py): the 100 Hz tick
#          as it was, tickless idle on QEMU's virt and on the Pro1X's code
#          paths (a tree of the Pro1X's kind, mkdtb.py), CPU_SUSPEND into
#          the core's state and the cluster's, and the power-down played
#          (kolibri.psci=simulate: the way back through cpu_resume)
TOP := ../..
K := $(TOP)/build/arm64/kernel.bin
B := $(TOP)/build/power
ARGS := kolibri.powertest=1 kolibri.powerline=10
CFLAGS := -O1 -g -Wall -Wextra -Wno-unused-parameter -Wno-sign-compare -fsanitize=address,undefined \
	-fno-omit-frame-pointer
KINC := -Wno-builtin-declaration-mismatch -I$(TOP)/kernel/include -I$(TOP)/kernel/arch/arm64
PRO1X_DTB ?=

test: host qemu

host: $(B)/host $(B)/pmic $(B)/sm6115-power.dtb $(B)/qemu-psci.dtb
	$(B)/host $(B)/sm6115-power.dtb $(B)/qemu-psci.dtb $(PRO1X_DTB)
	$(B)/pmic

$(B)/pmic: pmic.c $(TOP)/kernel/modem/pmic.c $(TOP)/kernel/modem/pmic.h $(TOP)/kernel/modem/modem.h
	@mkdir -p $(B)
	$(CC) $(CFLAGS) -DMODEM_HOST pmic.c $(TOP)/kernel/modem/pmic.c -o $@

$(B)/host: host.c $(TOP)/kernel/arch/arm64/powerdt.c $(TOP)/kernel/arch/arm64/powerdt.h $(TOP)/kernel/fdt.c
	@mkdir -p $(B)
	$(CC) $(CFLAGS) -DPOWER_HOST -c host.c -o $(B)/host.o
	$(CC) $(CFLAGS) $(KINC) -c $(TOP)/kernel/fdt.c -o $(B)/fdt.o
	$(CC) $(CFLAGS) $(KINC) -c $(TOP)/kernel/arch/arm64/powerdt.c -o $(B)/powerdt.o
	$(CC) $(CFLAGS) $(B)/host.o $(B)/fdt.o $(B)/powerdt.o -o $@

$(B)/sm6115-power.dtb: sm6115-power.dts
	@mkdir -p $(B)
	dtc -q -I dts -O dtb -o $@ $<

qemu: kernel $(B)/pro1x.dtb $(B)/pro1x-psci.dtb $(B)/pro1x-sim.dtb $(B)/pro1x-cluster.dtb
	python3 qemu.py $(K) periodic 23 min-rate=90 -- -append "$(ARGS) kolibri.tick=periodic"
	python3 qemu.py $(K) tickless 23 max-rate=8 has="tickless idle" -- -append "$(ARGS)"
	python3 qemu.py --pro1x $(B)/pro1x.dtb $(K) pro1x 23 max-rate=8 has="pro1x: lifetime 90 s"
	python3 qemu.py --pro1x $(B)/pro1x-psci.dtb $(K) psci 23 max-rate=8 core
	python3 qemu.py --pro1x $(B)/pro1x-sim.dtb $(K) simulate 23 max-rate=8 core resumed
	python3 qemu.py --pro1x $(B)/pro1x-cluster.dtb $(K) cluster 23 max-rate=8 cluster resumed
	python3 qemu.py $(K) standby 23 max-rate=2.5 has="whole seconds" -- -append "$(ARGS) kolibri.standby"

kernel:
	$(MAKE) -C $(TOP) PLATFORM=arm64

$(B)/pro1x.dtb: mkdtb.py
	@mkdir -p $(B)
	python3 mkdtb.py $@ --bootargs "$(ARGS) kolibri.fakemodem"
$(B)/pro1x-psci.dtb: mkdtb.py
	python3 mkdtb.py $@ --psci --bootargs "$(ARGS) kolibri.fakemodem"
$(B)/pro1x-sim.dtb: mkdtb.py
	python3 mkdtb.py $@ --psci --bootargs "$(ARGS) kolibri.fakemodem kolibri.psci=simulate"
$(B)/pro1x-cluster.dtb: mkdtb.py
	python3 mkdtb.py $@ --psci --bootargs "$(ARGS) kolibri.fakemodem kolibri.psci=simulate,cluster"
$(B)/qemu-psci.dtb: mkdtb.py
	@mkdir -p $(B)
	python3 mkdtb.py $@ --psci --virt

.PHONY: test host qemu kernel

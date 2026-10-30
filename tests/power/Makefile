# The AArch64 kernel's power management: make -C tests/power
#
#   host   powerdt.c on the PC against the SM6115's tree (sm6115-power.dts,
#          and the phone's own with PRO1X_DTB=...) and QEMU's; pmic.c (the
#          SPMI arbiter, the battery) against an arbiter in RAM; dsi_ulps.c
#          (the DSI lanes to ULPS) against a model of the lanes
#   qemu   the kernel in QEMU (builds it first); each run prints the
#          kernel's power lines and checks them (qemu.py): the 100 Hz tick
#          as it was, tickless idle on QEMU's virt and on the Pro1X's code
#          paths (a tree of the Pro1X's kind, mkdtb.py), CPU_SUSPEND into
#          the core's state and the cluster's, and the power-down played
#          (kolibri.psci=simulate: the way back through cpu_resume); the
#          standby ended by a played press of the power key (kolibri.keytest),
#          with the DSI lanes' ULPS asked for (kolibri.dsiulps)
#   qemu-net  the wake-ups with programs and the network: virtio-net and
#          DHCP (lwIP's timers on demand, kernel/net/timers.c, and lwIP's own
#          with kolibri.nettimers=periodic), an idle TCP connection to the
#          host held by a Linux program (tests/linux/tcphold, without and
#          with a 15 s keepalive), the messenger on the fake backend
#          (tests/messenger/fakepeer); each with the display on and in the
#          standby (kolibri.standby)
TOP := ../..
K := $(TOP)/build/arm64/kernel.bin
B := $(TOP)/build/power
ARGS := kolibri.powertest=1 kolibri.powerline=10
# the most wake-ups a second in an idle interval after the start: the
# shell waits for events (EV_SYSTEM), lwIP's timers on demand; what is
# left is the fake modem's, the power test's own line, a live message of
# the fake messenger backend every 10 s, a keepalive every 15 s
IDLE ?= 1.2
CFLAGS := -O1 -g -Wall -Wextra -Wno-unused-parameter -Wno-sign-compare -fsanitize=address,undefined \
	-fno-omit-frame-pointer
KINC := -Wno-builtin-declaration-mismatch -I$(TOP)/kernel/include -I$(TOP)/kernel/arch/arm64
PRO1X_DTB ?=

test: host qemu qemu-net

host: $(B)/host $(B)/pmic $(B)/dsi $(B)/sm6115-power.dtb $(B)/qemu-psci.dtb
	$(B)/host $(B)/sm6115-power.dtb $(B)/qemu-psci.dtb $(PRO1X_DTB)
	$(B)/pmic
	$(B)/dsi

$(B)/dsi: dsi.c $(TOP)/kernel/platform/dsi_ulps.c $(TOP)/kernel/platform/dsi_ulps.h
	@mkdir -p $(B)
	$(CC) $(CFLAGS) -DDSI_HOST -I$(TOP)/kernel/platform dsi.c $(TOP)/kernel/platform/dsi_ulps.c -o $@

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

qemu: kernel $(B)/pro1x.dtb $(B)/pro1x-psci.dtb $(B)/pro1x-sim.dtb $(B)/pro1x-cluster.dtb $(B)/pro1x-key.dtb \
		$(B)/pro1x-ulps.dtb
	python3 qemu.py $(K) periodic 23 min-rate=90 -- -append "$(ARGS) kolibri.tick=periodic"
	python3 qemu.py $(K) tickless 33 max-rate=8 skip-first max-rate=$(IDLE) has="tickless idle" -- -append "$(ARGS)"
	python3 qemu.py --pro1x $(B)/pro1x.dtb $(K) pro1x 33 max-rate=8 skip-first max-rate=$(IDLE) has="pro1x: lifetime 90 s"
	python3 qemu.py --pro1x $(B)/pro1x-psci.dtb $(K) psci 23 max-rate=8 core
	python3 qemu.py --pro1x $(B)/pro1x-sim.dtb $(K) simulate 23 max-rate=8 core resumed
	python3 qemu.py --pro1x $(B)/pro1x-cluster.dtb $(K) cluster 23 max-rate=8 cluster resumed
	python3 qemu.py $(K) standby 33 skip-first max-rate=$(IDLE) has="whole seconds" -- -append "$(ARGS) kolibri.standby"
	python3 qemu.py --pro1x $(B)/pro1x-key.dtb $(K) keytest 33 max-rate=8 "has=pro1x: standby (the power test)" \
		"has=pro1x: awake (the power key, played) after 10 s"
	python3 qemu.py --pro1x $(B)/pro1x-ulps.dtb $(K) ulps 33 max-rate=8 "has=pro1x: DSI lanes to ULPS (lanes 1f, LANE_CTRL 0): done" \
		"has=pro1x: awake (the power key, played) after 10 s" "has=pro1x: DSI lanes out of ULPS: a lane stuck" \
		"has=pro1x: display on: sleep out" -- $(ULPS_REGS)

# the kernel with the Linux programs (tcphold, the fake messenger backend)
# on its ramdisk, kept as $(KL)
KL := $(B)/kernel-linux.bin
FAKEPEER := $(TOP)/tests/messenger/fakepeer/fakepeer
LINUX_FILES := bin/tcphold=build/power/tcphold bin/yappari-wa=tests/messenger/fakepeer/fakepeer
$(B)/tcphold: $(TOP)/tests/linux/tcphold/tcphold.c
	@mkdir -p $(B)
	aarch64-linux-gnu-gcc -static -O2 -Wall -s $< -o $@
$(FAKEPEER): $(TOP)/tests/messenger/fakepeer/main.go
	$(MAKE) -C $(TOP) tests/messenger/fakepeer/fakepeer
$(KL): $(B)/tcphold $(FAKEPEER) FORCE
	$(MAKE) -C $(TOP) PLATFORM=arm64 LINUX_FILES="$(LINUX_FILES)"
	cp $(K) $@
FORCE:

qemu-net: $(KL)
	python3 qemu.py --net $(KL) net 33 skip-first max-rate=$(IDLE) has="net: 10.0.2.15" -- -append "$(ARGS)"
	python3 qemu.py --net $(KL) net-periodic 33 skip-first min-rate=2 has="net: 10.0.2.15" \
		-- -append "$(ARGS) kolibri.nettimers=periodic"
	python3 qemu.py --net --hold 18766 $(KL) tcp 43 skip-first max-rate=$(IDLE) hold \
		-- -append "$(ARGS) kolibri.run=/bin/tcphold"
	python3 qemu.py --net --hold 18767 $(KL) tcp-standby 43 skip-first max-rate=$(IDLE) hold \
		-- -append "$(ARGS) kolibri.run=/bin/tcphold kolibri.args=18767 kolibri.standby"
	python3 qemu.py --net --hold 18768 $(KL) tcp-keepalive 43 skip-first max-rate=$(IDLE) hold \
		-- -append "$(ARGS) kolibri.run=/bin/tcphold kolibri.args=18768,15"
	python3 qemu.py $(KL) messenger 43 skip-first max-rate=$(IDLE) -- -append "$(ARGS) kolibri.run=/sys/messenger kolibri.args=whatsapp"
	python3 qemu.py $(KL) messenger-standby 43 skip-first max-rate=$(IDLE) \
		-- -append "$(ARGS) kolibri.run=/sys/messenger kolibri.args=whatsapp kolibri.standby"

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
$(B)/pro1x-key.dtb: mkdtb.py
	python3 mkdtb.py $@ --psci --bootargs "$(ARGS) kolibri.fakemodem kolibri.keytest=10"
$(B)/pro1x-ulps.dtb: mkdtb.py
	python3 mkdtb.py $@ --psci --display --bootargs "$(ARGS) kolibri.fakemodem kolibri.keytest=10 kolibri.dsiulps"
# the DSI controller's registers in RAM as the lanes would answer: a FIFO
# empty (FIFO_STATUS), every lane in its stop state and, once asked, in
# ULPS (LANE_STATUS 0x1f: ULPS_ACTIVE_NOT clear); RAM does not change, so
# the way out finds them still in ULPS ("stuck") and goes on
ULPS_REGS := -device loader,addr=0x5110000c,data=0x11111000,data-len=4 \
	-device loader,addr=0x511000a8,data=0x1f,data-len=4
$(B)/qemu-psci.dtb: mkdtb.py
	@mkdir -p $(B)
	python3 mkdtb.py $@ --psci --virt

.PHONY: test host qemu qemu-net kernel

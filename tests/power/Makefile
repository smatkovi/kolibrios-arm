# The AArch64 kernel's power management, in QEMU: make -C tests/power
# (builds the kernel first). Each run prints the kernel's power lines and
# checks them (qemu.py): the 100 Hz tick as it was, tickless idle on
# QEMU's virt and on the Pro1X's code paths (a device tree of the Pro1X's
# kind, mkdtb.py).
TOP := ../..
K := $(TOP)/build/arm64/kernel.bin
B := $(TOP)/build/power
ARGS := kolibri.powertest=1 kolibri.powerline=10

test: kernel $(B)/pro1x.dtb
	python3 qemu.py $(K) periodic 23 min-rate=90 -- -append "$(ARGS) kolibri.tick=periodic"
	python3 qemu.py $(K) tickless 23 max-rate=8 has="tickless idle" -- -append "$(ARGS)"
	python3 qemu.py --pro1x $(B)/pro1x.dtb $(K) pro1x 23 max-rate=8 has="pro1x: lifetime 90 s"

kernel:
	$(MAKE) -C $(TOP) PLATFORM=arm64

$(B)/pro1x.dtb: mkdtb.py
	@mkdir -p $(B)
	python3 mkdtb.py $@ --bootargs "$(ARGS) kolibri.fakemodem"

.PHONY: test kernel

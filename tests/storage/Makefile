# The storage (kernel/storage, docs/storage.md) on the PC and in QEMU:
#   make -C tests/storage            everything (run.py)
#   make -C tests/storage host ufs mmc   the block layer, the container's checks
#                                    and the fence, the UFS driver and the eMMC's
#                                    sleep against models
#   make -C tests/storage qemu       virtio-blk, QEMU's UFS and eMMC, realview, kfar and
#                                    t_edit, tools/kolibri-home itself (with sudo)
#   make -C tests/storage ext4-test fat-test   the file systems' own tests
S := ../../kernel/storage
CFLAGS := -DSTORAGE_HOST -O1 -g -Wall -Wextra -Wno-unused-parameter -Wno-sign-compare \
	-fsanitize=address,undefined -fno-omit-frame-pointer
CORE := $(S)/blk.c $(S)/ext4.c $(S)/fat.c $(S)/home.c

all:
	python3 run.py
host: bins
	python3 run.py host
ufs: bins
	python3 run.py ufs
mmc: bins
	python3 run.py mmc
qemu:
	python3 run.py virtio ufs-qemu emmc realview tamper apps messenger helper

bins: host-bin ufs_sim mmc_sim
host-bin: host.c $(CORE) $(S)/*.h
	$(CC) $(CFLAGS) host.c $(CORE) -o host
ufs_sim: ufs_sim.c $(CORE) $(S)/ufs.c $(S)/*.h
	$(CC) $(CFLAGS) ufs_sim.c $(CORE) $(S)/ufs.c -o $@
mmc_sim: mmc_sim.c $(CORE) $(S)/mmc.c $(S)/*.h
	$(CC) $(CFLAGS) mmc_sim.c $(CORE) $(S)/mmc.c -o $@

-include ext4.mk fat.mk

clean:
	rm -f host ufs_sim mmc_sim

.PHONY: all host ufs mmc qemu bins host-bin clean

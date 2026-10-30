# The FAT driver (kernel/storage/fat.c) on the PC: fat_test.c (a shell over an
# image) driven by fat_test.py, from the repository's root
#   make -f tests/storage/fat.mk fat-test        (fat-test-quick: fewer images,
#                                                operations and resets)
# or included by tests/storage/Makefile. Also compiles fat.c for both kernels
# (fat-cross, warnings are errors). Its images are made in /tmp/kolibri-fat-test
# (sparse, removed at the end); needs dosfstools (mkfs.fat, fsck.fat), mtools
# and python3, no root.
FAT_DIR := $(dir $(lastword $(MAKEFILE_LIST)))
FAT_ROOT := $(FAT_DIR)../..
FAT_KS := $(FAT_ROOT)/kernel/storage
FAT_CFLAGS := -DSTORAGE_HOST -O1 -g -std=gnu11 -fsanitize=address,undefined -fno-sanitize-recover=all \
	-fno-omit-frame-pointer -Wall -Wextra -Wno-unused-parameter -Wno-sign-compare -I $(FAT_KS)
FAT_KFLAGS := -O2 -std=gnu11 -ffreestanding -fno-builtin -fno-common -Wall -Wextra -Wno-unused-parameter \
	-Wno-sign-compare -Werror -I $(FAT_ROOT)/kernel/include
FAT_RUN := LC_ALL=C.UTF-8 MTOOLS_SKIP_CHECK=1 python3 -I $(FAT_DIR)fat_test.py $(FAT_DIR)fat_test

$(FAT_DIR)fat_test: $(FAT_DIR)fat_test.c $(FAT_KS)/fat.c $(FAT_KS)/storage.h
	$(CC) $(FAT_CFLAGS) $(FAT_DIR)fat_test.c $(FAT_KS)/fat.c -o $@

fat-test: $(FAT_DIR)fat_test fat-cross
	$(FAT_RUN)

fat-test-quick: $(FAT_DIR)fat_test
	$(FAT_RUN) quick

fat-cross:
	clang --target=aarch64-none-elf -mcpu=cortex-a53 -fpie -mgeneral-regs-only -mstrict-align $(FAT_KFLAGS) \
		-I $(FAT_ROOT)/kernel/arch/arm64 -c $(FAT_KS)/fat.c -o /tmp/kolibri-fat-a64.o
	arm-none-eabi-gcc -march=armv7-a -marm -mfloat-abi=soft -fno-pic $(FAT_KFLAGS) \
		-I $(FAT_ROOT)/kernel/arch/armv7 -c $(FAT_KS)/fat.c -o /tmp/kolibri-fat-a32.o
	rm -f /tmp/kolibri-fat-a64.o /tmp/kolibri-fat-a32.o

fat-clean:
	rm -f $(FAT_DIR)fat_test

.PHONY: fat-test fat-test-quick fat-cross fat-clean

# The ext4 driver (kernel/storage/ext4.c) on the PC, ext4_test.c: from the
# repository's root
#   make -f tests/storage/ext4.mk ext4-test        (ext4-test-quick: no fuzzing,
#                                                  no 300 GB and 17 TiB volumes)
# or included by tests/storage/Makefile. Its images are made in
# /tmp/kolibri-ext4-test (removed at the end); needs e2fsprogs (mkfs.ext2/3/4,
# debugfs, dumpe2fs), no root.
EXT4_DIR := $(dir $(lastword $(MAKEFILE_LIST)))
EXT4_KS := $(EXT4_DIR)../../kernel/storage
EXT4_CFLAGS := -DSTORAGE_HOST -O1 -g -std=gnu11 -fsanitize=address,undefined -fno-sanitize-recover=all \
	-fno-omit-frame-pointer -Wall -Wextra -Wno-unused-parameter -Wno-sign-compare -I $(EXT4_KS)

$(EXT4_DIR)ext4_test: $(EXT4_DIR)ext4_test.c $(EXT4_KS)/ext4.c $(EXT4_KS)/storage.h
	$(CC) $(EXT4_CFLAGS) $(EXT4_DIR)ext4_test.c $(EXT4_KS)/ext4.c -o $@

ext4-test: $(EXT4_DIR)ext4_test
	$(EXT4_DIR)ext4_test

ext4-test-quick: $(EXT4_DIR)ext4_test
	$(EXT4_DIR)ext4_test quick

ext4-clean:
	rm -f $(EXT4_DIR)ext4_test

.PHONY: ext4-test ext4-test-quick ext4-clean

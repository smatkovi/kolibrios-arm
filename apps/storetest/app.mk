# storetest: the storage through function 70 (tests/storage): the disks'
# volumes read only, the container written and kept; "storetest:" lines
CAPPS += storetest
SRC_storetest := apps/storetest/storetest.c
MEM_storetest := 0x400000
# its scripts (tests/storage/scripts): /sys/tests/st-NAME
RD_storetest := $(foreach s,$(notdir $(wildcard tests/storage/scripts/*)),tests/st-$(s)=tests/storage/scripts/$(s))

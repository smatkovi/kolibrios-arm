# kfar_arc (lib/kfar_arc) on the PC: host.c drives it through kfar's
# plugin interface and run.py compares what it unpacks with Python's and
# (when there) 7-Zip's; built 32-bit as on ARM (gcc -m32). The names the
# plugin imports from kfar under the C library's (read, seek, close,
# filesize, open2) are renamed in its objects.
TOP := ../..
OUT ?= /tmp/kfar_arc-test
CC := gcc -m32
CFLAGS := -O1 -g -std=gnu11 -Wall -Wno-unused-parameter -I$(TOP)/apps/kfar -I$(TOP)/lib/kfar_arc
SRC := $(wildcard $(TOP)/lib/kfar_arc/*.c)
OBJ := $(patsubst $(TOP)/lib/kfar_arc/%.c,$(OUT)/%.o,$(SRC))
RENAME := $(foreach s,read seek close filesize open2,--redefine-sym $(s)=kfar_$(s))

all: $(OUT)/host
	python3 run.py $(OUT)
$(OUT)/%.o: $(TOP)/lib/kfar_arc/%.c $(TOP)/lib/kfar_arc/kfar_arc.h $(TOP)/apps/kfar/kfar_api.h
	@mkdir -p $(OUT)
	$(CC) $(CFLAGS) -c $< -o $@
	objcopy $(RENAME) $@
$(OUT)/host: host.c $(OBJ)
	$(CC) $(CFLAGS) $^ -o $@
.PHONY: all

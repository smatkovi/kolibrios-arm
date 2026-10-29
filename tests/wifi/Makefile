# The Pro1X's Wi-Fi above WLFW on the PC: make -C tests/wifi
# (the copy engines, HTC, WMI-TLV against a simulated WCN3990)
M := ../../kernel/modem
SRC := $(M)/ce.c $(M)/htc.c $(M)/wmi.c
CFLAGS := -DMODEM_HOST -O1 -g -Wall -Wextra -Wno-unused-parameter -Wno-sign-compare \
	-fsanitize=address,undefined -fno-omit-frame-pointer

test: test.c $(SRC) $(M)/ce.h $(M)/htc.h $(M)/wmi.h $(M)/modem.h
	$(CC) $(CFLAGS) test.c $(SRC) -o $@
	./test

clean:
	rm -f test

.PHONY: clean

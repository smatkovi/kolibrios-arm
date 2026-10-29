# The modem's code (kernel/modem) on the PC: make -C tests/modem
M := ../../kernel/modem
SRC := $(M)/smem.c $(M)/glink.c $(M)/qrtr.c $(M)/qmi.c $(M)/rmtfs.c $(M)/sms.c $(M)/phone.c $(M)/fake.c $(M)/wlfw.c $(M)/rpm.c
CFLAGS := -DMODEM_HOST -O1 -g -Wall -Wextra -Wno-unused-parameter -Wno-sign-compare \
	-fsanitize=address,undefined -fno-omit-frame-pointer

test: test.c $(SRC) $(M)/*.h ../../lib/kos_modem.h
	$(CC) $(CFLAGS) test.c $(SRC) -o $@
	./test | grep -v "^modem:\|^rmtfs:\|^glink:\|^fake:"
	./test preopened | grep -v "^modem:\|^rmtfs:\|^fake:"

clean:
	rm -f test

.PHONY: clean

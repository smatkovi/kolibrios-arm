# The call's sound (kernel/modem/apr.c, callaudio.c) on the PC: make -C tests/audio
M := ../../kernel/modem
SRC := $(M)/apr.c $(M)/callaudio.c $(M)/glink.c $(M)/smem.c
CFLAGS := -DMODEM_HOST -O1 -g -Wall -Wextra -Wno-unused-parameter -Wno-sign-compare \
	-fsanitize=address,undefined -fno-omit-frame-pointer

test: test.c $(SRC) $(M)/apr.h $(M)/callaudio.h $(M)/glink.h $(M)/modem.h
	$(CC) $(CFLAGS) test.c $(SRC) -o $@
	./test | grep -v "^callaudio:\|^glink:"

clean:
	rm -f test

.PHONY: clean

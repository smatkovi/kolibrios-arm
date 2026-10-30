# The random number generator on the PC: make -C tests/rng
#  test:     ChaCha20's RFC 8439 vectors, the generator's rules, FIPS
#            140-2's tests, the Qualcomm PRNG's driver against a model of
#            its registers and clock (test.c)
#  model.py: the generator, FIPS 140-2's statistics and the most common
#            value estimate against a reference in Python, ChaCha20
#            against OpenSSL's
#  qemu:     the kernels in QEMU (qemu.py): virtio-rng and kexec's seed,
#            no source (jitter, the warning), no seed at all (getrandom
#            waits), ARMv7's jitter, the Linux personality's getrandom and
#            /dev/urandom (tests/linux/rngtest); make qemu
K := ../../kernel
CFLAGS := -DMODEM_HOST -DQPRNG_HOST -O1 -g -Wall -Wextra -Wno-unused-parameter -Wno-sign-compare \
	-fsanitize=address,undefined -fno-omit-frame-pointer -I$(K) -I$(K)/modem
SRC := test.c $(K)/rng_core.c $(K)/modem/crypto.c $(K)/platform/qcom_prng.c

all: test
	./test
	python3 model.py ./test

test: $(SRC) $(K)/rng_core.h $(K)/platform/qcom_prng.h $(K)/modem/crypto.h
	$(CC) $(CFLAGS) $(SRC) -o $@

qemu:
	python3 qemu.py

clean:
	rm -f test

.PHONY: all qemu clean

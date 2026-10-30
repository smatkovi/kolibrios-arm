# The messenger's parts on the PC: make -C tests/messenger
#   the JSON reader (lib/json.h), the fake backend's protocol, and the real
#   backends' when given: PURPLE_HOST=... (with its test protocol and logged
#   out), SIGNAL_HOST=..., YAPPARI_WA=... (logged out, no network)
# The chats in QEMU: make test-chats at the top.
CFLAGS := -O1 -g -Wall -Wextra -fsanitize=address,undefined -fno-omit-frame-pointer
PURPLE_HOST ?=
SIGNAL_HOST ?=
YAPPARI_WA ?=

test: json_test fakepeer/fakepeer
	./json_test
	python3 hosttest.py fakepeer-whatsapp fakepeer/fakepeer
	python3 hosttest.py fakepeer-telegram fakepeer/fakepeer
	python3 hosttest.py fakepeer-signal fakepeer/fakepeer
	$(if $(PURPLE_HOST),python3 hosttest.py purple-test $(PURPLE_HOST))
	$(if $(PURPLE_HOST),python3 hosttest.py purple $(PURPLE_HOST))
	$(if $(SIGNAL_HOST),python3 hosttest.py signal $(SIGNAL_HOST))
	$(if $(YAPPARI_WA),python3 hosttest.py yappari $(YAPPARI_WA))

json_test: json_test.c ../../lib/json.h
	$(CC) $(CFLAGS) json_test.c -o $@

fakepeer/fakepeer: fakepeer/main.go
	cd fakepeer && CGO_ENABLED=0 GOOS=linux GOARCH=arm64 go build -trimpath -ldflags=-s -o fakepeer .

clean:
	rm -f json_test fakepeer/fakepeer

.PHONY: test clean

# The Pro1X's Wi-Fi above WLFW on the PC: make -C tests/wifi
#  test:    the copy engines, HTC and WMI-TLV, the first steps (test.c)
#  crypto:  SHA-1, HMAC, PBKDF2, the PRF, AES, the key wrap (crypto.c)
#  station: the whole driver against a simulated WCN3990 and access point,
#           with lwIP above it (station.c): stages 4-6, three scenarios
# Needs OpenSSL's libcrypto (the AP's side, the reference values)
M := ../../kernel/modem
LWIP := ../../third_party/lwip/src
CFLAGS := -DMODEM_HOST -O1 -g -Wall -Wextra -Wno-unused-parameter -Wno-sign-compare \
	-fsanitize=address,undefined -fno-omit-frame-pointer
WIFI := $(M)/wifi.c $(M)/ce.c $(M)/htc.c $(M)/wmi.c $(M)/htt.c $(M)/wlan.c $(M)/wpa.c $(M)/crypto.c
HDRS := $(wildcard $(M)/*.h)
LWIPSRC := $(addprefix $(LWIP)/,core/init.c core/def.c core/dns.c core/inet_chksum.c core/ip.c core/mem.c \
	core/memp.c core/netif.c core/pbuf.c core/tcp.c core/tcp_in.c core/tcp_out.c core/timeouts.c core/udp.c \
	core/ipv4/dhcp.c core/ipv4/etharp.c core/ipv4/icmp.c core/ipv4/ip4.c core/ipv4/ip4_addr.c \
	core/ipv4/ip4_frag.c core/ipv4/acd.c netif/ethernet.c)

all: test crypto station
	./test
	./crypto
	./station
	./station wrongpsk
	./station noap

test: test.c $(M)/ce.c $(M)/htc.c $(M)/wmi.c $(HDRS)
	$(CC) $(CFLAGS) test.c $(M)/ce.c $(M)/htc.c $(M)/wmi.c -o $@

crypto: crypto.c crypto_main.c $(M)/crypto.c $(M)/wpa.c $(HDRS)
	$(CC) $(CFLAGS) -Wno-deprecated-declarations crypto.c crypto_main.c $(M)/crypto.c $(M)/wpa.c -lcrypto -o $@

$(LWIP)/core/init.c:
	cd ../.. && tools/fetch-lwip.sh

station: station.c $(WIFI) $(HDRS) $(LWIP)/core/init.c lwip/lwipopts.h lwip/arch/cc.h
	$(CC) $(CFLAGS) -Wno-deprecated-declarations -Ilwip -I$(LWIP)/include station.c $(WIFI) \
		$(LWIPSRC) -lcrypto -o $@

clean:
	rm -f test crypto station

.PHONY: all clean

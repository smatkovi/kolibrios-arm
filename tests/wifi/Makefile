# The Pro1X's Wi-Fi above WLFW on the PC: make -C tests/wifi
#  test:    the copy engines, HTC and WMI-TLV, the first steps (test.c)
#  crypto:  SHA-1, HMAC, PBKDF2, the PRF, AES, the key wrap (crypto.c)
#  regd:    the regulatory domain: ath's EEPROM codes, the channels (regd.c)
#  station: the whole driver against a simulated WCN3990 and access point,
#           with lwIP above it (station.c): stages 4-6, HT/WMM, power save;
#           scenarios normal (HT40, WMM), legacy, uapsd, wrongpsk, noap
#  qemu:    the kernel's side in QEMU (qemu.py): the Pro1X's tree with a
#           Wi-Fi node of RAM, the fake modem's WLFW, the network from the
#           initrd: WLAN_CFG, WLAN_MODE, the engines, waiting for READY
# Needs OpenSSL's libcrypto (the AP's side, the reference values)
M := ../../kernel/modem
LWIP := ../../third_party/lwip/src
CFLAGS := -DMODEM_HOST -O1 -g -Wall -Wextra -Wno-unused-parameter -Wno-sign-compare \
	-fsanitize=address,undefined -fno-omit-frame-pointer
WIFI := $(M)/wifi.c $(M)/ce.c $(M)/htc.c $(M)/wmi.c $(M)/htt.c $(M)/wlan.c $(M)/wpa.c $(M)/crypto.c $(M)/regd.c $(M)/regdb.c $(M)/ecc.c $(M)/sae.c
HDRS := $(wildcard $(M)/*.h)
LWIPSRC := $(addprefix $(LWIP)/,core/init.c core/def.c core/dns.c core/inet_chksum.c core/ip.c core/mem.c \
	core/memp.c core/netif.c core/pbuf.c core/tcp.c core/tcp_in.c core/tcp_out.c core/timeouts.c core/udp.c \
	core/ipv4/dhcp.c core/ipv4/etharp.c core/ipv4/icmp.c core/ipv4/ip4.c core/ipv4/ip4_addr.c \
	core/ipv4/ip4_frag.c core/ipv4/acd.c netif/ethernet.c)

all: test crypto regd station
	./test
	./crypto
	./regd
	./station
	./station legacy
	./station uapsd
	./station wrongpsk
	./station noap

test: test.c $(M)/ce.c $(M)/htc.c $(M)/wmi.c $(HDRS)
	$(CC) $(CFLAGS) test.c $(M)/ce.c $(M)/htc.c $(M)/wmi.c -o $@

crypto: crypto.c crypto_main.c $(M)/crypto.c $(M)/wpa.c $(M)/ecc.c $(M)/sae.c $(HDRS)
	$(CC) $(CFLAGS) -Wno-deprecated-declarations crypto.c crypto_main.c $(M)/crypto.c $(M)/wpa.c $(M)/ecc.c $(M)/sae.c -lcrypto -o $@

regd: regd.c $(M)/regd.c $(M)/regdb.c $(HDRS)
	$(CC) $(CFLAGS) regd.c $(M)/regd.c $(M)/regdb.c -o $@

$(LWIP)/core/init.c:
	cd ../.. && tools/fetch-lwip.sh

station: station.c $(WIFI) $(HDRS) $(LWIP)/core/init.c lwip/lwipopts.h lwip/arch/cc.h
	$(CC) $(CFLAGS) -Wno-deprecated-declarations -Ilwip -I$(LWIP)/include station.c $(WIFI) \
		$(LWIPSRC) -lcrypto -o $@

B := ../../build/tests-wifi
qemu:
	$(MAKE) -C ../.. PLATFORM=arm64
	@mkdir -p $(B)
	python3 ../power/mkdtb.py $(B)/pro1x-wifi.dtb --wifi --bootargs "kolibri.fakemodem kolibri.wifi kolibri.lifetime=60"
	printf 'ssid=KolibriNet\npsk=correct horse battery\n' > $(B)/wifi.conf
	python3 ../../tools/mkefs.py $(B)/initrd.img wifi=$(B)/wifi.conf
	python3 qemu.py ../../build/arm64/kernel.bin $(B)/pro1x-wifi.dtb $(B)/initrd.img 10 \
		"pro1x: initrd at" "wlfw: registered, the firmware is ready" "pro1x: Wi-Fi at" "wifi: DMA" \
		"wlfw: WLAN_CFG taken" "wlfw: WLAN_MODE mission" "wifi: copy engines up, waiting for HTC READY" \
		"wifi: no answer in time (in HTC ready): stopped"

clean:
	rm -f test crypto regd station

.PHONY: all clean qemu

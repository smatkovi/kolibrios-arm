# lwIP's timers on demand (kernel/net/timers.c) on the PC: make -C tests/net
#   net-kos   the kernel's options and timers
#   net-lwip  the same with lwIP's own timers (timeouts.c), the reference
# run.py runs both through the scenarios of net.c and compares them: the
# same frames at the same times (TCP's within its timer's 250 ms), the
# same outcomes, and the wake-ups of the idle phases
TOP := ../..
LWIP := $(TOP)/third_party/lwip/src
CFLAGS := -O1 -g -Wall -Wextra -Wno-unused-parameter -Wno-sign-compare -Wno-unused-but-set-variable \
	-fsanitize=address,undefined -fno-omit-frame-pointer -I. -I$(TOP)/kernel/net -I$(LWIP)/include
LWIPSRC := $(addprefix $(LWIP)/,core/init.c core/def.c core/dns.c core/inet_chksum.c core/ip.c core/mem.c \
	core/memp.c core/netif.c core/pbuf.c core/tcp.c core/tcp_in.c core/tcp_out.c core/timeouts.c core/udp.c \
	core/ipv4/dhcp.c core/ipv4/etharp.c core/ipv4/icmp.c core/ipv4/ip4.c core/ipv4/ip4_addr.c \
	core/ipv4/ip4_frag.c core/ipv4/acd.c netif/ethernet.c)
DEPS := net.c lwipopts.h arch/cc.h $(TOP)/kernel/net/lwipopts.h $(TOP)/kernel/net/timers.c $(TOP)/kernel/net/nettimers.h

test: net-kos net-lwip
	python3 run.py

net-kos: $(DEPS) $(LWIP)/core/init.c
	$(CC) $(CFLAGS) -DLWIP_TIMERS_CUSTOM=1 net.c $(TOP)/kernel/net/timers.c $(LWIPSRC) -o $@
net-lwip: $(DEPS) $(LWIP)/core/init.c
	$(CC) $(CFLAGS) -DLWIP_TIMERS_CUSTOM=0 net.c $(LWIPSRC) -o $@

$(LWIP)/core/init.c:
	cd $(TOP) && tools/fetch-lwip.sh

clean:
	rm -f net-kos net-lwip

.PHONY: test clean

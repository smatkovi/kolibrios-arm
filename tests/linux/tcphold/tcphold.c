/* tcphold: a Linux program for the AArch64 kernel's Linux personality
 * that holds one TCP connection open and idle, as a messenger's backend
 * holds its server's (tests/power: the wake-ups with an idle connection).
 *
 *   tcphold [PORT [KEEPIDLE]]
 *
 * Connects to QEMU's host (10.0.2.2, user networking) on PORT (18766),
 * sends "hello\n", then reads until the connection ends; with KEEPIDLE
 * (seconds) TCP keepalive on, probes every KEEPIDLE seconds. Built
 * static for linux/arm64 (tests/power/Makefile). */
#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/socket.h>
#include <unistd.h>

int main(int argc, char **argv)
{
	int port = argc > 1 ? atoi(argv[1]) : 18766;
	int idle = argc > 2 ? atoi(argv[2]) : 0;
	int s = socket(AF_INET, SOCK_STREAM, 0);
	if (s < 0) {
		perror("tcphold: socket");
		return 1;
	}
	if (idle > 0) {
		int on = 1, cnt = 3;
		setsockopt(s, SOL_SOCKET, SO_KEEPALIVE, &on, sizeof(on));
		setsockopt(s, IPPROTO_TCP, TCP_KEEPIDLE, &idle, sizeof(idle));
		setsockopt(s, IPPROTO_TCP, TCP_KEEPINTVL, &idle, sizeof(idle));
		setsockopt(s, IPPROTO_TCP, TCP_KEEPCNT, &cnt, sizeof(cnt));
	}
	struct sockaddr_in a = { .sin_family = AF_INET, .sin_port = htons(port) };
	inet_pton(AF_INET, "10.0.2.2", &a.sin_addr);
	for (int i = 0; connect(s, (struct sockaddr *)&a, sizeof(a)) < 0; i++) {
		if (i == 20) {
			perror("tcphold: connect");
			return 1;
		}
		sleep(1);
		close(s);
		s = socket(AF_INET, SOCK_STREAM, 0);
	}
	fprintf(stderr, "tcphold: connected to 10.0.2.2:%d%s\n", port, idle ? ", keepalive" : "");
	write(s, "hello\n", 6);
	char buf[256];
	ssize_t n;
	long total = 0;
	while ((n = read(s, buf, sizeof(buf))) > 0) total += n;
	fprintf(stderr, "tcphold: the connection ended after %ld bytes\n", total);
	return 0;
}

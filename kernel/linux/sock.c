/* KolibriOS for ARM: the Linux personality's sockets: IPv4 TCP and UDP on
 * lwIP's raw API. lwIP's callbacks (from the network interrupt, the tick
 * or a call into lwIP) fill the socket; a waiting thread sees it when the
 * scheduler tries its call again; epoll sees the socket's generation
 * count move. */
#include "sock.h"
#include "lwip/tcp.h"
#include "lwip/udp.h"
#include "lwip/ip_addr.h"

#define SOCK_STREAM 1
#define SOCK_DGRAM 2
#define SOCK_NONBLOCK 04000
#define AF_INET 2

#define EPOLLIN 1
#define EPOLLOUT 4
#define EPOLLERR 8
#define EPOLLHUP 0x10
#define EPOLLRDHUP 0x2000

enum { S_NEW, S_CONNECTING, S_CONNECTED, S_LISTEN, S_CLOSED };

#define DQ 16
#define ACQ 8

struct lx_sock {
	bool used;
	int type, state, err;
	u32 gen;
	struct tcp_pcb *tcp;
	struct udp_pcb *udp;
	struct pbuf *rx;                /* TCP: what came, not read yet */
	bool eof, shut_wr;
	struct { struct pbuf *p; ip_addr_t addr; u16 port; } dq[DQ];   /* UDP */
	int dq_head, dq_n;
	struct tcp_pcb *acq[ACQ];       /* listening: connections not accepted yet */
	int acq_n;
};

static struct lx_sock socks[128];
static u8 sbuf[65536];

static struct lx_sock *sock_new(int type)
{
	for (u32 i = 0; i < ARRAY_SIZE(socks); i++)
		if (!socks[i].used) {
			memset(&socks[i], 0, sizeof(socks[i]));
			socks[i].used = true;
			socks[i].type = type;
			return &socks[i];
		}
	return NULL;
}

static int lwip_errno(err_t e, bool connecting)
{
	switch (e) {
	case ERR_RST: return connecting ? ECONNREFUSED : ECONNRESET;
	case ERR_ABRT: return connecting ? ETIMEDOUT : ECONNABORTED;
	case ERR_TIMEOUT: return ETIMEDOUT;
	case ERR_RTE: return ENETUNREACH;
	case ERR_USE: return EADDRINUSE;
	case ERR_MEM: case ERR_BUF: return ENOBUFS;
	case ERR_ISCONN: return EISCONN;
	case ERR_CONN: return ENOTCONN;
	default: return ECONNRESET;
	}
}

/* TCP's callbacks */
static err_t t_recv(void *arg, struct tcp_pcb *pcb, struct pbuf *p, err_t err)
{
	struct lx_sock *s = arg;
	(void)pcb, (void)err;
	if (!s) {
		if (p) pbuf_free(p);
		return ERR_OK;
	}
	if (!p) s->eof = true;
	else if (s->rx) pbuf_cat(s->rx, p);
	else s->rx = p;
	s->gen++;
	return ERR_OK;
}

static err_t t_sent(void *arg, struct tcp_pcb *pcb, u16_t len)
{
	struct lx_sock *s = arg;
	(void)pcb, (void)len;
	if (s) s->gen++;
	return ERR_OK;
}

static void t_err(void *arg, err_t err)
{
	struct lx_sock *s = arg;
	if (!s) return;
	s->tcp = NULL;                          /* freed by lwIP */
	s->err = lwip_errno(err, s->state == S_CONNECTING);
	s->state = S_CLOSED;
	s->gen++;
}

static err_t t_connected(void *arg, struct tcp_pcb *pcb, err_t err)
{
	struct lx_sock *s = arg;
	(void)pcb;
	if (!s) return ERR_OK;
	s->state = err == ERR_OK ? S_CONNECTED : S_CLOSED;
	if (err != ERR_OK) s->err = lwip_errno(err, true);
	s->gen++;
	return ERR_OK;
}

static void tcp_hook(struct lx_sock *s, struct tcp_pcb *pcb)
{
	s->tcp = pcb;
	tcp_arg(pcb, s);
	tcp_recv(pcb, t_recv);
	tcp_sent(pcb, t_sent);
	tcp_err(pcb, t_err);
}

static err_t t_accept(void *arg, struct tcp_pcb *pcb, err_t err)
{
	struct lx_sock *s = arg;
	if (err != ERR_OK || !pcb) return ERR_VAL;
	if (!s || s->acq_n >= ACQ) {
		tcp_abort(pcb);
		return ERR_ABRT;
	}
	tcp_arg(pcb, NULL);
	tcp_recv(pcb, t_recv);                  /* data before accept(): dropped */
	s->acq[s->acq_n++] = pcb;
	s->gen++;
	return ERR_OK;
}

/* UDP's */
static void u_recv(void *arg, struct udp_pcb *pcb, struct pbuf *p, const ip_addr_t *addr, u16_t port)
{
	struct lx_sock *s = arg;
	(void)pcb;
	if (!s || s->dq_n >= DQ) {
		pbuf_free(p);
		return;
	}
	int i = (s->dq_head + s->dq_n++) % DQ;
	s->dq[i].p = p;
	ip_addr_copy(s->dq[i].addr, *addr);
	s->dq[i].port = port;
	s->gen++;
}

void sock_close(struct lx_sock *s)
{
	if (s->tcp) {
		struct tcp_pcb *pcb = s->tcp;
		tcp_arg(pcb, NULL);
		if (s->state == S_LISTEN) {
			tcp_close(pcb);
		} else {
			tcp_recv(pcb, NULL);
			tcp_sent(pcb, NULL);
			tcp_err(pcb, NULL);
			if (tcp_close(pcb) != ERR_OK) tcp_abort(pcb);
		}
	}
	for (int i = 0; i < s->acq_n; i++) tcp_abort(s->acq[i]);
	if (s->udp) udp_remove(s->udp);
	if (s->rx) pbuf_free(s->rx);
	for (int i = 0; i < s->dq_n; i++) pbuf_free(s->dq[(s->dq_head + i) % DQ].p);
	s->used = false;
}

long sock_read(struct lx_sock *s, void *buf, u64 len, bool nb)
{
	if (s->type == SOCK_DGRAM) {
		if (!s->dq_n) return nb ? -EAGAIN : LX_BLOCK;
		struct pbuf *p = s->dq[s->dq_head].p;
		u32 n = MIN(len, (u64)p->tot_len);
		pbuf_copy_partial(p, buf, n, 0);
		pbuf_free(p);
		s->dq_head = (s->dq_head + 1) % DQ;
		s->dq_n--;
		return n;
	}
	if (s->rx) {
		u32 n = MIN(len, (u64)s->rx->tot_len);
		pbuf_copy_partial(s->rx, buf, n, 0);
		s->rx = pbuf_free_header(s->rx, n);
		if (s->tcp) tcp_recved(s->tcp, n);
		s->gen++;
		return n;
	}
	if (s->eof) return 0;
	if (s->err) return -s->err;
	if (s->state == S_NEW || s->state == S_LISTEN) return -ENOTCONN;
	return nb ? -EAGAIN : LX_BLOCK;
}

long sock_write(struct lx_sock *s, const void *buf, u64 len, bool nb)
{
	if (s->type == SOCK_DGRAM) {
		if (!s->udp || len > 65507) return -EMSGSIZE;
		struct pbuf *p = pbuf_alloc(PBUF_TRANSPORT, len, PBUF_RAM);
		if (!p) return -ENOBUFS;
		pbuf_take(p, buf, len);
		err_t e = udp_send(s->udp, p);
		pbuf_free(p);
		return e == ERR_OK ? (long)len : e == ERR_RTE ? -ENETUNREACH : -EDESTADDRREQ;
	}
	if (s->err) {
		int e = s->err;
		return -e;
	}
	if (s->shut_wr) return -EPIPE;
	if (s->state == S_CONNECTING) return nb ? -EAGAIN : LX_BLOCK;
	if (s->state != S_CONNECTED || !s->tcp) return -ENOTCONN;
	u32 room = tcp_sndbuf(s->tcp);
	if (tcp_sndqueuelen(s->tcp) >= TCP_SND_QUEUELEN - 4) room = 0;
	if (!room) return nb ? -EAGAIN : LX_BLOCK;
	u32 n = MIN(len, (u64)room);
	if (tcp_write(s->tcp, buf, n, TCP_WRITE_FLAG_COPY) != ERR_OK) return nb ? -EAGAIN : LX_BLOCK;
	tcp_output(s->tcp);
	return n;
}

u32 sock_poll(struct lx_sock *s, u32 *gen)
{
	*gen = s->gen;
	if (s->type == SOCK_DGRAM) return (s->dq_n ? EPOLLIN : 0) | EPOLLOUT;
	if (s->state == S_LISTEN) return s->acq_n ? EPOLLIN : 0;
	u32 m = 0;
	if (s->rx || s->eof || s->err) m |= EPOLLIN;
	if (s->eof) m |= EPOLLRDHUP;
	if (s->err) m |= EPOLLERR | EPOLLHUP;
	if (s->state == S_CONNECTED && s->tcp && tcp_sndbuf(s->tcp) > 0) m |= EPOLLOUT;
	if (s->state == S_CLOSED) m |= EPOLLHUP | EPOLLOUT;
	return m;
}

/* struct sockaddr_in */
static int get_addr(struct lx_proc *p, u64 ua, u64 len, ip_addr_t *ip, u16 *port)
{
	u8 sa[16];
	if (len < 8) return -EINVAL;
	if (lx_copy_in(p, sa, ua, 8)) return -EFAULT;
	if (sa[0] != AF_INET) return -EAFNOSUPPORT;
	*port = sa[2] << 8 | sa[3];
	u32 a;
	memcpy(&a, sa + 4, 4);
	ip_addr_set_ip4_u32(ip, a);
	return 0;
}

static int put_addr(struct lx_proc *p, u64 ua, u64 ulen, const ip_addr_t *ip, u16 port)
{
	if (!ua) return 0;
	u32 len;
	if (lx_copy_in(p, &len, ulen, 4)) return -EFAULT;
	u8 sa[16] = { AF_INET, 0, port >> 8, port & 255 };
	u32 a = ip ? ip4_addr_get_u32(ip_2_ip4(ip)) : 0;
	memcpy(sa + 4, &a, 4);
	if (lx_copy_out(p, ua, sa, MIN(len, 16u))) return -EFAULT;
	len = 16;
	return lx_copy_out(p, ulen, &len, 4);
}

static long s_connect(struct task *t, struct lx_sock *s, u64 ua, u64 len, bool nb)
{
	struct lx_proc *p = t->lx->p;
	if (t->lx->retry) {
		if (s->state == S_CONNECTED) return 0;
		if (s->state == S_CONNECTING) return LX_BLOCK;
		int e = s->err;
		s->err = 0;
		return -(e ? e : ECONNREFUSED);
	}
	ip_addr_t ip;
	u16 port;
	int e = get_addr(p, ua, len, &ip, &port);
	if (e) return e;
	if (s->type == SOCK_DGRAM) {
		if (!s->udp) {
			s->udp = udp_new();
			if (!s->udp) return -ENOBUFS;
			udp_recv(s->udp, u_recv, s);
		}
		return udp_connect(s->udp, &ip, port) == ERR_OK ? 0 : -ENETUNREACH;
	}
	if (s->state == S_CONNECTING) return -EALREADY;
	if (s->state == S_CONNECTED) return -EISCONN;
	if (s->state != S_NEW || !s->tcp) return -EINVAL;
	err_t r = tcp_connect(s->tcp, &ip, port, t_connected);
	if (r != ERR_OK) return -lwip_errno(r, true);
	s->state = S_CONNECTING;
	return nb ? -EINPROGRESS : LX_BLOCK;
}

static long s_sockopt_get(struct lx_proc *p, struct lx_sock *s, int level, int opt, u64 uval, u64 ulen)
{
	s32 v = 0;
	if (level == 1 && opt == 4) {           /* SOL_SOCKET, SO_ERROR */
		v = s->err;
		if (s->state != S_CLOSED) s->err = 0;
	} else if (level == 1 && opt == 3) {    /* SO_TYPE */
		v = s->type;
	} else if (level == 1 && (opt == 7 || opt == 8)) {    /* SO_SNDBUF, SO_RCVBUF */
		v = 65536;
	} else if (level == 6 && opt == 1) {    /* TCP_NODELAY */
		v = s->tcp && tcp_nagle_disabled(s->tcp);
	}
	u32 len = 4;
	if (lx_copy_out(p, uval, &v, 4) || lx_copy_out(p, ulen, &len, 4)) return -EFAULT;
	return 0;
}

static long s_sockopt_set(struct lx_proc *p, struct lx_sock *s, int level, int opt, u64 uval)
{
	s32 v = 0;
	lx_copy_in(p, &v, uval, 4);
	if (!s->tcp) return 0;
	if (level == 6 && opt == 1) {           /* TCP_NODELAY */
		if (v) tcp_nagle_disable(s->tcp); else tcp_nagle_enable(s->tcp);
	} else if (level == 1 && opt == 9) {    /* SO_KEEPALIVE */
		if (v) ip_set_option(s->tcp, SOF_KEEPALIVE); else ip_reset_option(s->tcp, SOF_KEEPALIVE);
	} else if (level == 1 && opt == 2) {    /* SO_REUSEADDR */
		if (v) ip_set_option(s->tcp, SOF_REUSEADDR);
	} else if (level == 6 && opt == 4 && v > 0) {     /* TCP_KEEPIDLE */
		s->tcp->keep_idle = (u32)v * 1000;
	} else if (level == 6 && opt == 5 && v > 0) {     /* TCP_KEEPINTVL */
		s->tcp->keep_intvl = (u32)v * 1000;
	} else if (level == 6 && opt == 6 && v > 0) {     /* TCP_KEEPCNT */
		s->tcp->keep_cnt = (u32)v;
	}
	return 0;
}

long sock_syscall(struct task *t, int nr, u64 *a, bool *handled)
{
	struct lx_proc *p = t->lx->p;
	int err = 0;
	*handled = true;
	if (nr == 198) {                        /* socket */
		if (a[0] != AF_INET) return -EAFNOSUPPORT;
		int type = a[1] & 15;
		if (type != SOCK_STREAM && type != SOCK_DGRAM) return -EPROTONOSUPPORT;
		struct lx_sock *s = sock_new(type);
		if (!s) return -ENFILE;
		if (type == SOCK_STREAM) {
			struct tcp_pcb *pcb = tcp_new();
			if (!pcb) { s->used = false; return -ENOBUFS; }
			tcp_hook(s, pcb);
		} else {
			s->udp = udp_new();
			if (!s->udp) { s->used = false; return -ENOBUFS; }
			udp_recv(s->udp, u_recv, s);
		}
		return lx_sock_install(p, s, (u32)a[1]);
	}
	if (nr == 199) return -EAFNOSUPPORT;    /* socketpair */
	if (nr < 200 || (nr > 212 && nr != 242)) {
		*handled = false;
		return 0;
	}
	struct lx_sock *s = lx_fd_sock(p, (int)a[0], &err);
	if (!s) return err;
	bool nb = lx_fd_nonblock(p, (int)a[0]);
	ip_addr_t ip;
	u16 port;
	switch (nr) {
	case 200:                               /* bind */
		if ((err = get_addr(p, a[1], a[2], &ip, &port))) return err;
		if (s->type == SOCK_DGRAM)
			return udp_bind(s->udp, &ip, port) == ERR_OK ? 0 : -EADDRINUSE;
		return s->tcp && tcp_bind(s->tcp, &ip, port) == ERR_OK ? 0 : -EADDRINUSE;
	case 201: {                             /* listen */
		if (s->type != SOCK_STREAM || !s->tcp) return -EOPNOTSUPP;
		if (s->state == S_LISTEN) return 0;
		struct tcp_pcb *l = tcp_listen_with_backlog(s->tcp, ACQ);
		if (!l) return -ENOBUFS;
		s->tcp = l;
		s->state = S_LISTEN;
		tcp_arg(l, s);
		tcp_accept(l, t_accept);
		return 0;
	}
	case 202:                               /* accept */
	case 242: {                             /* accept4 */
		if (s->state != S_LISTEN) return -EINVAL;
		if (!s->acq_n) return nb ? -EAGAIN : LX_BLOCK;
		struct tcp_pcb *pcb = s->acq[0];
		struct lx_sock *n = sock_new(SOCK_STREAM);
		if (!n) return -ENFILE;
		memmove(&s->acq[0], &s->acq[1], (--s->acq_n) * sizeof(s->acq[0]));
		tcp_hook(n, pcb);
		n->state = S_CONNECTED;
		put_addr(p, a[1], a[2], &pcb->remote_ip, pcb->remote_port);
		return lx_sock_install(p, n, nr == 242 ? (u32)a[3] : 0);
	}
	case 203:                               /* connect */
		return s_connect(t, s, a[1], a[2], nb);
	case 204:                               /* getsockname */
		if (s->udp) return put_addr(p, a[1], a[2], &s->udp->local_ip, s->udp->local_port);
		return put_addr(p, a[1], a[2], s->tcp ? &s->tcp->local_ip : NULL, s->tcp ? s->tcp->local_port : 0);
	case 205:                               /* getpeername */
		if (s->udp) return put_addr(p, a[1], a[2], &s->udp->remote_ip, s->udp->remote_port);
		if (s->state != S_CONNECTED || !s->tcp) return -ENOTCONN;
		return put_addr(p, a[1], a[2], &s->tcp->remote_ip, s->tcp->remote_port);
	case 206: {                             /* sendto */
		u64 len = MIN(a[2], (u64)sizeof(sbuf));
		if (lx_copy_in(p, sbuf, a[1], len)) return -EFAULT;
		if (s->type == SOCK_DGRAM && a[4]) {
			if ((err = get_addr(p, a[4], a[5], &ip, &port))) return err;
			struct pbuf *pb = pbuf_alloc(PBUF_TRANSPORT, len, PBUF_RAM);
			if (!pb) return -ENOBUFS;
			pbuf_take(pb, sbuf, len);
			err_t e = udp_sendto(s->udp, pb, &ip, port);
			pbuf_free(pb);
			return e == ERR_OK ? (long)len : -ENETUNREACH;
		}
		return sock_write(s, sbuf, len, nb || (a[3] & 0x40));   /* MSG_DONTWAIT */
	}
	case 207: {                             /* recvfrom */
		u64 len = MIN(a[2], (u64)sizeof(sbuf));
		bool dgram = s->type == SOCK_DGRAM && s->dq_n;
		ip_addr_t from;
		u16 fport = 0;
		if (dgram) {
			ip_addr_copy(from, s->dq[s->dq_head].addr);
			fport = s->dq[s->dq_head].port;
		}
		long n = sock_read(s, sbuf, len, nb || (a[3] & 0x40));
		if (n <= 0) return n;
		if (lx_copy_out(p, a[1], sbuf, n)) return -EFAULT;
		if (dgram && a[4]) put_addr(p, a[4], a[5], &from, fport);
		return n;
	}
	case 208: return s_sockopt_set(p, s, (int)a[1], (int)a[2], a[3]);
	case 209: return s_sockopt_get(p, s, (int)a[1], (int)a[2], a[3], a[4]);
	case 210:                               /* shutdown */
		if (s->tcp && s->state == S_CONNECTED && a[1] != 0) {
			tcp_shutdown(s->tcp, 0, 1);
			s->shut_wr = true;
		}
		return 0;
	case 211: {                             /* sendmsg: the pieces gathered */
		u64 m[7];
		if (lx_copy_in(p, m, a[1], sizeof(m))) return -EFAULT;
		u64 len = 0;
		for (u64 i = 0; i < m[3] && i < 64; i++) {
			u64 v[2];
			if (lx_copy_in(p, v, m[2] + 16 * i, 16)) return -EFAULT;
			u64 n = MIN(v[1], sizeof(sbuf) - len);
			if (n && lx_copy_in(p, sbuf + len, v[0], n)) return -EFAULT;
			len += n;
		}
		bool dontwait = nb || (a[2] & 0x40);
		if (s->type == SOCK_DGRAM && m[0]) {
			if ((err = get_addr(p, m[0], (u32)m[1], &ip, &port))) return err;
			struct pbuf *pb = pbuf_alloc(PBUF_TRANSPORT, len, PBUF_RAM);
			if (!pb) return -ENOBUFS;
			pbuf_take(pb, sbuf, len);
			err_t e = udp_sendto(s->udp, pb, &ip, port);
			pbuf_free(pb);
			return e == ERR_OK ? (long)len : -ENETUNREACH;
		}
		return sock_write(s, sbuf, len, dontwait);
	}
	case 212: {                             /* recvmsg: into the pieces */
		u64 m[7];
		if (lx_copy_in(p, m, a[1], sizeof(m))) return -EFAULT;
		u64 room = 0;
		for (u64 i = 0; i < m[3] && i < 64; i++) {
			u64 v[2];
			if (lx_copy_in(p, v, m[2] + 16 * i, 16)) return -EFAULT;
			room += v[1];
		}
		bool dgram = s->type == SOCK_DGRAM && s->dq_n;
		ip_addr_t from;
		u16 fport = 0;
		if (dgram) {
			ip_addr_copy(from, s->dq[s->dq_head].addr);
			fport = s->dq[s->dq_head].port;
		}
		long n = sock_read(s, sbuf, MIN(room, (u64)sizeof(sbuf)), nb || (a[2] & 0x40));
		if (n <= 0) return n;
		u64 done = 0;
		for (u64 i = 0; i < m[3] && i < 64 && done < (u64)n; i++) {
			u64 v[2];
			lx_copy_in(p, v, m[2] + 16 * i, 16);
			u64 k = MIN(v[1], (u64)n - done);
			if (k && lx_copy_out(p, v[0], sbuf + done, k)) return -EFAULT;
			done += k;
		}
		if (dgram && m[0]) put_addr(p, m[0], a[1] + 8, &from, fport);
		u64 zero = 0;
		lx_copy_out(p, a[1] + 40, &zero, 8);        /* no control data */
		u32 flags = 0;
		lx_copy_out(p, a[1] + 48, &flags, 4);
		return n;
	}
	default:
		kprintf("lx: socket call %d not there\n", nr);
		return -ENOSYS;
	}
}

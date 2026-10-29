/* KolibriOS for ARM: the Linux personality's sockets (sock.c, on lwIP) */
#ifndef LX_SOCK_H
#define LX_SOCK_H

#include "lx.h"

struct lx_sock;
long sock_read(struct lx_sock *s, void *buf, u64 len, bool nb);
long sock_write(struct lx_sock *s, const void *buf, u64 len, bool nb);
u32 sock_poll(struct lx_sock *s, u32 *gen);
void sock_close(struct lx_sock *s);
long sock_syscall(struct task *t, int nr, u64 *a, bool *handled);

/* file.c */
int lx_sock_install(struct lx_proc *p, struct lx_sock *s, u32 flags);
struct lx_sock *lx_fd_sock(struct lx_proc *p, int fd, int *err);
bool lx_fd_nonblock(struct lx_proc *p, int fd);

#endif

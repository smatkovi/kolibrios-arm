/* KolibriOS for ARM: the C library (lib/libc): the current folder,
 * function 30 (30.1 set, 30.2 get) */
#include <unistd.h>

char *getcwd(char *buf, size_t size)
{
	register unsigned r0 asm("r0") = 30, r1 asm("r1") = 2, r2 asm("r2") = (unsigned)buf, r3 asm("r3") = size;
	asm volatile("svc #0x40" : "+r"(r0), "+r"(r1), "+r"(r2), "+r"(r3) :: "r4", "r5", "memory");
	return buf;
}

int chdir(const char *path)
{
	register unsigned r0 asm("r0") = 30, r1 asm("r1") = 1, r2 asm("r2") = (unsigned)path;
	asm volatile("svc #0x40" : "+r"(r0), "+r"(r1), "+r"(r2) :: "r3", "r4", "r5", "memory");
	return 0;
}

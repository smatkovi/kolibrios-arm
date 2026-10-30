/* KolibriOS for ARM: the C library (lib/libc): the start of a program
 * with main(argc, argv), as KolibriOS's C libraries do it: argv[0] is the
 * program's path, the parameters split at blanks ("..." keeps blanks).
 * lib/crt0.S calls this instead of main when built with -DLIBC. */
#include <stdlib.h>
#include <string.h>

extern char __path[];
int main(int argc, char **argv);

#define MAX_ARGS 32
static char args[256];

void __libc_start_main(const char *params)
{
	static char *argv[MAX_ARGS + 1];
	int argc = 0;
	argv[argc++] = __path;
	strncpy(args, params, sizeof(args) - 1);
	for (char *p = args; *p && argc < MAX_ARGS;) {
		while (*p == ' ' || *p == '\t') p++;
		if (!*p) break;
		if (*p == '"') {
			argv[argc++] = ++p;
			while (*p && *p != '"') p++;
		} else {
			argv[argc++] = p;
			while (*p && *p != ' ' && *p != '\t') p++;
		}
		if (*p) *p++ = 0;
	}
	argv[argc] = NULL;
	exit(main(argc, argv));
}

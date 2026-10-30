/* KolibriOS for ARM: the C library (lib/libc): the start of a program
 * with main(argc, argv), as KolibriOS's C libraries do it: argv[0] is the
 * program's path, the parameters split at blanks ("..." keeps blanks).
 * lib/crt0.S calls this instead of main when built with -DLIBC. The
 * libraries the program uses as KolibriOS's (68.19) are loaded first. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <kos_dll.h>

extern char __path[];
extern const struct kos_import __import_start[];   /* lib/app.ld: the libraries the program links to */
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
	if (__import_start[0].table && dll_load(__import_start)) {
		printf("%s: its libraries could not be loaded\n", __path);
		exit(1);
	}
	exit(main(argc, argv));
}

/* KolibriOS for ARM: console.obj used as a library (68.19): what the
 * program has itself. con_init without arguments is ktcc's conio.h's,
 * console.obj's con_init (con_init_opt here) with the defaults. */
#include "conio.h"

int __con_is_load = 1;

int con_init(void)
{
	return con_init_opt(-1, -1, -1, -1, "Console");
}

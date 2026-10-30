/* KolibriOS for ARM: t_edit's string functions, translated to C from
 * programs/other/t_edit/strlen.inc (IgorA, the KolibriOS team; GPL-2.0) */
#include "t_edit.h"

u32 str_len(const char *s)
{
	const char *p = s;
	while (*p) p++;
	return p - s;
}

void str_cat(char *str1, const char *str2)
{
	char *d = str1 + str_len(str1);
	while ((*d++ = *str2++));
}

/* 0 if str1 is at the start of str0 */
int str_instr(const char *str0, const char *str1)
{
	for (;; str0++, str1++) {
		u8 a = *str1;
		if (!a) return 0;                  /* str1 ended */
		if (a != (u8)*str0) return (u8)(a - (u8)*str0);
	}
}

/* the number in decimal into edi (len bytes, the 0 included) */
static void conv_str(u32 v, char **edi, char *end)
{
	if (v >= 10) conv_str(v / 10, edi, end);
	if (*edi < end) {
		*(*edi)++ = '0' | (v % 10);
		**edi = 0;
	}
}

void convert_int_to_str(u32 v, char *edi, int len)
{
	conv_str(v, &edi, edi + len - 1);
}

/* a number in decimal or (0x...) hexadecimal, '-' before it negative */
s32 conv_str_to_int(const char *s)
{
	s32 v = 0;
	int neg = 0;
	if (*s == '-') { neg = 1; s++; }
	if (s[0] == '0' && s[1] == 'x') {
		for (s += 2;; s++) {
			u8 c = *s;
			if (c >= '0' && c <= '9') c -= '0';
			else if (c >= 'A' && c <= 'F') c -= 'A' - 10;
			else if (c >= 'a' && c <= 'f') c -= 'a' - 10;
			else break;
			v = v << 4 | c;
		}
	} else {
		for (; *s >= '0' && *s <= '9'; s++) v = v * 10 + *s - '0';
	}
	return neg ? -v : v;
}

/* KolibriOS for ARM: keyboard and mouse input.
 *
 * Function 2 gives a program al = 0, ah = the key's ASCII code and bits
 * 16-23 = its scan code (set 1, as KolibriOS has them from the PC
 * keyboard). PS/2 keyboards (QEMU) send set 2, which is translated here;
 * other keyboards (the N950's matrix) report set 1 codes directly. */
#include "gui.h"
#include "rng.h"

/* the i8042 controller's translation of set 2 codes into set 1 */
static const u8 set2_to_set1[128] = {
	0xff, 0x43, 0x41, 0x3f, 0x3d, 0x3b, 0x3c, 0x58, 0x64, 0x44, 0x42, 0x40, 0x3e, 0x0f, 0x29, 0x59,
	0x65, 0x38, 0x2a, 0x70, 0x1d, 0x10, 0x02, 0x5a, 0x66, 0x71, 0x2c, 0x1f, 0x1e, 0x11, 0x03, 0x5b,
	0x67, 0x2e, 0x2d, 0x20, 0x12, 0x05, 0x04, 0x5c, 0x68, 0x39, 0x2f, 0x21, 0x14, 0x13, 0x06, 0x5d,
	0x69, 0x31, 0x30, 0x23, 0x22, 0x15, 0x07, 0x5e, 0x6a, 0x72, 0x32, 0x24, 0x16, 0x08, 0x09, 0x5f,
	0x6b, 0x33, 0x25, 0x17, 0x18, 0x0b, 0x0a, 0x60, 0x6c, 0x34, 0x35, 0x26, 0x27, 0x19, 0x0c, 0x61,
	0x6d, 0x73, 0x28, 0x74, 0x1a, 0x0d, 0x62, 0x6e, 0x3a, 0x36, 0x1c, 0x1b, 0x75, 0x2b, 0x63, 0x76,
	0x55, 0x56, 0x77, 0x78, 0x79, 0x7a, 0x0e, 0x7b, 0x7c, 0x4f, 0x7d, 0x4b, 0x47, 0x7e, 0x7f, 0x6f,
	0x52, 0x53, 0x50, 0x4c, 0x4d, 0x48, 0x01, 0x45, 0x57, 0x4e, 0x51, 0x4a, 0x37, 0x49, 0x46, 0x54,
};

/* US layout, set 1 codes 0x00-0x39 (and the terminating zero) */
static const char key_lower[0x3b] =
	"\0\x1b" "1234567890-=\b\tqwertyuiop[]\r\0asdfghjkl;'`\0\\zxcvbnm,./\0*\0 ";
static const char key_upper[0x3b] =
	"\0\x1b" "!@#$%^&*()_+\b\tQWERTYUIOP{}\r\0ASDFGHJKL:\"~\0|ZXCVBNM<>?\0*\0 ";

static bool shift_l, shift_r, ctrl_l, ctrl_r, alt_l, alt_r, caps, num, scroll;
static bool e0, release;

/* KolibriOS's keyboard layouts (function 26.2), US: its kernel's keymap,
 * keymap_shift and keymap_alt (kernel/trunk/data32.inc), their first 128
 * bytes, by set 1 scan code */
#define K_ROWS(a, b, c, d) a b c d "ABCDEFGHIJKLMNOPQR"
static const char layout_normal[128] __attribute__((nonstring)) =
	K_ROWS("6\x1b" "1234567890-=\b\t" "qwertyuiop[]\r" "~asdfghjkl;'`\0\\zxcvbnm,./\0" "45 ",
	       "@234567890123\xb4\xb2\xb8" "6\xb0" "7", "\xb3" "8\xb5\xb1\xb7\xb9\xb6", "AB<D\xff" "FGHIJKLMNOPQRSTUVWXYZ");
static const char layout_shift[128] __attribute__((nonstring)) =
	K_ROWS("6\x1b" "!@#$%^&*()_+\b\t" "QWERTYUIOP{}\r" "~ASDFGHJKL:\"~\0|ZXCVBNM<>?\0" "45 ",
	       "@234567890123\xb4\xb2\xb8" "6\xb0" "7", "\xb3" "8\xb5\xb1\xb7\xb9\xb6", "AB>D\xff" "FGHIJKLMNOPQRSTUVWXYZ");
static const char layout_alt[128] __attribute__((nonstring)) =
	K_ROWS(" \x1b" " @ $  {[]}\\ \b\t" "            \r" "             \0           \0" "4\0 ",
	       "             \xb4\xb2\xb8" "6\xb0" "7", "\xb3" "8\xb5\xb1\xb7\xb9\xb6", "ABCD\xff" "FGHIJKLMNOPQRSTUVWXYZ");
static const char *const layouts[3] = { layout_normal, layout_shift, layout_alt };

/* function 26.2: layout 1 normal, 2 with Shift, 3 with Alt */
void input_layout(int which, u8 *out)
{
	memcpy(out, layouts[which >= 1 && which <= 3 ? which - 1 : 0], 128);
}

/* function 66.3: the control keys */
u32 input_modifiers(void)
{
	return shift_l | shift_r << 1 | ctrl_l << 2 | ctrl_r << 3 | alt_l << 4 | alt_r << 5 |
	       caps << 6 | num << 7 | scroll << 8;
}

/* KolibriOS's codes for the keys without ASCII (its default keymap) */
static u8 special(u8 sc)
{
	switch (sc) {
	case 0x4b: return 0xb0; /* left */
	case 0x50: return 0xb1; /* down */
	case 0x48: return 0xb2; /* up */
	case 0x4d: return 0xb3; /* right */
	case 0x47: return 0xb4; /* home */
	case 0x4f: return 0xb5; /* end */
	case 0x53: return 0xb6; /* delete */
	case 0x51: return 0xb7; /* page down */
	case 0x49: return 0xb8; /* page up */
	case 0x52: return 0xb9; /* insert */
	}
	return 0;
}

/* a key with its E0 prefix (the gray keys of a PC keyboard) pressed or
 * released. A program in scan code mode (function 66.1) gets every
 * byte: 0xE0, then the code, | 0x80 when released, the control keys
 * too; in ASCII mode the presses of the other keys, with their ASCII
 * code (or KolibriOS's code for the arrows and such) and the scan code */
static void key(u8 sc, bool ex, bool released)
{
	switch (sc) {
	case 0x2a: shift_l = !released; break;
	case 0x36: shift_r = !released; break;
	case 0x1d: if (ex) ctrl_r = !released; else ctrl_l = !released; break;
	case 0x38: if (ex) alt_r = !released; else alt_l = !released; break;
	case 0x3a: if (!released) caps = !caps; break;
	case 0x45: if (!released) num = !num; break;
	case 0x46: if (!released) scroll = !scroll; break;
	}
	struct task *a = wm_active();
	if (a && a->kbd_mode == 1) {
		if (ex) wm_key(0xe0 << 8);
		wm_key((u32)(sc | (released ? 0x80 : 0)) << 8);
		return;
	}
	if (released) return;
	switch (sc) {
	case 0x2a: case 0x36: case 0x1d: case 0x38: case 0x3a: case 0x45: case 0x46:
		return;
	}
	u8 ascii = special(sc);
	if (!ascii && sc < sizeof(key_lower)) {
		bool sh = shift_l || shift_r;
		char c = sh ? key_upper[sc] : key_lower[sc];
		if (caps && c >= 'a' && c <= 'z') c -= 32;
		else if (caps && c >= 'A' && c <= 'Z' && sh) c += 32;
		if ((ctrl_l || ctrl_r) && ((c | 32) >= 'a' && (c | 32) <= 'z')) c = (c | 32) - 'a' + 1;
		ascii = c;
	}
	wm_key((u32)sc << 16 | (u32)ascii << 8);
}

/* a key from a keyboard without E0 prefixes (the phones', virtio): the
 * cursor block as the gray keys, since these have no numeric keypad */
u32 input_last;

void input_key(u8 sc, bool released)
{
	input_last = ticks;
	rng_event(sc | (u32)released << 8);     /* its time into the random numbers' pool, credited nothing */
	bool ex = sc == 0x47 || sc == 0x48 || sc == 0x49 || sc == 0x4b || sc == 0x4d ||
		  sc == 0x4f || sc == 0x50 || sc == 0x51 || sc == 0x52 || sc == 0x53;
	key(sc, ex, released);
}

void input_key_set2(u8 b)
{
	if (b == 0xe0) { e0 = true; return; }
	if (b == 0xf0) { release = true; return; }
	if (b == 0xe1 || b == 0xfa || b == 0xaa) return;
	input_last = ticks;
	u8 sc = b < 128 ? set2_to_set1[b] : 0xff;
	if (b == 0x83) sc = 0x41;            /* F7 */
	bool rel = release, ex = e0;
	e0 = release = false;
	if (sc == 0xff) return;
	if (ex && sc == 0x2a) return;        /* the fake shifts around the gray keys */
	if (ex && sc == 0x36) return;
	key(sc, ex, rel);
}

static int mx, my, mb;

/* the wheel's turns since function 37.7 read them (+: towards the user) */
int input_wheel;

void input_mouse_rel(int dx, int dy, int buttons, int wheel)
{
	input_last = ticks;
	rng_event((u32)(dx & 0xfff) | (u32)(dy & 0xfff) << 12 | (u32)buttons << 24);
	input_wheel += wheel;
	mx = MAX(0, MIN(mx + dx, scr.w - 1));
	my = MAX(0, MIN(my + dy, scr.h - 1));
	int changed = buttons ^ mb;
	mb = buttons;
	wm_mouse(mx, my, buttons, changed);
}

/* a turn of the wheel alone (+: towards the user) */
void input_mouse_wheel(int dz)
{
	input_last = ticks;
	input_wheel += dz;
	wm_mouse(mx, my, mb, 0);
}

void input_mouse_abs(int x, int y, int buttons)
{
	input_last = ticks;
	rng_event((u32)(x & 0xfff) | (u32)(y & 0xfff) << 12 | (u32)buttons << 24);
	mx = x;
	my = y;
	int changed = buttons ^ mb;
	mb = buttons;
	wm_mouse(mx, my, buttons, changed);
}

void input_init(void)
{
	mx = scr.w / 2;
	my = scr.h / 2;
}

/* KolibriOS for ARM: keyboard and mouse input.
 *
 * Function 2 gives a program al = 0, ah = the key's ASCII code and bits
 * 16-23 = its scan code (set 1, as KolibriOS has them from the PC
 * keyboard). PS/2 keyboards (QEMU) send set 2, which is translated here;
 * other keyboards (the N950's matrix) report set 1 codes directly. */
#include "gui.h"

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

static bool shift_l, shift_r, ctrl, alt, caps;
static bool e0, release;

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

void input_key(u8 sc, bool released)
{
	switch (sc) {
	case 0x2a: shift_l = !released; return;
	case 0x36: shift_r = !released; return;
	case 0x1d: ctrl = !released; return;
	case 0x38: alt = !released; return;
	case 0x3a: if (!released) caps = !caps; return;
	}
	if (released) return;
	u8 ascii = special(sc);
	if (!ascii && sc < sizeof(key_lower)) {
		bool sh = shift_l || shift_r;
		char c = sh ? key_upper[sc] : key_lower[sc];
		if (caps && c >= 'a' && c <= 'z') c -= 32;
		else if (caps && c >= 'A' && c <= 'Z' && sh) c += 32;
		if (ctrl && ((c | 32) >= 'a' && (c | 32) <= 'z')) c = (c | 32) - 'a' + 1;
		ascii = c;
	}
	wm_key((u32)sc << 16 | (u32)ascii << 8);
}

void input_key_set2(u8 b)
{
	if (b == 0xe0) { e0 = true; return; }
	if (b == 0xf0) { release = true; return; }
	if (b == 0xe1 || b == 0xfa || b == 0xaa) return;
	u8 sc = b < 128 ? set2_to_set1[b] : 0xff;
	if (b == 0x83) sc = 0x41;            /* F7 */
	bool rel = release;
	e0 = release = false;
	if (sc == 0xff) return;
	input_key(sc, rel);
}

static int mx, my, mb;

void input_mouse_rel(int dx, int dy, int buttons, int wheel)
{
	(void)wheel;
	mx = MAX(0, MIN(mx + dx, scr.w - 1));
	my = MAX(0, MIN(my + dy, scr.h - 1));
	int changed = buttons ^ mb;
	mb = buttons;
	wm_mouse(mx, my, buttons, changed);
}

void input_mouse_abs(int x, int y, int buttons)
{
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

/* KolibriOS for ARM: libini in C (lib/libini/libini.h). The original
 * (programs/develop/libraries/libs-dev/libini, Alexey "mike.dld"
 * Tsaplin, KolibriOS team, GPL-2.0+) reads the file through libio in
 * blocks; here the file is read whole with function 70.0 and written
 * whole with 70.2. The same rules: sections "[name]" (blanks around the
 * name allowed), "key=value" lines (blanks before the key and around
 * '=' skipped; the value up to the line's end), ';' comments, names
 * compared as written. */
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <kos.h>
#include "libini.h"

/* the file in memory, terminated by a 0 */
static char *load(const char *name, unsigned *len)
{
	unsigned cap = 4096, n = 0;
	char *buf = malloc(cap + 1);
	if (!buf) return NULL;
	for (;;) {
		struct kos_r2 r = kos_read_file(name, buf + n, cap - n, n);
		if (r.eax != 0 && r.eax != 6) { free(buf); return NULL; }
		n += r.ebx;
		if (r.eax == 6 || n < cap) break;
		char *b = realloc(buf, cap * 2 + 1);
		if (!b) { free(buf); return NULL; }
		buf = b;
		cap *= 2;
	}
	buf[n] = 0;
	*len = n;
	return buf;
}

static int save(const char *name, const char *buf, unsigned len)
{
	struct kos_r2 r = kos_write_file(name, buf, len);
	return r.eax ? -1 : 0;
}

static int blank(char c) { return c == ' ' || c == '\t'; }
static int eol(char c) { return c == '\r' || c == '\n' || !c; }
static const char *next_line(const char *p)
{
	while (!eol(*p)) p++;
	while (*p == '\r' || *p == '\n') p++;
	return p;
}

/* a section's header at p ("[ name ]"): the name's start and length */
static int section_at(const char *p, const char **name, int *len)
{
	while (blank(*p)) p++;
	if (*p != '[') return 0;
	p++;
	while (blank(*p)) p++;
	const char *s = p;
	while (!eol(*p) && *p != ']') p++;
	if (*p != ']') return 0;
	const char *e = p;
	while (e > s && blank(e[-1])) e--;
	*name = s;
	*len = e - s;
	return 1;
}

/* the line after the header of section sec, or NULL */
static const char *find_section(const char *buf, const char *sec)
{
	for (const char *p = buf; *p; p = next_line(p)) {
		const char *n;
		int l;
		if (section_at(p, &n, &l) && (int)strlen(sec) == l && !strncmp(n, sec, l))
			return next_line(p);
	}
	return NULL;
}

/* a "key=value" line at p: the key's and the value's start and length */
static int key_at(const char *p, const char **key, int *klen, const char **val, int *vlen)
{
	while (blank(*p)) p++;
	if (eol(*p) || *p == ';' || *p == '[') return 0;
	const char *k = p;
	while (!eol(*p) && *p != '=') p++;
	if (*p != '=') return 0;
	const char *ke = p;
	while (ke > k && blank(ke[-1])) ke--;
	p++;
	while (blank(*p)) p++;
	const char *v = p;
	while (!eol(*p)) p++;
	const char *ve = p;
	while (ve > v && blank(ve[-1])) ve--;
	*key = k;
	*klen = ke - k;
	*val = v;
	*vlen = ve - v;
	return 1;
}

static int is_section(const char *p)
{
	const char *n;
	int l;
	return section_at(p, &n, &l);
}

int ini_enum_sections(const char *f_name, ini_sec_cb callback)
{
	unsigned len;
	char *buf = load(f_name, &len);
	if (!buf) return -1;
	char name[256];
	for (const char *p = buf; *p; p = next_line(p)) {
		const char *n;
		int l;
		if (!section_at(p, &n, &l)) continue;
		l = l < 255 ? l : 255;
		memcpy(name, n, l);
		name[l] = 0;
		if (!callback(f_name, name)) break;
	}
	free(buf);
	return 0;
}

int ini_enum_keys(const char *f_name, const char *sec_name, ini_key_cb callback)
{
	unsigned len;
	char *buf = load(f_name, &len);
	if (!buf) return -1;
	const char *p = find_section(buf, sec_name);
	if (!p) { free(buf); return -1; }
	char key[256], val[4096];
	for (; *p && !is_section(p); p = next_line(p)) {
		const char *k, *v;
		int kl, vl;
		if (!key_at(p, &k, &kl, &v, &vl)) continue;
		kl = kl < 255 ? kl : 255;
		vl = vl < 4095 ? vl : 4095;
		memcpy(key, k, kl);
		key[kl] = 0;
		memcpy(val, v, vl);
		val[vl] = 0;
		if (!callback(f_name, sec_name, key, val)) break;
	}
	free(buf);
	return 0;
}

/* the value of key in sec: its start and length, or NULL */
static const char *find_value(const char *buf, const char *sec, const char *key, int *vlen)
{
	const char *p = find_section(buf, sec);
	if (!p) return NULL;
	int kl0 = strlen(key);
	for (; *p && !is_section(p); p = next_line(p)) {
		const char *k, *v;
		int kl, vl;
		if (key_at(p, &k, &kl, &v, &vl) && kl == kl0 && !strncmp(k, key, kl)) {
			*vlen = vl;
			return v;
		}
	}
	return NULL;
}

int ini_get_str(const char *f_name, const char *sec_name, const char *key_name, char *buffer, int buf_len, const char *def_val)
{
	unsigned len;
	char *buf = load(f_name, &len);
	const char *v = NULL;
	int vl = 0;
	if (buf) v = find_value(buf, sec_name, key_name, &vl);
	if (!v) {
		if (buf_len > 0) {
			strncpy(buffer, def_val ? def_val : "", buf_len - 1);
			buffer[buf_len - 1] = 0;
		}
		free(buf);
		return -1;
	}
	if (buf_len > 0) {
		if (vl > buf_len - 1) vl = buf_len - 1;
		memcpy(buffer, v, vl);
		buffer[vl] = 0;
	}
	free(buf);
	return 0;
}

int ini_set_str(const char *f_name, const char *sec_name, const char *key_name, const char *buffer, int buf_len)
{
	unsigned len = 0;
	char *buf = load(f_name, &len);
	if (!buf) {
		buf = malloc(1);
		if (!buf) return -1;
		buf[0] = 0;
		len = 0;
	}
	int sl = strlen(sec_name), kl = strlen(key_name);
	if (buf_len < 0) buf_len = 0;
	char *out = malloc(len + sl + kl + buf_len + 16);
	if (!out) { free(buf); return -1; }
	unsigned o = 0;
	const char *sec = find_section(buf, sec_name);
	if (!sec) {
		/* a new section at the end */
		memcpy(out, buf, len);
		o = len;
		if (o && out[o - 1] != '\n') out[o++] = '\r', out[o++] = '\n';
		o += sprintf(out + o, "[%s]\r\n", sec_name);
		memcpy(out + o, key_name, kl);
		o += kl;
		out[o++] = '=';
		memcpy(out + o, buffer, buf_len);
		o += buf_len;
		out[o++] = '\r';
		out[o++] = '\n';
	} else {
		int vl;
		const char *v = find_value(buf, sec_name, key_name, &vl);
		if (v) {
			/* the value replaced */
			memcpy(out, buf, v - buf);
			o = v - buf;
			memcpy(out + o, buffer, buf_len);
			o += buf_len;
			const char *rest = v + vl;
			memcpy(out + o, rest, len - (rest - buf));
			o += len - (rest - buf);
		} else {
			/* a new key after the section's last key line */
			const char *p = sec, *end = sec;
			for (; *p && !is_section(p); p = next_line(p)) {
				const char *k, *vv;
				int kk, vvl;
				if (key_at(p, &k, &kk, &vv, &vvl)) end = next_line(p);
			}
			memcpy(out, buf, end - buf);
			o = end - buf;
			if (o && out[o - 1] != '\n') out[o++] = '\r', out[o++] = '\n';
			memcpy(out + o, key_name, kl);
			o += kl;
			out[o++] = '=';
			memcpy(out + o, buffer, buf_len);
			o += buf_len;
			out[o++] = '\r';
			out[o++] = '\n';
			memcpy(out + o, end, len - (end - buf));
			o += len - (end - buf);
		}
	}
	int r = save(f_name, out, o);
	free(out);
	free(buf);
	return r;
}

int ini_get_int(const char *f_name, const char *sec_name, const char *key_name, int def_val)
{
	char s[32];
	if (ini_get_str(f_name, sec_name, key_name, s, sizeof(s), NULL)) return def_val;
	const char *p = s;
	int neg = *p == '-';
	if (neg) p++;
	if (*p < '0' || *p > '9') return def_val;
	int v = 0;
	while (*p >= '0' && *p <= '9') v = v * 10 + *p++ - '0';
	return neg ? -v : v;
}

int ini_set_int(const char *f_name, const char *sec_name, const char *key_name, int val)
{
	char s[16];
	int n = sprintf(s, "%d", val);
	return ini_set_str(f_name, sec_name, key_name, s, n);
}

static unsigned str_int(const char **p)
{
	unsigned v = 0;
	while (**p == ' ') (*p)++;
	while (**p >= '0' && **p <= '9') v = v * 10 + *(*p)++ - '0';
	return v;
}

unsigned ini_get_color(const char *f_name, const char *sec_name, const char *key_name, unsigned def_val)
{
	char s[14];
	ini_get_str(f_name, sec_name, key_name, s, sizeof(s), "");
	if (!s[0]) return def_val;
	const char *p = s;
	unsigned c = (str_int(&p) & 0xff) << 16;
	if (*p++ == ',') {
		c |= (str_int(&p) & 0xff) << 8;
		if (*p++ == ',') c |= str_int(&p) & 0xff;
	}
	return c;
}

int ini_set_color(const char *f_name, const char *sec_name, const char *key_name, unsigned val)
{
	char s[16];
	int n = sprintf(s, "%u,%u,%u", (val >> 16) & 0xff, (val >> 8) & 0xff, val & 0xff);
	return ini_set_str(f_name, sec_name, key_name, s, n);
}

/* modifiers: 0 none, 1 one of L/R, 2 both, 3 left, 4 right, a nibble each */
static unsigned mod_generic(unsigned m, int shift)
{
	unsigned v = (m >> shift) & 0xf;
	v = v ? 2 : 1;
	return (m & ~(0xfu << shift)) | v << shift;
}

static unsigned mod_side(unsigned m, int shift, unsigned side)
{
	unsigned v = (m >> shift) & 0xf;
	v = !v || v == side ? side : 2;
	return (m & ~(0xfu << shift)) | v << shift;
}

int ini_get_shortcut(const char *f_name, const char *sec_name, const char *key_name, int def_val, unsigned *modifiers)
{
	static const char *const names[] = { "Ctrl", "Alt", "Shift", "LCtrl", "RCtrl", "LAlt", "RAlt", "LShift", "RShift",
		"Home", "End", "PgUp", "PgDn", "Ins", "Insert", "Del", "Delete", "Tab", "Plus", "Esc", "Enter",
		"Backspace", "Space", "Left", "Right", "Up", "Down" };
	static const unsigned char scans[] = { 0x47, 0x4f, 0x49, 0x51, 0x52, 0x52, 0x53, 0x53, 0x0f, 0x4e, 0x01, 0x1c,
		0x0e, 0x39, 0x4b, 0x4d, 0x48, 0x50 };
	char s[64];
	ini_get_str(f_name, sec_name, key_name, s, sizeof(s), "");
	if (!s[0]) return def_val;
	unsigned m = 0;
	int scan = 0;
	const char *p = s;
	for (;;) {
		while (*p == '+') p++;
		if (!*p) break;
		int found = -1, fl = 0;
		for (unsigned i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
			int l = strlen(names[i]);
			if (!strncasecmp(p, names[i], l)) { found = i; fl = l; break; }
		}
		if (found >= 0) {
			p += fl;
			switch (found) {
			case 0: m = mod_generic(m, 4); continue;
			case 1: m = mod_generic(m, 8); continue;
			case 2: m = mod_generic(m, 0); continue;
			case 3: m = mod_side(m, 4, 3); continue;
			case 4: m = mod_side(m, 4, 4); continue;
			case 5: m = mod_side(m, 8, 3); continue;
			case 6: m = mod_side(m, 8, 4); continue;
			case 7: m = mod_side(m, 0, 3); continue;
			case 8: m = mod_side(m, 0, 4); continue;
			}
			scan = scans[found - 9];
			break;
		}
		if ((*p == 'f' || *p == 'F') && p[1] >= '1' && p[1] <= '9') {
			const char *q = p + 1;
			int n = str_int(&q);
			scan = n <= 10 ? 0x3b + n - 1 : 0x57 + n - 11;
			break;
		}
		/* an ordinary key: its scan code from the keyboard layout (26.2) */
		u8 lay[128];
		for (int t = 1; t <= 2 && !scan; t++) {
			kos_call(26, 2, t, (u32)lay, 0, 0);
			for (int i = 1; i <= 0x36; i++)
				if (lay[i] == (u8)*p) { scan = i; break; }
		}
		if (!scan) return def_val;
		break;
	}
	if (modifiers) *modifiers = m;
	return scan;
}

int ini_del_section(const char *f_name, const char *sec_name)
{
	unsigned len;
	char *buf = load(f_name, &len);
	if (!buf) return -1;
	const char *body = find_section(buf, sec_name);
	if (!body) { free(buf); return -1; }
	/* from the header's line to the next section */
	const char *start = buf;
	for (const char *p = buf; p < body; p = next_line(p))
		if (is_section(p)) start = p;
	const char *end = body;
	while (*end && !is_section(end)) end = next_line(end);
	memmove((char *)start, end, len - (end - buf) + 1);
	int r = save(f_name, buf, len - (end - start));
	free(buf);
	return r;
}

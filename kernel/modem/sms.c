/* KolibriOS for ARM: SMS PDUs.
 *
 * As the modem has them (QMI WMS, format "GSM/WCDMA point-to-point"): the
 * service centre's address (a length octet, 0: the default), then the
 * TPDU: the first octet (message type, user data header indicator,
 * validity format), the other side's address (digits as swapped BCD, or
 * GSM 7-bit letters), protocol id, data coding scheme, the time stamp
 * (DELIVER) or validity (SUBMIT), the user data length (septets for the
 * 7-bit alphabet, octets else) and the user data, a header first when the
 * indicator says so (concatenation: IE 0 or 8). */
#include "sms.h"

/* the GSM 7-bit default alphabet (23.038 6.2.1) */
static const u16 gsm7[128] = {
	'@', 0xa3, '$', 0xa5, 0xe8, 0xe9, 0xf9, 0xec, 0xf2, 0xc7, '\n', 0xd8, 0xf8, '\r', 0xc5, 0xe5,
	0x394, '_', 0x3a6, 0x393, 0x39b, 0x3a9, 0x3a0, 0x3a8, 0x3a3, 0x398, 0x39e, 0x1b, 0xc6, 0xe6, 0xdf, 0xc9,
	' ', '!', '"', '#', 0xa4, '%', '&', '\'', '(', ')', '*', '+', ',', '-', '.', '/',
	'0', '1', '2', '3', '4', '5', '6', '7', '8', '9', ':', ';', '<', '=', '>', '?',
	0xa1, 'A', 'B', 'C', 'D', 'E', 'F', 'G', 'H', 'I', 'J', 'K', 'L', 'M', 'N', 'O',
	'P', 'Q', 'R', 'S', 'T', 'U', 'V', 'W', 'X', 'Y', 'Z', 0xc4, 0xd6, 0xd1, 0xdc, 0xa7,
	0xbf, 'a', 'b', 'c', 'd', 'e', 'f', 'g', 'h', 'i', 'j', 'k', 'l', 'm', 'n', 'o',
	'p', 'q', 'r', 's', 't', 'u', 'v', 'w', 'x', 'y', 'z', 0xe4, 0xf6, 0xf1, 0xfc, 0xe0,
};
/* its extension table: after the escape (0x1b) */
static const u16 gsm7ext[][2] = {
	{ 0x0a, 0x0c }, { 0x14, '^' }, { 0x28, '{' }, { 0x29, '}' }, { 0x2f, '\\' },
	{ 0x3c, '[' }, { 0x3d, '~' }, { 0x3e, ']' }, { 0x40, '|' }, { 0x65, 0x20ac },
};

u32 utf8_get(const char **sp)
{
	const u8 *s = (const u8 *)*sp;
	u32 c = *s;
	if (!c) return 0;
	int n = c < 0x80 ? 0 : c < 0xe0 ? 1 : c < 0xf0 ? 2 : 3;
	if (n) c &= 0x3f >> n;
	s++;
	for (; n; n--, s++) {
		if ((*s & 0xc0) != 0x80) { c = 0xfffd; break; }
		c = c << 6 | (*s & 0x3f);
	}
	*sp = (const char *)s;
	return c;
}

int utf8_put(char *d, u32 c)
{
	if (c < 0x80) { d[0] = c; return 1; }
	if (c < 0x800) { d[0] = 0xc0 | c >> 6; d[1] = 0x80 | (c & 0x3f); return 2; }
	if (c < 0x10000) { d[0] = 0xe0 | c >> 12; d[1] = 0x80 | (c >> 6 & 0x3f); d[2] = 0x80 | (c & 0x3f); return 3; }
	d[0] = 0xf0 | c >> 18; d[1] = 0x80 | (c >> 12 & 0x3f); d[2] = 0x80 | (c >> 6 & 0x3f); d[3] = 0x80 | (c & 0x3f);
	return 4;
}

/* a code point in the 7-bit alphabet: its septets (1, 2 with the
 * escape), 0: not there */
static int gsm7_of(u32 c, u8 *sep)
{
	for (int i = 0; i < 128; i++)
		if (gsm7[i] == c && i != 0x1b) { sep[0] = i; return 1; }
	for (unsigned i = 0; i < sizeof(gsm7ext) / sizeof(gsm7ext[0]); i++)
		if (gsm7ext[i][1] == c) { sep[0] = 0x1b; sep[1] = gsm7ext[i][0]; return 2; }
	return 0;
}

static void put_septets(u8 *ud, u32 bit, const u8 *sep, u32 n)
{
	for (u32 i = 0; i < n; i++, bit += 7) {
		u32 o = bit / 8, sh = bit % 8;
		ud[o] |= (u8)(sep[i] << sh);
		if (sh > 1) ud[o + 1] |= sep[i] >> (8 - sh);
	}
}

static u8 get_septet(const u8 *ud, u32 len, u32 bit)
{
	u32 o = bit / 8, sh = bit % 8;
	u32 v = o < len ? ud[o] >> sh : 0;
	if (sh > 1 && o + 1 < len) v |= ud[o + 1] << (8 - sh);
	return v & 0x7f;
}

/* septets from bit on as UTF-8 into d (n bytes); the length */
static u32 gsm7_text(const u8 *ud, u32 len, u32 bit, u32 count, char *d, u32 n)
{
	u32 o = 0;
	for (u32 i = 0; i < count && o + 4 < n; i++) {
		u8 s = get_septet(ud, len, bit + 7 * i);
		u32 c = gsm7[s];
		if (s == 0x1b && i + 1 < count) {
			u8 e = get_septet(ud, len, bit + 7 * ++i);
			c = 0xa0;                        /* unknown: a space */
			for (unsigned k = 0; k < sizeof(gsm7ext) / sizeof(gsm7ext[0]); k++)
				if (gsm7ext[k][0] == e) c = gsm7ext[k][1];
		}
		o += utf8_put(d + o, c);
	}
	d[o] = 0;
	return o;
}

static u32 ucs2_text(const u8 *p, u32 len, char *d, u32 n)
{
	u32 o = 0;
	for (u32 i = 0; i + 1 < len && o + 4 < n; i += 2) {
		u32 c = p[i] << 8 | p[i + 1];
		if (c >= 0xd800 && c < 0xdc00 && i + 3 < len) {
			u32 lo = p[i + 2] << 8 | p[i + 3];
			if (lo >= 0xdc00 && lo < 0xe000) {
				c = 0x10000 + ((c - 0xd800) << 10) + (lo - 0xdc00);
				i += 2;
			}
		}
		o += utf8_put(d + o, c);
	}
	d[o] = 0;
	return o;
}

/* an address: its length in digits (letters: semi-octets), type, value */
static int get_address(const u8 *p, u32 len, char *d, u32 n)
{
	if (len < 2) return -1;
	u32 digits = p[0], ton = (p[1] >> 4) & 7, octets = (digits + 1) / 2;
	if (len < 2 + octets) return -1;
	u32 o = 0;
	if (ton == 5) {                          /* alphanumeric: 7-bit letters */
		gsm7_text(p + 2, octets, 0, digits * 4 / 7, d, n);
		return 2 + octets;
	}
	if (ton == 1 && n > 1) d[o++] = '+';
	for (u32 i = 0; i < digits && o + 1 < n; i++) {
		u8 v = i & 1 ? p[2 + i / 2] >> 4 : p[2 + i / 2] & 0xf;
		static const char bcd[] = "0123456789*#abc";
		if (v == 0xf) break;
		d[o++] = bcd[v];
	}
	d[o] = 0;
	return 2 + octets;
}

static int bcd2(u8 v) { return (v & 0xf) * 10 + (v >> 4); }

static u32 days_from_civil(int y, int m, int d)
{
	y -= m <= 2;
	int era = y / 400, yoe = y - era * 400;
	int doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
	int doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
	return era * 146097 + doe - 719468;
}

static u32 scts(const u8 *p)
{
	int y = 2000 + bcd2(p[0]), mo = bcd2(p[1]), d = bcd2(p[2]);
	int h = bcd2(p[3]), mi = bcd2(p[4]), s = bcd2(p[5]);
	int tz = (p[6] & 7) * 10 + (p[6] >> 4);  /* quarter hours, the sign in bit 3 */
	if (p[6] & 8) tz = -tz;
	if (mo < 1 || mo > 12 || d < 1 || d > 31) return 0;
	s32 t = (s32)(days_from_civil(y, mo, d) * 86400 + h * 3600 + mi * 60 + s);
	return (u32)(t - tz * 15 * 60);
}

/* the user data: its length, the header (concatenation), the text */
static int user_data(const u8 *t, u32 n, u8 dcs, bool udhi, struct sms_part *p)
{
	if (n < 1) return -1;
	u32 udl = t[0], udn = n - 1;
	const u8 *ud = t + 1;
	int alphabet = 0;                        /* 0: 7-bit, 1: 8-bit, 2: UCS-2 */
	if ((dcs & 0xc0) == 0 || (dcs & 0xc0) == 0x40) alphabet = (dcs >> 2) & 3;
	else if ((dcs & 0xf0) == 0xf0) alphabet = (dcs >> 2) & 1;
	else if ((dcs & 0xf0) == 0xe0) alphabet = 2;
	if (alphabet == 3) alphabet = 1;
	u32 hdr = 0;                             /* the header's octets, its length octet included */
	if (udhi && udn) {
		hdr = 1 + ud[0];
		if (hdr > udn) return -1;
		for (u32 i = 1; i + 1 < hdr; ) {
			u8 iei = ud[i], iel = ud[i + 1];
			if (i + 2 + iel > hdr) break;
			const u8 *v = ud + i + 2;
			if (iei == 0 && iel == 3) { p->ref = v[0]; p->total = v[1]; p->seq = v[2]; }
			if (iei == 8 && iel == 4) { p->ref = v[0] << 8 | v[1]; p->total = v[2]; p->seq = v[3]; }
			i += 2 + iel;
		}
		if (p->total < 2 || !p->seq || p->seq > p->total) p->total = p->seq = 0;
	}
	if (alphabet == 0) {
		u32 skip = (hdr * 8 + 6) / 7;        /* the header's septets, the fill bits included */
		gsm7_text(ud, udn, skip * 7, udl > skip ? udl - skip : 0, p->text, sizeof(p->text));
	} else {
		u32 count = MIN(udl, udn);
		count = count > hdr ? count - hdr : 0;
		if (alphabet == 2) {
			ucs2_text(ud + hdr, count, p->text, sizeof(p->text));
		} else {                             /* 8-bit data: as Latin-1 */
			u32 o = 0;
			for (u32 i = 0; i < count && o + 4 < sizeof(p->text); i++)
				o += utf8_put(p->text + o, ud[hdr + i]);
			p->text[o] = 0;
		}
	}
	return 0;
}

int sms_decode(const u8 *pdu, u32 len, struct sms_part *p)
{
	mzero(p, sizeof(*p));
	if (len < 2 || len < 2u + pdu[0]) return -1;
	const u8 *t = pdu + 1 + pdu[0];
	u32 n = len - 1 - pdu[0], o = 1;
	u8 first = t[0], mti = first & 3;
	bool udhi = first & 0x40;
	int a;
	if (mti == 0) {                          /* SMS-DELIVER */
		a = get_address(t + o, n - o, p->number, sizeof(p->number));
		if (a < 0) return -1;
		o += a;
		if (o + 2 + 7 + 1 > n) return -1;
		u8 dcs = t[o + 1];
		p->time = scts(t + o + 2);
		o += 2 + 7;
		return user_data(t + o, n - o, dcs, udhi, p);
	}
	if (mti == 1) {                          /* SMS-SUBMIT: sent, stored */
		p->submit = true;
		o++;                                 /* the message reference */
		if (o >= n) return -1;
		a = get_address(t + o, n - o, p->number, sizeof(p->number));
		if (a < 0) return -1;
		o += a;
		u32 vpf = (first >> 3) & 3, vp = vpf == 2 ? 1 : vpf ? 7 : 0;
		if (o + 2 + vp + 1 > n) return -1;
		u8 dcs = t[o + 1];
		o += 2 + vp;
		return user_data(t + o, n - o, dcs, udhi, p);
	}
	return -1;                               /* a status report, a command */
}

static int put_address(u8 *p, const char *num)
{
	u32 nd = 0;
	bool intl = false;
	u8 digits[24];
	for (const char *s = num; *s && nd < 20; s++) {
		if (*s == '+' && !nd) intl = true;
		else if (*s >= '0' && *s <= '9') digits[nd++] = *s - '0';
		else if (*s == '*') digits[nd++] = 10;
		else if (*s == '#') digits[nd++] = 11;
	}
	p[0] = nd;
	p[1] = intl ? 0x91 : 0x81;
	for (u32 i = 0; i < (nd + 1) / 2; i++) {
		u8 lo = digits[2 * i], hi = 2 * i + 1 < nd ? digits[2 * i + 1] : 0xf;
		p[2 + i] = lo | hi << 4;
	}
	return 2 + (nd + 1) / 2;
}

/* the text's characters: code points, and as 7-bit septets (if it can) */
#define SMS_MAXCP 1200
static u32 cps[SMS_MAXCP];

int sms_encode(const char *number, const char *utf8, u8 ref, u8 out[][SMS_PDU_MAX], u32 *len, int max)
{
	u32 ncp = 0;
	for (const char *s = utf8; ncp < SMS_MAXCP; ) {
		u32 c = utf8_get(&s);
		if (!c) break;
		cps[ncp++] = c;
	}
	bool gsm = true;
	u32 units = 0;                           /* septets, or UTF-16 units */
	for (u32 i = 0; i < ncp; i++) {
		u8 sep[2];
		int k = gsm7_of(cps[i], sep);
		if (!k) gsm = false;
		units += k;
	}
	if (!gsm) {
		units = 0;
		for (u32 i = 0; i < ncp; i++) units += cps[i] >= 0x10000 ? 2 : 1;
	}
	u32 single = gsm ? 160 : 70, per = gsm ? 153 : 67;
	bool multi = units > single;
	int parts = 0;
	/* the parts' first characters */
	u32 start[16], ci = 0;
	do {
		if (parts == max || parts == 16) return 0;
		start[parts++] = ci;
		u32 room = multi ? per : single, used = 0;
		while (ci < ncp) {
			u8 sep[2];
			u32 k = gsm ? (u32)gsm7_of(cps[ci], sep) : cps[ci] >= 0x10000 ? 2u : 1u;
			if (used + k > room) break;
			used += k;
			ci++;
		}
	} while (ci < ncp);
	for (int pi = 0; pi < parts; pi++) {
		u8 *p = out[pi];
		u32 o = 0, end = pi + 1 < parts ? start[pi + 1] : ncp;
		p[o++] = 0x00;                       /* the default service centre */
		p[o++] = 0x01 | 0x10 | (multi ? 0x40 : 0);  /* SUBMIT, relative validity */
		p[o++] = 0x00;                       /* message reference: the modem's */
		o += put_address(p + o, number);
		p[o++] = 0x00;                       /* protocol */
		p[o++] = gsm ? 0x00 : 0x08;
		p[o++] = 0xa7;                       /* valid for 24 hours */
		u32 udl_at = o++;
		u8 *ud = p + o;
		mzero(ud, SMS_PDU_MAX - o);
		u32 hdr = 0;
		if (multi) {
			ud[0] = 5; ud[1] = 0; ud[2] = 3; ud[3] = ref; ud[4] = parts; ud[5] = pi + 1;
			hdr = 6;
		}
		if (gsm) {
			u32 skip = (hdr * 8 + 6) / 7, ns = 0;
			u8 sep[SMS_PDU_MAX];
			for (u32 i = start[pi]; i < end; i++) ns += gsm7_of(cps[i], sep + ns);
			put_septets(ud, skip * 7, sep, ns);
			p[udl_at] = skip + ns;
			o += ((skip + ns) * 7 + 7) / 8;
		} else {
			u32 b = hdr;
			for (u32 i = start[pi]; i < end; i++) {
				u32 c = cps[i];
				if (c >= 0x10000) {
					u32 v = c - 0x10000, hi = 0xd800 + (v >> 10), lo = 0xdc00 + (v & 0x3ff);
					ud[b++] = hi >> 8; ud[b++] = hi;
					ud[b++] = lo >> 8; ud[b++] = lo;
				} else {
					ud[b++] = c >> 8; ud[b++] = c;
				}
			}
			p[udl_at] = b;
			o += b;
		}
		len[pi] = o;
	}
	return parts;
}

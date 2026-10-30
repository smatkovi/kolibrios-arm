/* KolibriOS for ARM: FAT12, FAT16 and FAT32 with long names, read and
 * written. Written from Microsoft's FAT specification ("FAT32 File System
 * Specification", fatgen103) and the VFAT long name format. It is
 * KolibriOS's own file system here: the container (home.c) holds one,
 * and the N950's MyDocs (FAT32) is read with it.
 *
 * What it does:
 *   - the type by the count of clusters alone, as the specification says
 *     (fewer than 4085 FAT12, fewer than 65525 FAT16, else FAT32);
 *     sectors of 512 to 4096 bytes, any cluster size; every FAT written
 *     (FAT32 with mirroring off: only the active one); the FSInfo's free
 *     count and next free cluster kept as hints;
 *   - long names (checksummed: a mismatch shows the short name), short
 *     names in lower case by Windows NT's flags; names compare without
 *     regard to case (ASCII, Latin-1, Latin Extended-A, Greek, Cyrillic);
 *     paths are UTF-8, a byte that is not taken as cp866, as the short
 *     names' bytes are;
 *   - a new name is its 8.3 name when it is one (lower case by the NT
 *     flags), else a long name with a short name: the 8.3 name in upper
 *     case when only the case differs, else BASIS~N;
 *   - read, write (zeros in a gap), truncate, mkdir, remove, rename (to
 *     another folder too: ".." follows), attributes and times, sync, map
 *     (a file's clusters as byte ranges, for home.c).
 * Every loop is bounded and every cluster number checked: a broken image
 * gives E_FS (a cycle in a chain too), never a hang. A volume that is not
 * writable is never written, not even at mount.
 *
 * The order of the writes (the kernel may be reset between any two; a
 * vol_wr is on the disk when it returns):
 *   - a file grows: the new clusters are made a chain of their own (lost
 *     clusters if it stops there), linked to the file's last cluster, the
 *     data written, then the entry's size and first cluster. A stop after
 *     the link leaves the chain longer than the size (fsck: "truncating"),
 *     the data the size covers intact; a new file's first data appear with
 *     one write of its entry;
 *   - a file shrinks or goes: its entry first, then the new last cluster's
 *     end mark, then the rest freed: lost clusters at worst;
 *   - a FAT sector goes to the first FAT before the others (fsck takes the
 *     first when they differ);
 *   - a name's entries (long name parts and the short entry) are put into
 *     one sector when they fit in one (one write: there, or not); where
 *     they cannot, the long name parts are deleted before their short
 *     entry and written after it, so that no part is ever left without
 *     its short entry; a move to another place deletes the old short entry
 *     before it writes the new one: a stop between leaves lost clusters,
 *     never two entries on one chain;
 *   - a folder grows by a zeroed cluster linked to its end;
 *   - the free count is counted once (at the first allocation) and kept;
 *     the FSInfo is written by sync() and whenever it is off by more than
 *     1/32 of the clusters (a hint: fsck may find it wrong after a reset).
 * The FAT changes of one step are gathered in a window of the FAT and
 * written together, before anything that depends on them. */
#include "storage.h"

#define FREE_UNKNOWN 0xffffffffu
#define MAX_SLOTS 65536                /* a folder's entries (the spec's limit) */
#define NAME_UNITS 255                 /* a long name's UTF-16 units */
#define RUN_MAX 21                     /* 20 long name parts and the short entry */
#define WIN 4096                       /* the FAT window's bytes */
#define NONE 0xffffffffu

enum { IO_RD, IO_WR, IO_ZERO };
enum { T_CREATE = 1, T_WRITE = 2, T_ACCESS = 4 };

/* cp866, KolibriOS's code page: bytes 0x80-0xff */
static const u16 cp866[128] = {
	0x0410, 0x0411, 0x0412, 0x0413, 0x0414, 0x0415, 0x0416, 0x0417,
	0x0418, 0x0419, 0x041a, 0x041b, 0x041c, 0x041d, 0x041e, 0x041f,
	0x0420, 0x0421, 0x0422, 0x0423, 0x0424, 0x0425, 0x0426, 0x0427,
	0x0428, 0x0429, 0x042a, 0x042b, 0x042c, 0x042d, 0x042e, 0x042f,
	0x0430, 0x0431, 0x0432, 0x0433, 0x0434, 0x0435, 0x0436, 0x0437,
	0x0438, 0x0439, 0x043a, 0x043b, 0x043c, 0x043d, 0x043e, 0x043f,
	0x2591, 0x2592, 0x2593, 0x2502, 0x2524, 0x2561, 0x2562, 0x2556,
	0x2555, 0x2563, 0x2551, 0x2557, 0x255d, 0x255c, 0x255b, 0x2510,
	0x2514, 0x2534, 0x252c, 0x251c, 0x2500, 0x253c, 0x255e, 0x255f,
	0x255a, 0x2554, 0x2569, 0x2566, 0x2560, 0x2550, 0x256c, 0x2567,
	0x2568, 0x2564, 0x2565, 0x2559, 0x2558, 0x2552, 0x2553, 0x256b,
	0x256a, 0x2518, 0x250c, 0x2588, 0x2584, 0x258c, 0x2590, 0x2580,
	0x0440, 0x0441, 0x0442, 0x0443, 0x0444, 0x0445, 0x0446, 0x0447,
	0x0448, 0x0449, 0x044a, 0x044b, 0x044c, 0x044d, 0x044e, 0x044f,
	0x0401, 0x0451, 0x0404, 0x0454, 0x0407, 0x0457, 0x040e, 0x045e,
	0x00b0, 0x2219, 0x00b7, 0x221a, 0x2116, 0x00a4, 0x25a0, 0x00a0,
};

/* a long name part's 13 characters: their offsets in the entry */
static const u8 lfn_at[13] = { 1, 3, 5, 7, 9, 14, 16, 18, 20, 22, 24, 28, 30 };

/* a mounted volume */
struct fat {
	struct vol *v;
	int type;                      /* 12, 16, 32 */
	u32 bps, csize, cshift;        /* bytes a sector, a cluster, log2 of a cluster */
	u32 lga;                       /* log2 of the bytes a folder write keeps to */
	u32 nclus;                     /* data clusters: 2 .. nclus + 1 */
	u32 eoc, eoc_min;              /* the end mark written, the least one read */
	u32 nfats, rfat;               /* FATs; the one read (and the only one written without mirror) */
	bool mirror;
	u64 fat_off, fat_stride;       /* the first FAT's bytes from the volume's start, a FAT's bytes */
	u32 fat_len;                   /* the bytes of a FAT the window reads */
	u64 root_off;                  /* FAT12/16: the root folder, its entries */
	u32 root_slots;
	u32 root_cl;                   /* FAT32: the root's first cluster */
	u64 data_off;                  /* cluster 2 */
	u32 fsi;                       /* FAT32: the FSInfo's sector (0: none) */
	u32 free, next;                /* free clusters (FREE_UNKNOWN), where the search starts */
	u32 fsi_free, fsi_next;        /* what the FSInfo has */
	bool counted;                  /* free counted from the FAT: exact */
	/* the FAT window: bytes ws.. of the FAT, the changed ones dlo..dhi-1 */
	u32 ws, wlen, dlo, dhi;
	bool wok;
	/* the chain cursor: cluster ccl is number cidx of the chain from cfirst */
	u32 cfirst, cidx, ccl;
	u8 win[WIN + 4];
};

/* an entry found in a folder (n 0: the root, which has none) */
struct dent {
	u32 d;                         /* the folder it is in (its cluster, 0 the FAT12/16 root) */
	u32 idx;                       /* the short entry's slot */
	u32 n;                         /* slots: the long name's parts and the short entry */
	u64 off;                       /* the short entry's bytes on the volume */
	u8 raw[32];                    /* the short entry */
};

/* slots idx..idx+n-1 of folder d; pad..idx-1 (its end) made deleted before */
struct run { u32 d, idx, n, pad; };
struct slot { u64 off; u8 b[32]; };

/* per operation (one at a time) */
static u8 zbuf[8192];                  /* zeros */
static u8 sbuf[4096];                  /* a sector's bytes */
static u16 lfn[20 * 13 + 1];           /* the long name dir_scan puts together */
static u16 nbuf[NAME_UNITS + 2];       /* a new name */
static u8 ents[RUN_MAX][32];           /* a new name's entries */
static u8 taken[MAX_SLOTS / 8];        /* the numeric tails in use */
static struct slot s_old[RUN_MAX], s_new[RUN_MAX], s_pad[128], s_tmp[2 * RUN_MAX + 128];
static struct vdirent rde;

static inline bool ok_cl(const struct fat *f, u32 c) { return c >= 2 && c <= f->nclus + 1; }
static inline u64 cl_off(const struct fat *f, u32 c) { return f->data_off + ((u64)(c - 2) << f->cshift); }
static inline u32 root_id(const struct fat *f) { return f->type == 32 ? f->root_cl : 0; }

static u32 lg2(u32 x)
{
	u32 n = 0;
	while (x > 1) x >>= 1, n++;
	return n;
}

/* --- characters ---------------------------------------------------------- */

static u16 up(u16 c)
{
	if (c < 0x61) return c;
	if (c <= 0x7a) return c - 0x20;
	if (c < 0xe0) return c;
	if (c <= 0xfe) return c == 0xf7 ? c : c - 0x20;
	if (c == 0xff) return 0x178;
	if (c < 0x180) {
		if (c == 0x131 || c == 0x138 || c == 0x149 || c == 0x17f) return c;
		if ((c >= 0x139 && c <= 0x148) || (c >= 0x179 && c <= 0x17e)) return c & 1 ? c : c - 1;
		return c & 1 ? c - 1 : c;
	}
	if (c == 0x3c2) return 0x3a3;
	if (c >= 0x3b1 && c <= 0x3cb) return c - 0x20;
	if (c >= 0x430 && c <= 0x44f) return c - 0x20;
	if (c >= 0x450 && c <= 0x45f) return c - 0x50;
	if ((c >= 0x460 && c <= 0x481) || (c >= 0x48a && c <= 0x4bf) || (c >= 0x4d0 && c <= 0x52f))
		return c & 1 ? c - 1 : c;
	if (c >= 0x4c1 && c <= 0x4ce) return c & 1 ? c : c - 1;
	return c;
}

static bool name_eq(const u16 *a, u32 na, const u16 *b, u32 nb)
{
	if (na != nb) return false;
	for (u32 i = 0; i < na; i++)
		if (a[i] != b[i] && up(a[i]) != up(b[i])) return false;
	return true;
}

/* a short name's byte */
static u16 oem(u8 c, bool lower)
{
	if (c >= 0x80) return cp866[c - 0x80];
	return lower && c >= 'A' && c <= 'Z' ? c + 0x20 : c;
}

/* a path's name: UTF-8, a byte that is not cp866; E_NOT_FOUND if longer than a long name */
static int name_decode(const char *p, u32 len, u16 *o, u32 *n)
{
	const u8 *s = (const u8 *)p;
	u32 k = 0;
	for (u32 i = 0; i < len;) {
		u32 c = s[i], l = 1;
		if (c >= 0x80) {
			u32 cp = 0;
			l = c >= 0xc2 && c <= 0xdf ? 2 : c >= 0xe0 && c <= 0xef ? 3 : c >= 0xf0 && c <= 0xf4 ? 4 : 0;
			bool ok = l && i + l <= len;
			if (ok) cp = c & (0x7f >> l);
			for (u32 j = 1; ok && j < l; j++) {
				if ((s[i + j] & 0xc0) != 0x80) ok = false;
				cp = cp << 6 | (s[i + j] & 0x3f);
			}
			if (ok && ((l == 3 && cp < 0x800) || (l == 4 && (cp < 0x10000 || cp > 0x10ffff)) ||
				   (cp >= 0xd800 && cp <= 0xdfff)))
				ok = false;
			if (ok) c = cp;
			else c = cp866[c - 0x80], l = 1;
		}
		i += l;
		if (k + (c >= 0x10000 ? 2 : 1) > NAME_UNITS) return E_NOT_FOUND;
		if (c >= 0x10000) {
			o[k++] = 0xd800 + ((c - 0x10000) >> 10);
			o[k++] = 0xdc00 + (c & 0x3ff);
		} else o[k++] = c;
	}
	*n = k;
	return E_OK;
}

static u32 utf8_put(char *o, u32 k, u32 size, u32 c)
{
	u32 l = c < 0x80 ? 1 : c < 0x800 ? 2 : c < 0x10000 ? 3 : 4;
	if (k + l >= size) return k;
	if (l == 1) o[k] = c;
	else if (l == 2) o[k] = 0xc0 | c >> 6;
	else if (l == 3) o[k] = 0xe0 | c >> 12;
	else o[k] = 0xf0 | c >> 18;
	for (u32 i = 1; i < l; i++) o[k + i] = 0x80 | ((c >> (6 * (l - 1 - i))) & 0x3f);
	return k + l;
}

static void to_utf8(const u16 *s, u32 n, char *o, u32 size)
{
	u32 k = 0;
	for (u32 i = 0; i < n; i++) {
		u32 c = s[i];
		if (c >= 0xd800 && c <= 0xdbff && i + 1 < n && s[i + 1] >= 0xdc00 && s[i + 1] <= 0xdfff)
			c = 0x10000 + ((c - 0xd800) << 10) + (s[++i] - 0xdc00);
		else if (c >= 0xd800 && c <= 0xdfff) c = 0xfffd;
		k = utf8_put(o, k, size, c);
	}
	o[k] = 0;
}

/* a short entry's name ("NAME.EXT"), lower case where the NT flags say if nt */
static u32 sfn_text(const u8 *r, u16 *o, bool nt)
{
	u32 n = 0, nb = 8, ne = 3;
	while (nb && r[nb - 1] == ' ') nb--;
	while (ne && r[8 + ne - 1] == ' ') ne--;
	for (u32 i = 0; i < nb; i++) o[n++] = oem(i == 0 && r[0] == 0x05 ? 0xe5 : r[i], nt && (r[12] & 0x08));
	if (ne) {
		o[n++] = '.';
		for (u32 i = 0; i < ne; i++) o[n++] = oem(r[8 + i], nt && (r[12] & 0x10));
	}
	return n;
}

/* may a file have this name? */
static bool name_ok(const u16 *s, u32 n)
{
	if (!n || n > NAME_UNITS) return false;
	for (u32 i = 0; i < n; i++) {
		u16 c = s[i];
		if (c < 0x20 || c == '"' || c == '*' || c == '/' || c == ':' || c == '<' || c == '>' ||
		    c == '?' || c == '\\' || c == '|')
			return false;
	}
	return s[n - 1] != '.' && s[n - 1] != ' ';
}

/* a character a short name may have (upper case) */
static bool sfn_char(u32 c)
{
	if ((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9')) return true;
	for (const char *p = "!#$%&'()-@^_`{}~"; *p; p++)
		if (c == (u8)*p) return true;
	return false;
}

static u8 basis_char(u32 c)
{
	if (c >= 'a' && c <= 'z') c -= 0x20;
	return sfn_char(c) ? c : '_';
}

/* the short name for a new name into sfn[0..10]: 2 it is one (*nt the
 * lower case flags, no long name), 1 it is one but for its case (a long
 * name too), 0 a basis that wants a tail ~N (*nb its base's length) */
static int sfn_make(const u16 *s, u32 n, u8 *sfn, u8 *nt, u32 *nb)
{
	memset(sfn, ' ', 11);
	*nt = 0;
	u32 dot = n, dots = 0;
	for (u32 i = 0; i < n; i++)
		if (s[i] == '.') dot = i, dots++;
	u32 bl = dot, el = dots ? n - dot - 1 : 0;
	bool fit = dots <= 1 && bl >= 1 && bl <= 8 && el <= 3 && (!dots || el);
	bool bu = false, blw = false, eu = false, elw = false;
	for (u32 i = 0; fit && i < n; i++) {
		if (i == dot) continue;
		u32 c = s[i];
		bool lw = c >= 'a' && c <= 'z', uc = c >= 'A' && c <= 'Z';
		if (!sfn_char(lw ? c - 0x20 : c)) fit = false;
		if (i < bl) bu |= uc, blw |= lw;
		else eu |= uc, elw |= lw;
	}
	if (fit) {
		for (u32 i = 0; i < bl; i++) sfn[i] = up(s[i]);
		for (u32 i = 0; i < el; i++) sfn[8 + i] = up(s[dot + 1 + i]);
		*nb = bl;
		if ((bu && blw) || (eu && elw)) return 1;
		*nt = (blw ? 0x08 : 0) | (elw ? 0x10 : 0);
		return 2;
	}
	/* the basis: no leading periods, no spaces, no periods but the last */
	u32 b0 = 0, last = n, k = 0, m = 0;
	while (b0 < n && (s[b0] == '.' || s[b0] == ' ')) b0++;
	for (u32 i = b0; i < n; i++)
		if (s[i] == '.') last = i;
	for (u32 i = b0; i < last && k < 8; i++)
		if (s[i] != '.' && s[i] != ' ') sfn[k++] = basis_char(s[i]);
	if (!k) sfn[k++] = '_';
	for (u32 i = last + 1; i < n && m < 3; i++)
		if (s[i] != ' ') sfn[8 + m++] = basis_char(s[i]);
	*nb = k;
	return 0;
}

static u8 sfn_sum(const u8 *s)
{
	u8 c = 0;
	for (int i = 0; i < 11; i++) c = (u8)(((c & 1) << 7) + (c >> 1) + s[i]);
	return c;
}

/* --- times: FAT's local date and time as Unix seconds (taken as UTC) ----- */

static u32 days_from_civil(u32 y, u32 m, u32 d)
{
	y -= m <= 2;
	u32 era = y / 400, yoe = y - era * 400;
	u32 doy = (153 * (m > 2 ? m - 3 : m + 9) + 2) / 5 + d - 1;
	u32 doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
	return era * 146097 + doe - 719468;
}

static u32 unix_time(u16 date, u16 time, u8 tenth)
{
	u32 m = date >> 5 & 15, d = date & 31;
	if (!date || m < 1 || m > 12 || !d) return 0;
	u64 t = (u64)days_from_civil(1980 + (date >> 9), m, d) * 86400 + (time >> 11) * 3600 +
		(time >> 5 & 63) * 60 + (time & 31) * 2 + (tenth < 200 ? tenth / 100 : 0);
	return t > 0xffffffffu ? 0xffffffffu : (u32)t;
}

static void fat_time(u32 t, u16 *date, u16 *time, u8 *tenth)
{
	u32 z = t / 86400 + 719468, s = t % 86400;
	u32 era = z / 146097, doe = z - era * 146097;
	u32 yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
	u32 doy = doe - (365 * yoe + yoe / 4 - yoe / 100), mp = (5 * doy + 2) / 153;
	u32 d = doy - (153 * mp + 2) / 5 + 1, m = mp < 10 ? mp + 3 : mp - 9;
	u32 y = yoe + era * 400 + (m <= 2);
	if (y < 1980) { *date = 1 << 5 | 1; *time = 0; *tenth = 0; return; }
	if (y > 2107) { *date = 127 << 9 | 12 << 5 | 31; *time = 23 << 11 | 59 << 5 | 29; *tenth = 100; return; }
	*date = (y - 1980) << 9 | m << 5 | d;
	*time = (s / 3600) << 11 | (s / 60 % 60) << 5 | (s % 60) / 2;
	*tenth = (s & 1) * 100;
}

static void stamp(u8 *r, u32 t, int what)
{
	u16 date, time;
	u8 tenth;
	fat_time(t, &date, &time, &tenth);
	if (what & T_CREATE) r[13] = tenth, put_le16(r + 14, time), put_le16(r + 16, date);
	if (what & T_WRITE) put_le16(r + 22, time), put_le16(r + 24, date);
	if (what & T_ACCESS) put_le16(r + 18, date);
}

/* --- the FAT ------------------------------------------------------------- */

/* after a failed write: what is in memory is dropped, read again from the disk */
static void fat_fail(struct fat *f)
{
	f->wok = false;
	f->dlo = f->dhi = 0;
	f->cfirst = 0;
	f->counted = false;
}

static int wr(struct fat *f, u64 off, u32 len, const void *b)
{
	int e = vol_wr(f->v, off, len, b);
	if (e) fat_fail(f);
	return e;
}

static int wr_zeros(struct fat *f, u64 off, u64 n)
{
	while (n) {
		u32 k = MIN(n, sizeof(zbuf));
		int e = wr(f, off, k, zbuf);
		if (e) return e;
		off += k, n -= k;
	}
	return E_OK;
}

/* the window's changes to the FAT (the first first) */
static int win_flush(struct fat *f)
{
	if (!f->wok || f->dlo >= f->dhi) return E_OK;
	u32 lo = f->dlo, hi = f->dhi;
	f->dlo = f->dhi = 0;
	for (u32 i = 0; i < (f->mirror ? f->nfats : 1); i++) {
		u32 k = f->mirror ? i : f->rfat;
		int e = wr(f, f->fat_off + k * f->fat_stride + f->ws + lo, hi - lo, f->win + lo);
		if (e) return e;
	}
	return E_OK;
}

/* the window over the FAT's byte o (and the next 3) */
static int win_load(struct fat *f, u32 o)
{
	u32 ws = o & ~(WIN - 1);
	if (f->wok && f->ws == ws) return E_OK;
	int e = win_flush(f);
	if (e) return e;
	f->wok = false;
	u32 len = MIN(WIN + 2, f->fat_len - ws);
	e = vol_rd(f->v, f->fat_off + f->rfat * f->fat_stride + ws, len, f->win);
	if (e) return e;
	f->ws = ws, f->wlen = len, f->wok = true;
	return E_OK;
}

static u32 fat_pos(const struct fat *f, u32 cl)
{
	return f->type == 12 ? cl + cl / 2 : f->type == 16 ? cl * 2 : cl * 4;
}

static int fat_get(struct fat *f, u32 cl, u32 *val)
{
	u32 o = fat_pos(f, cl);
	int e = win_load(f, o);
	if (e) return e;
	const u8 *p = f->win + (o - f->ws);
	if (f->type == 12) *val = cl & 1 ? le16(p) >> 4 : le16(p) & 0xfff;
	else if (f->type == 16) *val = le16(p);
	else *val = le32(p) & 0x0fffffff;
	return E_OK;
}

static int fat_set(struct fat *f, u32 cl, u32 val)
{
	u32 o = fat_pos(f, cl);
	int e = win_load(f, o);
	if (e) return e;
	u32 r = o - f->ws, nb = f->type == 32 ? 4 : 2;
	u8 *p = f->win + r;
	if (f->type == 12) {
		u32 w = le16(p);
		put_le16(p, cl & 1 ? (w & 0x000f) | (val << 4) : (w & 0xf000) | (val & 0xfff));
	} else if (f->type == 16) put_le16(p, val);
	else put_le32(p, (le32(p) & 0xf0000000) | (val & 0x0fffffff));
	if (f->dlo >= f->dhi) f->dlo = r, f->dhi = r + nb;
	else f->dlo = MIN(f->dlo, r), f->dhi = MAX(f->dhi, r + nb);
	return E_OK;
}

/* the cluster after cl: E_OK (*n), E_EOF (cl is the last), E_FS (free, bad, out of range) */
static int fat_next(struct fat *f, u32 cl, u32 *n)
{
	u32 v;
	int e = fat_get(f, cl, &v);
	if (e) return e;
	if (v >= f->eoc_min) return E_EOF;
	if (!ok_cl(f, v)) return E_FS;
	*n = v;
	return E_OK;
}

/* cluster k of the chain from first (*cl); E_EOF if it has fewer (*cl its
 * last, *len its length); a chain longer than the clusters is a cycle */
static int chain_seek(struct fat *f, u32 first, u32 k, u32 *cl, u32 *len)
{
	if (!ok_cl(f, first)) return E_FS;
	u32 i = 0, c = first;
	if (f->cfirst == first && f->cidx <= k) i = f->cidx, c = f->ccl;
	int e = E_OK;
	while (i < k) {
		u32 n;
		if ((e = fat_next(f, c, &n))) break;
		if (++i > f->nclus) return E_FS;
		c = n;
	}
	if (e && e != E_EOF) return e;
	f->cfirst = first, f->cidx = i, f->ccl = c;
	*cl = c;
	if (len) *len = i + 1;
	return e;
}

static int chain_end(struct fat *f, u32 first, u32 *last, u32 *len)
{
	int e = chain_seek(f, first, NONE, last, len);
	return e == E_EOF ? E_OK : e ? e : E_FS;
}

static int count_free(struct fat *f)
{
	if (f->counted) return E_OK;
	u32 n = 0;
	for (u32 c = 2; c <= f->nclus + 1; c++) {
		u32 v;
		int e = fat_get(f, c, &v);
		if (e) return e;
		n += !v;
	}
	f->free = n, f->counted = true;
	return E_OK;
}

static void fsi_write(struct fat *f)
{
	if (!f->fsi || !f->v->writable || !f->counted || (f->free == f->fsi_free && f->next == f->fsi_next)) return;
	u8 b[8];
	put_le32(b, f->free);
	put_le32(b + 4, f->next);
	if (wr(f, (u64)f->fsi * f->bps + 488, 8, b)) return;
	f->fsi_free = f->free, f->fsi_next = f->next;
}

/* the FSInfo when it is far off */
static void fsi_check(struct fat *f)
{
	u32 d = f->free > f->fsi_free ? f->free - f->fsi_free : f->fsi_free - f->free;
	if (d > MAX(f->nclus >> 5, 64)) fsi_write(f);
}

/* up to want free clusters made a chain (*first, *got): E_DISK_FULL with fewer */
static int chain_alloc(struct fat *f, u32 want, u32 *first, u32 *got)
{
	*first = 0, *got = 0;
	int e = count_free(f);
	if (e) return e;
	u32 c = ok_cl(f, f->next) ? f->next : 2, prev = 0;
	for (u32 seen = 0; *got < want && f->free && seen < f->nclus; seen++) {
		u32 v;
		if ((e = fat_get(f, c, &v))) return e;
		if (!v) {
			if ((e = fat_set(f, c, f->eoc))) return e;
			if (prev && (e = fat_set(f, prev, c))) return e;
			if (!prev) *first = c;
			prev = c, (*got)++, f->free--;
		}
		if (++c > f->nclus + 1) c = 2;
	}
	f->next = c;
	if ((e = win_flush(f))) return e;
	fsi_check(f);
	return *got < want ? E_DISK_FULL : E_OK;
}

/* the chain from first freed (as far as it is a chain) */
static int chain_free(struct fat *f, u32 first)
{
	f->cfirst = 0;
	int e = count_free(f);
	if (e) return e;
	u32 c = first;
	for (u32 i = 0; i <= f->nclus && ok_cl(f, c); i++) {
		u32 v;
		if ((e = fat_get(f, c, &v))) return e;
		if (!v) break;                 /* free already: a cycle, a cross link */
		if ((e = fat_set(f, c, 0))) return e;
		f->free++;
		c = v < f->eoc_min ? v : 0;
	}
	if ((e = win_flush(f))) return e;
	fsi_check(f);
	return E_OK;
}

/* n bytes at off of the file's data, the chain from first (long enough):
 * read into buf, written from it, or zeros written; contiguous clusters
 * in one */
static int file_io(struct fat *f, u32 first, u64 off, u64 n, u8 *buf, int how)
{
	while (n) {
		u32 ci = off >> f->cshift, cl, last;
		int e = chain_seek(f, first, ci, &cl, NULL);
		if (e) return e == E_EOF ? E_FS : e;
		u32 in = off & (f->csize - 1);
		u64 run = f->csize - in;
		for (last = cl; run < n && run < (1u << 30);) {
			u32 nx;
			e = fat_next(f, last, &nx);
			if (e) return e == E_EOF ? E_FS : e;
			f->cidx++, f->ccl = nx;
			if (nx != last + 1) break;
			last = nx, run += f->csize;
		}
		u32 k = MIN(run, n);
		if (k > (1u << 30)) k = 1u << 30;
		u64 at = cl_off(f, cl) + in;
		if (how == IO_RD) e = vol_rd(f->v, at, k, buf);
		else if (how == IO_WR) e = wr(f, at, k, buf);
		else e = wr_zeros(f, at, k);
		if (e) return e;
		off += k, n -= k;
		if (buf) buf += k;
	}
	return E_OK;
}

/* an entry point's end: the window is written (it is but for an error) */
static int finish(struct fat *f, int e)
{
	if (f->dlo < f->dhi) {
		if (e == E_OK || e == E_DISK_FULL) {
			int x = win_flush(f);
			if (x) e = x;
		} else fat_fail(f);
	}
	return e;
}

/* --- folders -------------------------------------------------------------- */

/* the cursor in a folder's chain: cluster number ci is cl */
struct dcur { u32 d, ci, cl; };

/* slot idx's bytes; E_EOF past the folder's end */
static int slot_off(struct fat *f, struct dcur *k, u32 idx, u64 *off)
{
	if (!k->d) {
		if (idx >= f->root_slots) return E_EOF;
		*off = f->root_off + (u64)idx * 32;
		return E_OK;
	}
	if (idx >= MAX_SLOTS) return E_EOF;
	u32 b = idx * 32, ci = b >> f->cshift;
	if (!k->cl || ci < k->ci) {
		if (!ok_cl(f, k->d)) return E_FS;
		k->ci = 0, k->cl = k->d;
	}
	while (k->ci < ci) {
		u32 n;
		int e = fat_next(f, k->cl, &n);
		if (e) return e;
		k->cl = n, k->ci++;
	}
	*off = cl_off(f, k->cl) + (b & (f->csize - 1));
	return E_OK;
}

static u32 de_first(const struct fat *f, const struct dent *de)
{
	if (!de->n) return root_id(f);
	u32 c = le16(de->raw + 26);
	if (f->type == 32) c |= (u32)le16(de->raw + 20) << 16;
	return c;
}

static void set_first(const struct fat *f, u8 *r, u32 c)
{
	put_le16(r + 26, c);
	put_le16(r + 20, f->type == 32 ? c >> 16 : 0);
}

static bool skip_entry(const u8 *r) { return r[0] == '.' || (r[11] & A_LABEL); }

/* fn for each entry of folder d (not the deleted ones, not the long name
 * parts; with its long name if it has one): fn's 0 goes on, anything else
 * stops the scan and is returned. fn may not scan a folder itself */
typedef int (*scan_fn)(struct fat *f, struct dent *de, const u16 *ln, u32 nl, void *ctx);

static void lfn_get(u16 *o, const u8 *b)
{
	for (int j = 0; j < 13; j++) o[j] = le16(b + lfn_at[j]);
}

static int dir_scan(struct fat *f, u32 d, scan_fn fn, void *ctx)
{
	struct dcur k = { d, 0, 0 };
	struct dent de;
	u32 parts = 0, want = 0, start = 0;
	u8 sum = 0;
	de.d = d;
	for (u32 i = 0;; i++) {
		int e = slot_off(f, &k, i, &de.off);
		if (e) return e == E_EOF ? E_OK : e;
		u8 *b = de.raw;
		if ((e = vol_rd(f->v, de.off, 32, b))) return e;
		if (!b[0]) return E_OK;
		if (b[0] == 0xe5) { parts = 0; continue; }
		if ((b[11] & 0x3f) == 0x0f) {
			u32 ord = b[0] & 0x1f;
			if (b[0] & 0x40) {
				if (!ord || ord > 20) { parts = 0; continue; }
				parts = want = ord, sum = b[13], start = i;
			} else if (!parts || ord != want || b[13] != sum) {
				parts = 0;
				continue;
			}
			lfn_get(lfn + (ord - 1) * 13, b);
			want--;
			continue;
		}
		u32 nl = 0;
		if (parts && !want && start + parts == i && sfn_sum(b) == sum) {
			while (nl < parts * 13 && lfn[nl]) nl++;
			if (nl > NAME_UNITS) nl = 0;
		}
		de.idx = i, de.n = nl ? parts + 1 : 1;
		parts = 0;
		if ((e = fn(f, &de, lfn, nl, ctx))) return e;
	}
}

/* the entry of a name in a folder */
struct look { const u16 *name; u32 n; struct dent *de; };

static int look_fn(struct fat *f, struct dent *de, const u16 *ln, u32 nl, void *ctx)
{
	struct look *l = ctx;
	if (skip_entry(de->raw)) return 0;
	u16 s[12];
	if (!(nl && name_eq(ln, nl, l->name, l->n)) && !name_eq(s, sfn_text(de->raw, s, false), l->name, l->n))
		return 0;
	*l->de = *de;
	return 1;
}

/* the folder an entry is (its cluster; 0 the FAT12/16 root); E_NOT_FOUND for a file */
static int dir_of(struct fat *f, const struct dent *de, u32 *d)
{
	if (!de->n) { *d = root_id(f); return E_OK; }
	if (!(de->raw[11] & A_DIR)) return E_NOT_FOUND;
	*d = de_first(f, de);
	return ok_cl(f, *d) ? E_OK : E_FS;
}

/* the entry the first len bytes of path name (de->n 0: the root); avoid a
 * folder the path may not go through (E_ACCESS) */
static int resolve(struct fat *f, const char *p, u32 len, u32 avoid, struct dent *de)
{
	memset(de, 0, sizeof(*de));
	for (u32 i = 0; i < len;) {
		u32 j = i, d, n;
		while (j < len && p[j] != '/') j++;
		int e = dir_of(f, de, &d);
		if (e) return e;
		if (avoid && d == avoid) return E_ACCESS;
		u16 nm[NAME_UNITS + 2];
		if (j == i || name_decode(p + i, j - i, nm, &n)) return E_NOT_FOUND;
		struct look l = { nm, n, de };
		e = dir_scan(f, d, look_fn, &l);
		if (e != 1) return e ? e : E_NOT_FOUND;
		i = j + 1;
	}
	return E_OK;
}

static void fill(const u8 *r, const u16 *ln, u32 nl, struct vdirent *e)
{
	u16 s[12];
	if (nl) to_utf8(ln, nl, e->name, sizeof(e->name));
	else to_utf8(s, sfn_text(r, s, true), e->name, sizeof(e->name));
	e->attr = r[11] & (A_RO | A_HIDDEN | A_SYSTEM | A_DIR | A_ARCH);
	e->size = r[11] & A_DIR ? 0 : le32(r + 28);
	e->ctime = unix_time(le16(r + 16), le16(r + 14), r[13]);
	e->atime = unix_time(le16(r + 18), 0, 0);
	e->mtime = unix_time(le16(r + 24), le16(r + 22), 0);
}

/* the numbers of BASIS~N in a folder */
struct tails { const u8 *sfn; u32 nb; u64 skip; };

static int tail_fn(struct fat *f, struct dent *de, const u16 *ln, u32 nl, void *ctx)
{
	struct tails *t = ctx;
	const u8 *r = de->raw;
	if (de->off == t->skip || memcmp(r + 8, t->sfn + 8, 3)) return 0;
	u32 q = 8, p, num = 0;
	while (q && r[q - 1] == ' ') q--;
	p = q;
	while (p && r[p - 1] >= '0' && r[p - 1] <= '9') p--;
	u32 d = q - p;
	if (!d || d > 5 || p < 2 || r[p - 1] != '~' || r[p] == '0') return 0;
	for (u32 i = p; i < q; i++) num = num * 10 + r[i] - '0';
	u32 pl = MIN(t->nb, 7 - d);
	if (num >= MAX_SLOTS || p - 1 != pl || memcmp(r, t->sfn, pl)) return 0;
	taken[num >> 3] |= 1 << (num & 7);
	return 0;
}

/* the basis in sfn given its least free tail in folder d (skip: an entry not counted) */
static int sfn_tail(struct fat *f, u32 d, u8 *sfn, u32 nb, u64 skip)
{
	memset(taken, 0, sizeof(taken));
	struct tails t = { sfn, nb, skip };
	int e = dir_scan(f, d, tail_fn, &t);
	if (e) return e;
	for (u32 num = 1; num < MAX_SLOTS; num++) {
		if (taken[num >> 3] & 1 << (num & 7)) continue;
		u8 dg[6];
		u32 dn = 0;
		for (u32 x = num; x; x /= 10) dg[dn++] = '0' + x % 10;
		u32 pl = MIN(nb, 7 - dn);
		memset(sfn + pl, ' ', 8 - pl);
		sfn[pl] = '~';
		for (u32 i = 0; i < dn; i++) sfn[pl + 1 + i] = dg[dn - 1 - i];
		return E_OK;
	}
	return E_DISK_FULL;
}

/* a new name's entries into ents: the long name parts (if lfn), then sfe */
static u32 make_run(const u16 *s, u32 n, bool ln, const u8 *sfe)
{
	u32 nl = ln ? (n + 12) / 13 : 0;
	u8 sum = sfn_sum(sfe);
	for (u32 i = 0; i < nl; i++) {
		u8 *b = ents[i];
		u32 ord = nl - i;
		memset(b, 0, 32);
		b[0] = ord | (i ? 0 : 0x40);
		b[11] = 0x0f;
		b[13] = sum;
		for (u32 j = 0; j < 13; j++) {
			u32 p = (ord - 1) * 13 + j;
			put_le16(b + lfn_at[j], p < n ? s[p] : p == n ? 0 : 0xffff);
		}
	}
	memcpy(ents[nl], sfe, 32);
	return nl + 1;
}

/* folder d (slots before: total) one cluster longer, zeroed */
static int dir_grow(struct fat *f, u32 d, u32 total)
{
	if (!d || total + (f->csize >> 5) > MAX_SLOTS) return E_DISK_FULL;
	u32 last, len, c, got;
	int e = chain_end(f, d, &last, &len);
	if (!e) e = chain_alloc(f, 1, &c, &got);
	if (!e) e = wr_zeros(f, cl_off(f, c), f->csize);
	if (!e) e = fat_set(f, last, c);
	if (!e) e = win_flush(f);
	return e;
}

/* place for n slots in folder d: within one sector if they fit in one
 * (deleted ones, else at the end, else in a new cluster), else anywhere */
static int find_slots(struct fat *f, u32 d, u32 n, struct run *r)
{
	u32 lgper = f->lga - 5, per = 1u << lgper;
	r->d = d, r->n = n;
	for (int tries = 0; tries < 4; tries++) {
		struct dcur k = { d, 0, 0 };
		u32 end = NONE, run = 0, any = NONE, total = 0;
		bool dirty = false;
		for (u32 i = 0;; i++) {
			u64 off;
			u8 b0;
			int e = slot_off(f, &k, i, &off);
			if (e == E_EOF) { total = i; break; }
			if (e || (e = vol_rd(f->v, off, 1, &b0))) return e;
			if (end != NONE) { dirty |= b0 != 0; continue; }
			if (!b0) { end = i; continue; }
			if (b0 != 0xe5) { run = 0; continue; }
			if (++run < n) continue;
			u32 s = i + 1 - n;
			if (n <= per && s >> lgper == i >> lgper) { r->idx = r->pad = s; return E_OK; }
			if (any == NONE) any = s;
		}
		if (end == NONE) end = total;
		u32 s = end;
		if (n <= per && s >> lgper != (s + n - 1) >> lgper) s = ((s >> lgper) + 1) << lgper;
		if (s + n <= total) {
			if (dirty) {                   /* what follows the end mark: zeros */
				struct dcur z = { d, 0, 0 };
				for (u32 i = end + 1; i < total; i++) {
					u64 off;
					u8 b0;
					int e = slot_off(f, &z, i, &off);
					if (!e) e = vol_rd(f->v, off, 1, &b0);
					if (!e && b0) e = wr(f, off, 1, zbuf);
					if (e) return e;
				}
			}
			r->idx = s, r->pad = end;
			return E_OK;
		}
		if (n > per && any != NONE) { r->idx = r->pad = any; return E_OK; }
		int e = dir_grow(f, d, total);
		if (e == E_DISK_FULL && any != NONE) { r->idx = r->pad = any; return E_OK; }
		if (e) return e;
	}
	return E_FS;
}

/* slots written as they are: neighbours in one sector together */
static int put_slots(struct fat *f, const struct slot *s, u32 n)
{
	for (u32 i = 0; i < n;) {
		u32 j = i + 1;
		while (j < n && s[j].off == s[j - 1].off + 32 && s[j].off >> f->lga == s[i].off >> f->lga) j++;
		for (u32 x = i; x < j; x++) memcpy(sbuf + (x - i) * 32, s[x].b, 32);
		int e = wr(f, s[i].off, (j - i) * 32, sbuf);
		if (e) return e;
		i = j;
	}
	return E_OK;
}

static bool in_one(const struct fat *f, const struct slot *s, u32 n)
{
	for (u32 i = 1; i < n; i++)
		if (s[i].off >> f->lga != s[0].off >> f->lga) return false;
	return true;
}

/* slots of one sector in one write (later ones over earlier ones) */
static int put_span(struct fat *f, const struct slot *s, u32 n)
{
	u64 lo = s[0].off, hi = s[0].off;
	for (u32 i = 1; i < n; i++) lo = MIN(lo, s[i].off), hi = MAX(hi, s[i].off);
	hi += 32;
	int e = vol_rd(f->v, lo, hi - lo, sbuf);
	if (e) return e;
	for (u32 i = 0; i < n; i++) memcpy(sbuf + (s[i].off - lo), s[i].b, 32);
	return wr(f, lo, hi - lo, sbuf);
}

static u32 gather(const struct slot *s, u32 n, u32 at)
{
	memcpy(s_tmp + at, s, n * sizeof(*s));
	return at + n;
}

static bool among(const struct slot *s, u32 n, u64 off)
{
	for (u32 i = 0; i < n; i++)
		if (s[i].off == off) return true;
	return false;
}

/* a name's entries deleted (old), written (nw: from ents), or both, in an
 * order that leaves a sound folder after each write (the top comment) */
static int commit(struct fat *f, const struct run *old, const struct run *nw)
{
	u32 no = 0, nn = 0, np = 0, m;
	struct dcur k;
	int e;
	if (old) {
		k = (struct dcur){ old->d, 0, 0 };
		for (u32 i = 0; i < old->n; i++, no++) {
			struct slot *s = &s_old[no];
			if ((e = slot_off(f, &k, old->idx + i, &s->off)) || (e = vol_rd(f->v, s->off, 32, s->b)))
				return e == E_EOF ? E_FS : e;
			s->b[0] = 0xe5;
		}
	}
	if (nw) {
		k = (struct dcur){ nw->d, 0, 0 };
		for (u32 i = nw->pad; i < nw->idx; i++, np++) {
			struct slot *s = &s_pad[np];
			if ((e = slot_off(f, &k, i, &s->off))) return e == E_EOF ? E_FS : e;
			memset(s->b, 0, 32);
			s->b[0] = 0xe5;
		}
		for (u32 i = 0; i < nw->n; i++, nn++) {
			struct slot *s = &s_new[nn];
			if ((e = slot_off(f, &k, nw->idx + i, &s->off))) return e == E_EOF ? E_FS : e;
			memcpy(s->b, ents[i], 32);
		}
	}
	m = gather(s_new, nn, gather(s_old, no, gather(s_pad, np, 0)));
	if (in_one(f, s_tmp, m)) return put_span(f, s_tmp, m);
	if (np && (e = put_slots(f, s_pad, np))) return e;
	m = gather(s_new, nn, gather(s_old, no, 0));
	if (in_one(f, s_tmp, m)) return put_span(f, s_tmp, m);
	/* the old long name deleted, places for the new one, the old short
	 * entry deleted, the new one written, then its long name */
	if (no > 1 && (e = put_slots(f, s_old, no - 1))) return e;
	m = 0;
	for (u32 i = 0; i + 1 < nn; i++)
		if (!among(s_old, no, s_new[i].off)) s_tmp[m] = s_new[i], s_tmp[m++].b[0] = 0xe5;
	if (m && (e = put_slots(f, s_tmp, m))) return e;
	if (no && (!nn || s_old[no - 1].off != s_new[nn - 1].off) && (e = put_slots(f, &s_old[no - 1], 1))) return e;
	if (nn && (e = put_slots(f, &s_new[nn - 1], 1))) return e;
	if (nn > 1 && (e = put_slots(f, s_new, nn - 1))) return e;
	return E_OK;
}

static int any_fn(struct fat *f, struct dent *de, const u16 *ln, u32 nl, void *ctx)
{
	return skip_entry(de->raw) ? 0 : 1;
}

/* path split: its folder's bytes (*plen), its name's start */
static u32 split(const char *path, u32 len, u32 *plen)
{
	u32 i = len;
	while (i && path[i - 1] != '/') i--;
	*plen = i ? i - 1 : 0;
	return i;
}

/* a new folder's cluster (*c): "." and "..", zeros */
static int dir_new(struct fat *f, u32 parent, const u8 *sfe, u32 *c)
{
	u32 got;
	int e = chain_alloc(f, 1, c, &got);
	if (e) return e;
	memset(sbuf, 0, 512);
	for (int i = 0; i < 2; i++) {
		u8 *r = sbuf + 32 * i;
		memcpy(r, sfe, 32);
		memset(r, ' ', 11);
		r[0] = '.';
		if (i) r[1] = '.';
		r[11] = A_DIR, r[12] = 0;
		set_first(f, r, i ? (parent == root_id(f) ? 0 : parent) : *c);
	}
	e = wr(f, cl_off(f, *c), 512, sbuf);
	if (!e) e = wr_zeros(f, cl_off(f, *c) + 512, f->csize - 512);
	return e;
}

/* a new file or folder at path (it is not there): *de */
static int create(struct fat *f, const char *path, bool dir, struct dent *de)
{
	u32 len = strlen(path), plen, i = split(path, len, &plen), d, n, nb, c = 0;
	struct dent pe;
	int e = resolve(f, path, plen, 0, &pe);
	if (e || (e = dir_of(f, &pe, &d))) return e;
	if (name_decode(path + i, len - i, nbuf, &n) || !name_ok(nbuf, n)) return E_NOT_FOUND;
	u8 sfe[32], nt;
	memset(sfe, 0, sizeof(sfe));
	int fit = sfn_make(nbuf, n, sfe, &nt, &nb);
	if (!fit && (e = sfn_tail(f, d, sfe, nb, ~0ull))) return e;
	sfe[11] = dir ? A_DIR : A_ARCH;
	sfe[12] = fit == 2 ? nt : 0;
	stamp(sfe, rtc_seconds(), T_CREATE | T_WRITE | T_ACCESS);
	u32 nr = make_run(nbuf, n, fit != 2, sfe);
	struct run r;
	if ((e = find_slots(f, d, nr, &r))) return e;
	if (dir) {
		if ((e = dir_new(f, d, sfe, &c))) return e;
		set_first(f, sfe, c);
		set_first(f, ents[nr - 1], c);
	}
	if ((e = commit(f, NULL, &r))) return e;
	de->d = d, de->idx = r.idx + nr - 1, de->n = nr, de->off = s_new[nr - 1].off;
	memcpy(de->raw, sfe, 32);
	return E_OK;
}

static int put_entry(struct fat *f, const struct dent *de) { return wr(f, de->off, 32, de->raw); }

/* the file shrunk to size: its entry first, then the clusters past it freed */
static int cut(struct fat *f, struct dent *de, u64 size)
{
	u32 first = de_first(f, de), keep = (size + f->csize - 1) >> f->cshift, last, nx;
	put_le32(de->raw + 28, size);
	if (!keep) set_first(f, de->raw, 0);
	stamp(de->raw, rtc_seconds(), T_WRITE | T_ACCESS);
	de->raw[11] |= A_ARCH;
	int e = put_entry(f, de);
	f->cfirst = 0;
	if (e || !ok_cl(f, first)) return e;
	if (!keep) return chain_free(f, first);
	if ((e = chain_seek(f, first, keep - 1, &last, NULL))) return e == E_EOF ? E_OK : e;
	e = fat_next(f, last, &nx);
	if (e == E_EOF) return E_OK;
	if (e && e != E_FS) return e;
	int x = fat_set(f, last, f->eoc);
	if (!x) x = win_flush(f);
	f->cfirst = 0;
	if (x || e) return x;
	return chain_free(f, nx);
}

/* len bytes (zeros: buf NULL) at off, a gap before it zeros; *done */
static int grow_write(struct fat *f, struct dent *de, u64 off, u32 len, const u8 *buf, u32 *done)
{
	u64 size = le32(de->raw + 28), end = off + len;
	u32 first = de_first(f, de), have = 0, last = 0, got = 0, nc = 0;
	int full = E_OK, e;
	if (end > 0xffffffffu) end = 0xffffffffu, full = E_DISK_FULL;
	if (off > end) return E_DISK_FULL;
	u32 need = (end + f->csize - 1) >> f->cshift, hs = (size + f->csize - 1) >> f->cshift;
	if (first) {
		if (!ok_cl(f, first)) return E_FS;
		have = hs;
		if (need > hs) {
			if ((e = chain_end(f, first, &last, &have))) return e;
			if (have < hs) return E_FS;
		}
	} else if (size) return E_FS;
	if (need > have) {
		if ((e = count_free(f))) return e;
		u32 want = need - have;
		if (want > f->free) {          /* the room there is */
			u64 room = (u64)(have + f->free) << f->cshift;
			if (room < end) end = room, full = E_DISK_FULL;
			if (end < off || (end == off && len)) return E_DISK_FULL;
			want = ((end + f->csize - 1) >> f->cshift) - have;
		}
		if (want) {
			e = chain_alloc(f, want, &nc, &got);
			if (e && e != E_DISK_FULL) return e;
			if (got < want) {
				full = E_DISK_FULL;
				u64 room = (u64)(have + got) << f->cshift;
				if (room < end) end = room;
				if (end < off || (end == off && len)) {
					if (got) chain_free(f, nc);
					return E_DISK_FULL;
				}
			}
			if (have) {
				if ((e = fat_set(f, last, nc)) || (e = win_flush(f))) return e;
			} else first = nc;
		}
	}
	if (size < off && (e = file_io(f, first, size, MIN(off, end) - size, NULL, IO_ZERO))) return e;
	if (end > off && (e = file_io(f, first, off, end - off, (u8 *)buf, buf ? IO_WR : IO_ZERO))) return e;
	set_first(f, de->raw, first);
	put_le32(de->raw + 28, MAX(size, end));
	stamp(de->raw, rtc_seconds(), T_WRITE | T_ACCESS);
	de->raw[11] |= A_ARCH;
	if ((e = put_entry(f, de))) return e;
	*done = end > off ? end - off : 0;
	return full;
}

/* --- the file system's functions ------------------------------------------ */

static int fat_info(struct vol *v, const char *path, struct vdirent *e)
{
	struct fat *f = v->fsd;
	struct dent de;
	int r = resolve(f, path, strlen(path), 0, &de);
	if (r) return r;
	memset(e, 0, sizeof(*e));
	if (!de.n) e->attr = A_DIR;
	else fill(de.raw, NULL, 0, e);
	return E_OK;
}

static int fat_read(struct vol *v, const char *path, u64 off, u32 len, void *buf, u32 *got)
{
	struct fat *f = v->fsd;
	struct dent de;
	*got = 0;
	int e = resolve(f, path, strlen(path), 0, &de);
	if (e) return e;
	if (!de.n || (de.raw[11] & A_DIR)) return E_ACCESS;
	u64 size = le32(de.raw + 28);
	if (off >= size) return E_OK;
	u64 n = MIN(len, size - off);
	if ((e = file_io(f, de_first(f, &de), off, n, buf, IO_RD))) return e;
	*got = n;
	return E_OK;
}

struct rdc {
	u32 first, count, k;
	void (*emit)(void *ctx, const struct vdirent *e);
	void *ctx;
};

static int rd_fn(struct fat *f, struct dent *de, const u16 *ln, u32 nl, void *ctx)
{
	struct rdc *r = ctx;
	if (skip_entry(de->raw)) return 0;
	if (r->k >= r->first && r->k - r->first < r->count) {
		fill(de->raw, ln, nl, &rde);
		r->emit(r->ctx, &rde);
	}
	r->k++;
	return 0;
}

static int fat_readdir(struct vol *v, const char *path, u32 first, u32 count,
		       void (*emit)(void *ctx, const struct vdirent *e), void *ctx, u32 *total)
{
	struct fat *f = v->fsd;
	struct dent de;
	u32 d;
	*total = 0;
	int e = resolve(f, path, strlen(path), 0, &de);
	if (e) return e;
	if ((e = dir_of(f, &de, &d))) return e == E_NOT_FOUND ? E_ACCESS : e;
	struct rdc r = { first, count, 0, emit, ctx };
	e = dir_scan(f, d, rd_fn, &r);
	*total = r.k;
	return e;
}

static int fat_map(struct vol *v, const char *path, struct fmap *m)
{
	struct fat *f = v->fsd;
	struct dent de;
	memset(m, 0, sizeof(*m));
	int e = resolve(f, path, strlen(path), 0, &de);
	if (e) return e;
	if (!de.n || (de.raw[11] & A_DIR)) return E_ACCESS;
	u32 c = de_first(f, &de);
	m->id = c;
	m->size = le32(de.raw + 28);
	m->flags = (de.raw[11] & A_RO ? FMAP_RO : 0) | (de.raw[11] & A_SYSTEM ? FMAP_SYSTEM : 0);
	if (!m->size) return E_OK;
	if (!ok_cl(f, c)) return E_FS;
	u32 need = (m->size + f->csize - 1) >> f->cshift;
	for (u32 i = 0; i < need; i++) {
		if (i && (e = fat_next(f, c, &c))) return e == E_EOF ? E_FS : e;
		u64 at = cl_off(f, c), len = i + 1 < need ? f->csize : m->size - ((u64)i << f->cshift);
		if (m->n && m->ext[m->n - 1].off + m->ext[m->n - 1].len == at) m->ext[m->n - 1].len += len;
		else if (m->n == FMAP_MAX) return E_FS;
		else m->ext[m->n].off = at, m->ext[m->n++].len = len;
	}
	for (int i = 0; i < m->n; i++)          /* a cluster twice: a cycle */
		for (int j = i + 1; j < m->n; j++)
			if (m->ext[i].off < m->ext[j].off + m->ext[j].len && m->ext[j].off < m->ext[i].off + m->ext[i].len)
				return E_FS;
	return E_OK;
}

static int fat_write(struct vol *v, const char *path, u64 off, u32 len, const void *buf, bool rewrite, u32 *done)
{
	*done = 0;
	if (!v->writable) return E_ACCESS;
	struct fat *f = v->fsd;
	struct dent de;
	int e = resolve(f, path, strlen(path), 0, &de);
	if (e == E_NOT_FOUND && rewrite) e = create(f, path, false, &de);
	else if (!e && (!de.n || (de.raw[11] & A_DIR))) return E_ACCESS;
	else if (!e && rewrite && (le32(de.raw + 28) || de_first(f, &de))) e = cut(f, &de, 0);
	if (!e) e = grow_write(f, &de, rewrite ? 0 : off, len, buf, done);
	return finish(f, e);
}

static int fat_truncate(struct vol *v, const char *path, u64 size)
{
	if (!v->writable) return E_ACCESS;
	struct fat *f = v->fsd;
	struct dent de;
	u32 dn;
	int e = resolve(f, path, strlen(path), 0, &de);
	if (e) return e;
	if (!de.n || (de.raw[11] & A_DIR)) return E_ACCESS;
	if (size > 0xffffffffu) return E_DISK_FULL;
	u64 cur = le32(de.raw + 28);
	if (size == cur) return E_OK;
	e = size < cur ? cut(f, &de, size) : grow_write(f, &de, size, 0, NULL, &dn);
	return finish(f, e);
}

static int fat_mkdir(struct vol *v, const char *path)
{
	if (!v->writable) return E_ACCESS;
	struct fat *f = v->fsd;
	struct dent de;
	int e = resolve(f, path, strlen(path), 0, &de);
	if (!e) return !de.n || (de.raw[11] & A_DIR) ? E_OK : E_ACCESS;
	if (e != E_NOT_FOUND) return e;
	return finish(f, create(f, path, true, &de));
}

static int fat_remove(struct vol *v, const char *path)
{
	if (!v->writable) return E_ACCESS;
	struct fat *f = v->fsd;
	struct dent de;
	int e = resolve(f, path, strlen(path), 0, &de);
	if (e) return e;
	if (!de.n) return E_ACCESS;
	u32 c = de_first(f, &de);
	if (de.raw[11] & A_DIR) {
		if (!ok_cl(f, c)) return E_FS;
		e = dir_scan(f, c, any_fn, NULL);
		if (e) return e == 1 ? E_ACCESS : e;
	}
	struct run r = { de.d, de.idx + 1 - de.n, de.n, 0 };
	e = commit(f, &r, NULL);
	if (!e && ok_cl(f, c)) e = chain_free(f, c);
	return finish(f, e);
}

/* a moved folder's ".." to its new parent */
static int set_dotdot(struct fat *f, u32 c, u32 parent)
{
	u8 b[32];
	u64 at = cl_off(f, c) + 32;
	int e = vol_rd(f->v, at, 32, b);
	if (e || b[0] != '.' || b[1] != '.') return e;
	set_first(f, b, parent == root_id(f) ? 0 : parent);
	return wr(f, at, 32, b);
}

static int fat_rename(struct vol *v, const char *from, const char *to)
{
	if (!v->writable) return E_ACCESS;
	struct fat *f = v->fsd;
	struct dent fe, pe, xe;
	u32 len = strlen(to), plen, i = split(to, len, &plen), td, n, nb;
	int e = resolve(f, from, strlen(from), 0, &fe);
	if (e) return e;
	if (!fe.n || i == len) return E_ACCESS;
	bool isdir = fe.raw[11] & A_DIR;
	u32 fc = de_first(f, &fe);
	if (isdir && !ok_cl(f, fc)) return E_FS;
	if ((e = resolve(f, to, plen, isdir ? fc : 0, &pe)) || (e = dir_of(f, &pe, &td))) return e;
	if (isdir && td == fc) return E_ACCESS;
	if (name_decode(to + i, len - i, nbuf, &n) || !name_ok(nbuf, n)) return E_NOT_FOUND;
	struct look l = { nbuf, n, &xe };
	e = dir_scan(f, td, look_fn, &l);
	if (e == 1) {
		if (xe.off != fe.off) return E_ACCESS;
	} else if (e) return e;
	u8 sfe[32], nt;
	memcpy(sfe, fe.raw, 32);
	int fit = sfn_make(nbuf, n, sfe, &nt, &nb);
	if (!fit && (e = sfn_tail(f, td, sfe, nb, fe.off))) return e;
	sfe[12] = (sfe[12] & ~0x18) | (fit == 2 ? nt : 0);
	u32 nr = make_run(nbuf, n, fit != 2, sfe);
	struct run ow = { fe.d, fe.idx + 1 - fe.n, fe.n, 0 }, nw;
	if (td == fe.d && nr <= fe.n) nw = (struct run){ td, fe.idx + 1 - nr, nr, fe.idx + 1 - nr };
	else if ((e = find_slots(f, td, nr, &nw))) return finish(f, e);
	e = commit(f, &ow, &nw);
	if (!e && isdir && td != fe.d) e = set_dotdot(f, fc, td);
	return finish(f, e);
}

static int fat_setattr(struct vol *v, const char *path, const struct vdirent *a)
{
	if (!v->writable) return E_ACCESS;
	struct fat *f = v->fsd;
	struct dent de;
	int e = resolve(f, path, strlen(path), 0, &de);
	if (e) return e;
	if (!de.n) return E_ACCESS;
	const u32 ch = A_RO | A_HIDDEN | A_SYSTEM | A_ARCH;
	de.raw[11] = (de.raw[11] & ~ch) | (a->attr & ch);
	if (a->ctime) stamp(de.raw, a->ctime, T_CREATE);
	if (a->atime) stamp(de.raw, a->atime, T_ACCESS);
	if (a->mtime) stamp(de.raw, a->mtime, T_WRITE);
	return finish(f, put_entry(f, &de));
}

static void fat_sync(struct vol *v)
{
	struct fat *f = v->fsd;
	if (!f || !v->writable || win_flush(f)) return;
	fsi_write(f);
}

/* --- mounting --------------------------------------------------------------- */

static int label_fn(struct fat *f, struct dent *de, const u16 *ln, u32 nl, void *ctx)
{
	if ((de->raw[11] & (A_LABEL | A_DIR)) != A_LABEL) return 0;
	memcpy(ctx, de->raw, 11);
	return 1;
}

static void label_text(const u8 *r, char *o, u32 size)
{
	u16 s[11];
	u32 n = 11;
	while (n && r[n - 1] == ' ') n--;
	for (u32 i = 0; i < n; i++) s[i] = oem(i == 0 && r[0] == 0x05 ? 0xe5 : r[i], false);
	to_utf8(s, n, o, size);
}

static int fat_mount(struct vol *v)
{
	static u8 bs[512];
	int e = vol_rd(v, 0, 512, bs);
	if (e) return e;
	u32 bps = le16(bs + 11), spc = bs[13], rsvd = le16(bs + 14), nfats = bs[16], rootent = le16(bs + 17);
	u32 tot = le16(bs + 19) ? le16(bs + 19) : le32(bs + 32);
	u32 fatsz = le16(bs + 22) ? le16(bs + 22) : le32(bs + 36);
	if (bs[510] != 0x55 || bs[511] != 0xaa || (bs[0] != 0xeb && bs[0] != 0xe9)) return E_UNKNOWN_FS;
	if ((bps != 512 && bps != 1024 && bps != 2048 && bps != 4096) || !spc || (spc & (spc - 1))) return E_UNKNOWN_FS;
	if (!rsvd || !nfats || nfats > 16 || !fatsz || !tot || (bs[21] != 0xf0 && bs[21] < 0xf8)) return E_UNKNOWN_FS;
	u32 lgbps = lg2(bps), rootsec = (rootent * 32 + bps - 1) >> lgbps;
	u64 meta = rsvd + (u64)nfats * fatsz + rootsec;
	if (meta >= tot) return E_UNKNOWN_FS;
	u32 nclus = (u32)((tot - meta) >> lg2(spc));
	int type = nclus < 4085 ? 12 : nclus < 65525 ? 16 : 32;
	if (type == 32 ? le16(bs + 22) || rootent || nclus > 0x0ffffff5 : !rootent) return E_UNKNOWN_FS;
	u64 fatbytes = (u64)fatsz << lgbps;
	u64 need = type == 12 ? (u64)(nclus + 2) * 3 / 2 + 1 : (u64)(nclus + 2) * (type / 8);
	if (fatbytes < need) return E_UNKNOWN_FS;
	u32 cshift = lgbps + lg2(spc);
	u64 data_off = meta << lgbps, vsize = v->nblocks * v->bsize;
	if (vsize && data_off + ((u64)nclus << cshift) > vsize) {
		slog("fat: %s: the file system (%u MiB) is larger than the volume (%u MiB)\n", v->name,
		     (u32)((data_off + ((u64)nclus << cshift)) >> 20), (u32)(vsize >> 20));
		return E_UNKNOWN_FS;
	}
	u32 root_cl = 0, fsi = 0, ffree = FREE_UNKNOWN, fnext = FREE_UNKNOWN, rfat = 0;
	bool mirror = true;
	if (type == 32) {
		root_cl = le32(bs + 44) & 0x0fffffff;
		if (root_cl < 2 || root_cl > nclus + 1) return E_UNKNOWN_FS;
		u32 ext = le16(bs + 40);
		mirror = !(ext & 0x80);
		rfat = mirror ? 0 : ext & 15;
		if (rfat >= nfats) return E_UNKNOWN_FS;
		fsi = le16(bs + 48);
		if (fsi && fsi < rsvd) {
			static u8 fb[512];
			if ((e = vol_rd(v, (u64)fsi << lgbps, 512, fb))) return e;
			if (le32(fb) == 0x41615252 && le32(fb + 484) == 0x61417272 && le32(fb + 508) == 0xaa550000) {
				ffree = le32(fb + 488), fnext = le32(fb + 492);
			} else fsi = 0;
		} else fsi = 0;
	}
	struct fat *f = v->fs == &fs_fat && v->fsd ? v->fsd : kmalloc(sizeof(struct fat));
	if (!f) return E_MEMORY;
	memset(f, 0, sizeof(*f));
	f->v = v;
	f->type = type;
	f->bps = bps, f->cshift = cshift, f->csize = 1u << cshift;
	u32 bsz = v->bsize >= 512 && !(v->bsize & (v->bsize - 1)) ? v->bsize : bps;
	f->lga = lg2(MIN(bps, bsz));
	f->nclus = nclus;
	f->eoc = type == 12 ? 0xfff : type == 16 ? 0xffff : 0x0fffffff;
	f->eoc_min = type == 12 ? 0xff8 : type == 16 ? 0xfff8 : 0x0ffffff8;
	f->nfats = nfats, f->rfat = rfat, f->mirror = mirror;
	f->fat_off = (u64)rsvd << lgbps, f->fat_stride = fatbytes;
	f->fat_len = (u32)MIN(fatbytes, need + 8);
	f->root_off = (rsvd + (u64)nfats * fatsz) << lgbps;
	f->root_slots = type == 32 ? 0 : rootent;
	f->root_cl = root_cl;
	f->data_off = data_off;
	f->fsi = fsi;
	f->fsi_free = ffree, f->fsi_next = fnext;
	f->free = ffree <= nclus ? ffree : FREE_UNKNOWN;
	f->next = ok_cl(f, fnext) ? fnext : 2;
	/* the label: the root's entry, else the boot sector's */
	u8 lab[11];
	char text[40];
	text[0] = 0;
	if (dir_scan(f, root_id(f), label_fn, lab) == 1) label_text(lab, text, sizeof(text));
	else if (bs[type == 32 ? 66 : 38] == 0x29 && memcmp(bs + (type == 32 ? 71 : 43), "NO NAME    ", 11))
		label_text(bs + (type == 32 ? 71 : 43), text, sizeof(text));
	if (!v->label[0]) memcpy(v->label, text, sizeof(text));
	f->wok = false, f->cfirst = 0;
	v->fs = &fs_fat, v->fsd = f;
	slog("fat: %s: FAT%d, %u clusters of %u bytes, label \"%s\", %s\n", v->name, type, nclus, f->csize, text,
	     v->writable ? "writable" : "read only");
	return E_OK;
}

const struct fsops fs_fat = {
	.name = "fat",
	.mount = fat_mount,
	.info = fat_info,
	.read = fat_read,
	.readdir = fat_readdir,
	.map = fat_map,
	.write = fat_write,
	.truncate = fat_truncate,
	.mkdir = fat_mkdir,
	.remove = fat_remove,
	.rename = fat_rename,
	.setattr = fat_setattr,
	.sync = fat_sync,
};

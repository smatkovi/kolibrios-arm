/* KolibriOS for ARM: the container, the one place on the phones' storage
 * that is written (docs/storage.md).
 *
 * tools/kolibri-home makes it from Linux, on Leste: kolibri/kolibri.img on
 * an ext4 file system (the Pro1X's userdata, the N950's Leste partition)
 * or a FAT one (the N950's MyDocs), every byte written (no holes, no
 * unwritten extents), a FAT file system inside it (4096-byte sectors),
 * immutable (chattr +i on ext4: nothing on Linux writes, moves or deletes
 * its blocks; read only and system on FAT), and next to it
 * kolibri/kolibri.map: the file's inode (or first cluster), its size, a
 * UUID, and its blocks on the partition (FIEMAP):
 *
 *   kolibri-home-map 1
 *   fs ext4 id 12 size 536870912 uuid 0123456789abcdef0123456789abcdef
 *   OFFSET LENGTH            (bytes on the partition, one line a range)
 *
 * Here, with the file system only read: the map read, the file looked up
 * by its path, and its place as that file system's own metadata gives it
 * (ext4's extent tree checked against the block bitmaps and the groups'
 * metadata, FAT's cluster chain) compared with the map, range for range;
 * its inode or first cluster and size the map's, immutable (ext4) or read
 * only and system (FAT); then the container's header (KOLIBRI-HOME, in
 * its FAT's reserved sectors) with the map's UUID and size. Only then is
 * it hd0/1, and writable only with kolibri.home=rw: its ranges are then
 * the fence's (blk.c), the only blocks any driver will write. Any
 * mismatch: said on the console, nothing written. */
#include "storage.h"

#define MAP_PATH "kolibri/kolibri.map"
#define IMG_PATH "kolibri/kolibri.img"
#define HDR_OFF 65536u                  /* the header, in the FAT's reserved sectors */
#define HDR_MAGIC "KOLIBRI-HOME 1"

static struct vol *home;

struct vol *home_vol(void) { return home; }

/* --- the map ----------------------------------------------------------- */

struct hmap {
	bool fat;
	u64 id, size;
	u8 uuid[16];
	int n;
	struct extent ext[FMAP_MAX];
};

static const char *skip_sp(const char *s, const char *e)
{
	while (s < e && (*s == ' ' || *s == '\t')) s++;
	return s;
}

static const char *word(const char *s, const char *e, const char *w)
{
	s = skip_sp(s, e);
	u32 n = strlen(w);
	if ((u32)(e - s) < n || memcmp(s, w, n) || (s + n < e && s[n] != ' ' && s[n] != '\t' && s[n] != '\n'))
		return NULL;
	return s + n;
}

static const char *number(const char *s, const char *e, u64 *v)
{
	s = skip_sp(s, e);
	if (s == e || *s < '0' || *s > '9') return NULL;
	u64 x = 0;
	for (; s < e && *s >= '0' && *s <= '9'; s++) {
		if (x > 0xffffffffffffffffull / 10 - 10) return NULL;
		x = x * 10 + (*s - '0');
	}
	*v = x;
	return s;
}

static int hexval(char c)
{
	return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
}

static const char *line_end(const char *s, const char *e)
{
	while (s < e && *s != '\n') s++;
	return s;
}

/* the map's text; a reason when it is not one */
static const char *map_parse(const char *t, u32 len, struct hmap *m)
{
	const char *e = t + len, *s = t, *le = line_end(s, e), *p;
	memset(m, 0, sizeof(*m));
	if (!(p = word(s, le, "kolibri-home-map")) || !(p = word(p, le, "1"))) return "not a kolibri-home-map 1";
	s = le + 1;
	if (s >= e) return "no second line";
	le = line_end(s, e);
	if (!(p = word(s, le, "fs"))) return "no fs";
	const char *q;
	if ((q = word(p, le, "ext4"))) m->fat = false;
	else if ((q = word(p, le, "fat"))) m->fat = true;
	else return "fs neither ext4 nor fat";
	if (!(p = word(q, le, "id")) || !(p = number(p, le, &m->id))) return "no id";
	if (!(p = word(p, le, "size")) || !(p = number(p, le, &m->size))) return "no size";
	if (!(p = word(p, le, "uuid"))) return "no uuid";
	p = skip_sp(p, le);
	for (int i = 0; i < 16; i++) {
		int h = p + 2 * i + 1 < le ? hexval(p[2 * i]) : -1, l = h >= 0 ? hexval(p[2 * i + 1]) : -1;
		if (h < 0 || l < 0) return "a uuid of 32 hex digits";
		m->uuid[i] = h << 4 | l;
	}
	for (s = le + 1; s < e; s = le + 1) {
		le = line_end(s, e);
		if (skip_sp(s, le) == le) continue;
		if (m->n == FMAP_MAX) return "too many ranges";
		if (!(p = number(s, le, &m->ext[m->n].off)) || !(p = number(p, le, &m->ext[m->n].len)) ||
		    skip_sp(p, le) != le || !m->ext[m->n].len)
			return "a range is not OFFSET LENGTH";
		m->n++;
	}
	return m->n ? NULL : "no ranges";
}

/* --- the checks ------------------------------------------------------------ */

static void say(const struct vol *v, const char *what)
{
	slog("home: %s: %s\n", v->name, what);
}

/* the container on v: its map, its place by v's file system, its header */
static bool check(struct vol *v, struct hmap *m)
{
	static char text[32768];
	struct vdirent e;
	if (v->fs->info(v, MAP_PATH, &e) != E_OK) return false;     /* none here */
	slog("home: %s: %s found\n", v->name, MAP_PATH);
	u32 got = 0;
	if (e.size >= sizeof(text) || v->fs->read(v, MAP_PATH, 0, sizeof(text) - 1, text, &got) || got != e.size) {
		say(v, "the map is not read (or larger than 32 KiB)");
		return false;
	}
	const char *why = map_parse(text, got, m);
	if (why) {
		say(v, why);
		return false;
	}
	bool fat = v->fs == &fs_fat;
	if (m->fat != fat) {
		say(v, "the map is for another file system");
		return false;
	}
	static struct fmap f;
	int r = v->fs->map(v, IMG_PATH, &f);
	if (r) {
		slog("home: %s: %s: its blocks not found (error %d)\n", v->name, IMG_PATH, r);
		return false;
	}
	if (f.id != m->id || f.size != m->size) {
		say(v, "the map is not this file's (inode or first cluster, size)");
		return false;
	}
	if (fat ? (f.flags & (FMAP_RO | FMAP_SYSTEM)) != (FMAP_RO | FMAP_SYSTEM) : !(f.flags & FMAP_IMMUTABLE)) {
		say(v, fat ? "the file is not read only and system" : "the file is not immutable (chattr +i)");
		return false;
	}
	if (f.n != m->n) {
		slog("home: %s: the file is in %d range(s), the map says %d\n", v->name, f.n, m->n);
		return false;
	}
	for (int i = 0; i < f.n; i++)
		if (f.ext[i].off != m->ext[i].off || f.ext[i].len != m->ext[i].len) {
			slog("home: %s: range %d differs from the file's blocks\n", v->name, i);
			return false;
		}
	if (m->size < (1u << 20) || (m->size & 4095) || m->ext[0].len < HDR_OFF + 4096) {
		say(v, "the container's size or first range is too small");
		return false;
	}
	static u8 h[512];
	if (vol_rd(v, m->ext[0].off + HDR_OFF, sizeof(h), h)) {
		say(v, "the header is not read");
		return false;
	}
	if (memcmp(h, HDR_MAGIC, sizeof(HDR_MAGIC)) || memcmp(h + 16, m->uuid, 16) || le64(h + 32) != m->size) {
		say(v, "the container's header is not the map's (magic, UUID, size)");
		return false;
	}
	return true;
}

void home_init(int mode)
{
	if (mode == HOME_OFF) {
		slog("home: not looked for (kolibri.home=off)\n");
		return;
	}
	static struct hmap m;
	int n = vol_count();
	for (int i = 0; i < n; i++) {
		struct vol *v = vol_get(i);
		if (!v->fs || v->host || (v->fs != &fs_ext4 && v->fs != &fs_fat)) continue;
		if (!check(v, &m)) continue;
		struct vol *c = blk_container(v, m.ext, m.n, m.size, mode == HOME_RW);
		if (!c) {
			say(v, "the container's ranges are not in whole blocks of the disk");
			return;
		}
		if (fs_fat.mount(c) != E_OK) {
			say(v, "no FAT file system in the container");
			return;
		}
		c->fs = &fs_fat;
		slog("home: %s's %s checked: %u MiB in %d range(s), hd0/1 %s\n", v->name, IMG_PATH,
		     (u32)(m.size >> 20), m.n, mode == HOME_RW ? "read-write" : "read only (kolibri.home=rw to write)");
		home = c;
		return;
	}
	slog("home: no container (kolibri/kolibri.map): nothing on the storage is written\n");
}

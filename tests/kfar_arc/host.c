/* tests/kfar_arc: kfar_arc (lib/kfar_arc) on the PC, with a kfar of its
 * own: the archive's files unpacked through the plugin's interface, as
 * kfar uses it (tests/kfar_arc/run.py compares them with Python's and
 * 7-Zip's).
 *
 *     host ARCHIVE OUTDIR [getfiles]
 *
 * Without "getfiles" the folders are walked (ReadFolder, SetFolder) and
 * each file read through open/read in pieces of 4 KiB, then read again
 * after setpos to 0 and to a middle (the first 512 bytes compared); with
 * it GetFiles copies everything (addfile, adddir). PASSWORD in the
 * environment answers the password dialog; SayErr's messages go to
 * stderr. Built 32-bit (gcc -m32), as on ARM, the names that clash
 * with the C library's renamed (Makefile). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include "kfar_api.h"

plugin_load_t plugin_load;
OpenFilePlugin_t OpenFilePlugin;
ClosePlugin_t ClosePlugin;
ReadFolder_t ReadFolder;
SetFolder_t SetFolder;
GetFiles_t GetFiles;
GetOpenPluginInfo_t GetOpenPluginInfo;
getattr_t mygetattr;
plugin_open_t myopen;
plugin_read_t myread;
setpos_t mysetpos;
plugin_close_t myclose;

static int errors;

/* ---- kfar's side ---- */

static HANDLE h_open(const char *name, int mode)
{
	return fopen(name, "rb");
}

static HANDLE h_open2(void *id, HANDLE inst, const char *name, int mode)
{
	return id ? 0 : h_open(name, mode);
}

static unsigned h_read(HANDLE f, void *buf, unsigned size)
{
	return fread(buf, 1, size, f);
}

static void h_seek(HANDLE f, int method, long long pos)
{
	fseeko(f, pos, method == 0 ? SEEK_SET : method == 1 ? SEEK_CUR : SEEK_END);
}

static long long h_tell(HANDLE f)
{
	return ftello(f);
}

static long long h_filesize(HANDLE f)
{
	long long p = ftello(f);
	fseeko(f, 0, SEEK_END);
	long long n = ftello(f);
	fseeko(f, p, SEEK_SET);
	return n;
}

static void h_close(HANDLE f)
{
	fclose(f);
}

static void *h_pgalloc(unsigned size)
{
	void *p = calloc(1, size ? size : 1);
	if (!p) fprintf(stderr, "host: no memory (%u)\n", size);
	return p;
}

static void *h_pgrealloc(void *p, unsigned size)
{
	return realloc(p, size ? size : 1);
}

static void h_pgfree(void *p)
{
	free(p);
}

static unsigned h_getfreemem(void)
{
	const char *s = getenv("FREEMEM");     /* KiB: small for no memory streams */
	return s ? strtoul(s, 0, 0) : 1u << 20;
}

static int h_menu(void *v, const char *t, unsigned f) { return -1; }
static int h_menu_c(int l, int t, int w, int h, void *v, const char *ti, unsigned f) { return -1; }

/* the password dialog: its edit box gets PASSWORD */
static int h_DialogBox(void *dlg)
{
	const char *pw = getenv("PASSWORD");
	if (!pw) return -1;
	DLGDATA *d = dlg;
	dlgitemtemplate *it = DLG_ITEMS(d);
	for (unsigned i = 0; i < d->num_items; i++)
		if (it[i].type == 3) {
			editdata *e = it[i].data;
			snprintf(e->data, e->maxlength + 1, "%s", pw);
			e->pos = strlen(e->data);
		}
	return 1;
}

static int h_SayErr(int n, const char **s, int nb, const char **b)
{
	fprintf(stderr, "host: SayErr:");
	for (int i = 0; i < n; i++) fprintf(stderr, " [%s]", s[i]);
	fprintf(stderr, "\n");
	errors++;
	return 0;
}

static int h_Message(const char *title, int n, const char **s, int nb, const char **b)
{
	fprintf(stderr, "host: Message %s:", title);
	for (int i = 0; i < n; i++) fprintf(stderr, " [%s]", s[i]);
	fprintf(stderr, "\n");
	return 0;
}

static struct kfar_console_size console = { 80, 25 };
static kfar_info info = {
	sizeof(kfar_info), 65, h_open, h_open2, h_read, (void *)-1, h_seek, h_tell, (void *)-1,
	h_filesize, h_close, h_pgalloc, h_pgrealloc, h_pgfree, h_getfreemem, h_pgalloc, h_pgrealloc,
	h_pgfree, h_menu, h_menu_c, h_DialogBox, h_SayErr, h_Message, &console,
};

/* ---- the walk ---- */

static HANDLE hPlugin;
static const char *outdir;

static void out_path(char *buf, size_t n, const char *name)
{
	snprintf(buf, n, "%s/%s", outdir, name);
}

/* a file of the plugin: read in pieces, then parts again after setpos */
static void unpack_file(const char *path)
{
	char op[2048];
	out_path(op, sizeof(op), path + 1);
	unsigned char attr[40];
	if (mygetattr(hPlugin, path, attr)) {
		fprintf(stderr, "host: getattr %s failed\n", path);
		errors++;
	}
	long long size = 0;
	memcpy(&size, attr + 32, 8);
	HANDLE f = myopen(hPlugin, path, O_READ);
	if (!f) {
		fprintf(stderr, "host: open %s failed\n", path);
		errors++;
		return;
	}
	FILE *o = fopen(op, "wb");
	static unsigned char buf[4096], again[512];
	long long total = 0;
	for (;;) {
		unsigned n = myread(f, buf, sizeof(buf));
		if (n == (unsigned)-1) {
			fprintf(stderr, "host: read %s failed\n", path);
			errors++;
			break;
		}
		fwrite(buf, 1, n, o);
		total += n;
		if (n < sizeof(buf)) break;
	}
	fclose(o);
	if (total != size) {
		fprintf(stderr, "host: %s: %lld bytes read, getattr says %lld\n", path, total, size);
		errors++;
	}
	/* setpos back and into the middle (multiples of 512, as kfar's) */
	long long poss[2] = { 0, (size / 2) & ~511LL };
	for (int k = 0; k < 2 && size; k++) {
		mysetpos(f, poss[k]);
		unsigned n = myread(f, again, sizeof(again));
		FILE *r = fopen(op, "rb");
		fseeko(r, poss[k], SEEK_SET);
		unsigned m = fread(buf, 1, sizeof(again), r);
		fclose(r);
		if (n != m || memcmp(buf, again, m)) {
			fprintf(stderr, "host: %s: setpos %lld reads other data\n", path, poss[k]);
			errors++;
		}
	}
	myclose(f);
}

static void walk(const char *dir)
{
	static unsigned char area[32 + 304 * 256];
	int r = ReadFolder(hPlugin, 0, 256, area);
	if (r && r != 6) {
		fprintf(stderr, "host: ReadFolder %s: %d\n", dir, r);
		errors++;
		return;
	}
	unsigned n = *(unsigned *)(area + 4);
	/* a copy: the recursion reads into the same area */
	unsigned char *ents = malloc(n * 304 + 1);
	memcpy(ents, area + 32, n * 304);
	for (unsigned i = 0; i < n; i++) {
		unsigned char *e = ents + i * 304;
		const char *name = (char *)e + 40;
		char path[1024];
		snprintf(path, sizeof(path), "%s%s%s", dir, dir[1] ? "/" : "", name);
		printf("%s %s %u\n", e[0] & 0x10 ? "d" : "f", path, *(unsigned *)(e + 32));
		if (e[0] & 0x10) {
			char op[2048];
			out_path(op, sizeof(op), path + 1);
			mkdir(op, 0755);
			if (!SetFolder(hPlugin, name, path)) {
				fprintf(stderr, "host: SetFolder %s failed\n", path);
				errors++;
				continue;
			}
			walk(path);
			if (!SetFolder(hPlugin, "..", dir)) {
				fprintf(stderr, "host: SetFolder .. from %s failed\n", path);
				errors++;
			}
		} else {
			unpack_file(path);
		}
	}
	free(ents);
}

static bool adddir(const char *name, void *bdfe)
{
	char op[2048];
	out_path(op, sizeof(op), name);
	printf("d /%s\n", name);
	mkdir(op, 0755);
	return true;
}

static bool addfile(const char *name, void *bdfe, HANDLE f)
{
	char op[2048];
	out_path(op, sizeof(op), name);
	printf("f /%s\n", name);
	FILE *o = fopen(op, "wb");
	static unsigned char buf[65536];
	for (;;) {
		unsigned n = myread(f, buf, sizeof(buf));
		if (n == (unsigned)-1) {
			errors++;
			break;
		}
		fwrite(buf, 1, n, o);
		if (n < sizeof(buf)) break;
	}
	fclose(o);
	return true;
}

int main(int argc, char **argv)
{
	if (argc < 3) {
		fprintf(stderr, "host ARCHIVE OUTDIR [getfiles]\n");
		return 2;
	}
	outdir = argv[2];
	mkdir(outdir, 0755);
	if (plugin_load(&info)) return 1;
	FILE *f = fopen(argv[1], "rb");
	if (!f) return 1;
	static unsigned char data[1024], attr[40];
	int n = fread(data, 1, sizeof(data), f);
	fseek(f, 0, SEEK_SET);
	hPlugin = OpenFilePlugin(f, attr, data, n, 0, 0, argv[1]);
	if (!hPlugin || hPlugin == (HANDLE)-1) {
		fprintf(stderr, "host: OpenFilePlugin: %p\n", hPlugin);
		return 1;
	}
	OpenPluginInfo opi = { 0 };
	GetOpenPluginInfo(hPlugin, &opi);
	if (argc > 3 && !strcmp(argv[3], "getfiles")) GetFiles(hPlugin, -1, 0, addfile, adddir);
	else walk("/");
	ClosePlugin(hPlugin);
	printf("host: %d errors\n", errors);
	return errors ? 1 : 0;
}

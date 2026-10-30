/* kfar_arc for KolibriOS for ARM: the loader of ZIP archives (zip.inc).
 *
 * Translated to C from KolibriOS's programs/fs/kfar/kfar_arc/zip.inc,
 * diamond (Evgeny Grechnikov), 2007, GPL-2.0 (kfar_arc.h).
 *
 * The central directory (zip64 too) is read into the handle's records,
 * the names into a buffer of their own (the archive's name first: the
 * parts of a split archive are opened by it). Files are stored (method
 * 0), deflated (8) or deflate64'ed (9), the traditional PKWARE
 * encryption too; their data is read through zip.read (the parts of a
 * split archive in turn, the decryption), the deflated ones through the
 * Deflate decoder (deflate.c). A file small enough for the free memory
 * is unpacked at open (a memory stream).
 *
 * Differences: a zip64 extra field gives the sizes, the offset and the
 * disk that are 0xFFFFFFFF (0xFFFF) in the central directory, in the
 * order of the specification (the original reads it over all four in
 * turn); the records are zeroed when the array grows (the original has
 * 68.20's fresh pages). */
#include "kfar_arc.h"

typedef struct file_in_zip {
	file_common c;
	u16 version_made_by;
	u16 version_needed;
	u16 flags;
	u16 method;
	u16 time;
	u16 date;
	u32 crc;
	u64 compressed_size;
	u64 uncompressed_size;
	u64 local_hdr;
	u32 disk;
	u32 attr;
} file_in_zip;

typedef struct handle_zip {
	handle_common c;
	char *names_buf;
	HANDLE host;
	u32 host_datetime[6];
	u32 num_disks;
	u32 cur_disk;
	HANDLE cur_handle;
	HANDLE host_hPlugin;
	void *host_idPlugin;
	int password_len;               /* -1 if no password defined; in characters */
	char password[password_maxlen];
} handle_zip;

const u32 handle_zip_basesize = sizeof(handle_zip), file_in_zip_size = sizeof(file_in_zip);

/* a file's handle */
typedef struct file_handle_zip {
	u32 type;                       /* type_zip */
	file_in_zip *item;
	handle_zip *base;
	u32 start_disk;
	u64 start;                      /* the data's start on start_disk */
	u64 pos;                        /* in the unpacked data */
	u64 posc;                       /* in the packed data */
	streamInfo *decoder;
	u32 cur_disk;
	u64 cur_disk_start;
	u8 bError;
	u8 bPassProtected;
	u8 bPassInited;
	u8 pad;
	u32 keys[3];
	u32 saved_keys[3];
} file_handle_zip;

/* the packed data's stream for the decoder: zipin.fillBuf reads it
 * through zip.read (streamInfo.size: the file's handle) */
typedef struct zipin {
	streamInfo s;
	file_handle_zip *handle;
} zipin;

/* zip.inc's uglobal */
static u32 zip_num_disks, zip_cur_disk;
static HANDLE zip_cur_handle;
static u32 zip_NumAllocated, zip_NamesSize;
static char zip_disk_name[1024];

static inline u16 get16(const u8 *p) { return p[0] | p[1] << 8; }
static inline u32 get32(const u8 *p) { return p[0] | p[1] << 8 | p[2] << 16 | (u32)p[3] << 24; }
static inline u64 get64(const u8 *p) { return get32(p) | (u64)get32(p + 4) << 32; }

/* zip64 end of central directory record, zip.file_header (the central
 * directory's), zip.extra */
#define zip64eocd_sz 56
#define file_header_sz_cdir 46
#define file_header_sz_local 30

static void close_cur_handle(void);
static HANDLE zip_open_splitted(u32 ecx, void *id, HANDLE instance, const char *name, int mode);

/* zip.inc: open_zip: in: the archive basefile; out: the handle, 0 if it
 * is not a zip after all, -1 if it could not be read */
HANDLE open_zip(HANDLE basefile, const void *attr, void *baseplugin_id, HANDLE baseplugin_instance,
		const char *name)
{
	zip_cur_handle = 0;
	/* look for end of central directory record */
	s64 pos = filesize(basefile) - 1024;
	if (pos < 0) pos = 0;
	seek(basefile, 0, pos);
	int eax = read(basefile, buffer, 1024);
	if (eax < 22) return 0;
	inStream = basefile;
	const u8 *edi = buffer + eax - 22;
	while (get32(edi) != 0x06054B50)
		if (--edi < buffer) return 0;
	/* .eocd_found */
	HANDLE ebx = basefile;
	u32 ecx;
	u8 hdr[zip64eocd_sz];
	if (edi >= buffer + 20 && get32(edi - 20) == 0x07064B50) {
		/* the zip64 locator: its disk, the record's offset, the disks */
		ecx = get32(edi - 16);
		zip_num_disks = get32(edi - 4);
		zip_cur_disk = zip_num_disks - 1;
		if (zip_cur_disk != ecx) {
			ebx = zip_open_splitted(ecx, baseplugin_id, baseplugin_instance, name, 1);
			if (!ebx) return 0;
		}
		seek(ebx, 0, get64(edi - 12));
		if (read(ebx, hdr, zip64eocd_sz) != zip64eocd_sz || get32(hdr) != 0x06064B50) goto close_err;
		ecx = get32(hdr + 20);          /* cdir_disk */
		if (ecx != zip_cur_disk) {
			ebx = zip_open_splitted(ecx, baseplugin_id, baseplugin_instance, name, 1);
			if (!ebx) return 0;
		}
		seek(ebx, 0, get64(hdr + 48));  /* cdir_start */
		ecx = get32(hdr + 32);          /* cdir_entries */
	} else {
		/* .no_eocd64 */
		ecx = get16(edi + 6);
		zip_num_disks = get16(edi + 4) + 1;
		zip_cur_disk = zip_num_disks - 1;
		if (zip_cur_disk != ecx) {
			ebx = zip_open_splitted(ecx, baseplugin_id, baseplugin_instance, name, 1);
			if (!ebx) return 0;
		}
		seek(ebx, 0, get32(edi + 16));
		ecx = get16(edi + 10);
	}
	/* .parse_cdir */
	zip_NumAllocated = ecx;
	u64 sz = (u64)ecx * sizeof(file_in_zip) + sizeof(handle_zip);
	if (sz > 0xFFFFFFFFu) goto close_err;
	hOut_allocated = sz;
	hOut = pgalloc(hOut_allocated);
	if (!hOut) goto close_err;
	memset(hOut, 0, hOut_allocated);
	zip_NamesSize = strlen(name) + 1;
	char *ebp = pgalloc(zip_NamesSize);
	if (!ebp) goto free_err;
	memcpy(ebp, name, zip_NamesSize);
	((handle_zip *)hOut)->c.type = type_zip;
	u32 rec = sizeof(handle_zip);           /* the record's offset in hOut */
	for (;;) {
		/* .cdir_loop */
		u8 h[file_header_sz_cdir];
		if (read(ebx, h, file_header_sz_cdir) != file_header_sz_cdir) {
			if (zip_cur_disk + 1 == zip_num_disks) break;   /* .cdir_done */
			ebx = zip_open_splitted(zip_cur_disk + 1, baseplugin_id, baseplugin_instance, name, 1);
			if (!ebx) goto free_names_err;
			continue;
		}
		/* .cdir_item_ok */
		if (get32(h) != 0x02014B50) break;
		handle_zip *hz = (handle_zip *)hOut;
		if (++hz->c.NumFiles > zip_NumAllocated) {
			zip_NumAllocated++;
			u8 *n = pgrealloc(hOut, hOut_allocated + sizeof(file_in_zip));
			if (!n) goto free_names_err;
			hOut = n;
			memset(hOut + hOut_allocated, 0, sizeof(file_in_zip));
			hOut_allocated += sizeof(file_in_zip);
		}
		/* .norealloc */
		file_in_zip *r = (file_in_zip *)(hOut + rec);
		r->version_made_by = get16(h + 4);
		r->version_needed = get16(h + 6);
		r->flags = get16(h + 8);
		r->method = get16(h + 10);
		r->time = get16(h + 12);
		r->date = get16(h + 14);
		r->crc = get32(h + 16);
		r->compressed_size = get32(h + 20);
		r->uncompressed_size = get32(h + 24);
		u32 fname_len = get16(h + 28);
		/* the name, after the names so far */
		char *nb = pgrealloc(ebp, zip_NamesSize + fname_len + 1);
		if (!nb) goto free_names_err;
		ebp = nb;
		r->c.fullname = (char *)(uintptr_t)zip_NamesSize;      /* (an offset until .cdir_done) */
		char *edx = ebp + zip_NamesSize;
		zip_NamesSize += fname_len + 1;
		edx[fname_len] = 0;
		read(ebx, edx, fname_len);
		if (fname_len && edx[fname_len - 1] == '/') {
			edx[fname_len - 1] = 0;
			r->c.bIsDirectory = 1;
		}
		r->local_hdr = get32(h + 42);
		r->disk = get16(h + 34);
		/* .parse_extra */
		u32 extra = get16(h + 30), left = extra;
		while (left >= 4) {
			u8 x[4];
			if (read(ebx, x, 4) != 4) break;
			left -= 4;
			u32 xsize = get16(x + 2);
			if (get16(x) == 1) {
				/* the zip64 extra: the fields that did not fit */
				u8 z[28];
				u32 n = xsize < sizeof(z) ? xsize : sizeof(z);
				if (n > left) break;
				n = read(ebx, z, n);
				left -= n;
				xsize -= n;
				u32 o = 0;
				if (r->uncompressed_size == 0xFFFFFFFFu && o + 8 <= n) r->uncompressed_size = get64(z + o), o += 8;
				if (r->compressed_size == 0xFFFFFFFFu && o + 8 <= n) r->compressed_size = get64(z + o), o += 8;
				if (r->local_hdr == 0xFFFFFFFFu && o + 8 <= n) r->local_hdr = get64(z + o), o += 8;
				if (r->disk == 0xFFFF && o + 4 <= n) r->disk = get32(z + o);
			}
			/* .extra_skip */
			if (xsize > left) break;
			left -= xsize;
			if (xsize) seek(ebx, 1, xsize);
		}
		/* .extra_done: the rest of the extra field, the comment */
		u32 skip = left + get16(h + 32);
		if (skip) seek(ebx, 1, skip);
		r->attr = get32(h + 38);
		rec += sizeof(file_in_zip);
	}
	/* .cdir_done */
	{
		handle_zip *edi2 = (handle_zip *)hOut;
		edi2->num_disks = zip_num_disks;
		edi2->cur_disk = zip_cur_disk;
		edi2->cur_handle = zip_cur_handle;
		edi2->names_buf = ebp;
		file_in_zip *r = (file_in_zip *)(edi2 + 1);
		for (u32 i = 0; i < edi2->c.NumFiles; i++)
			r[i].c.fullname = ebp + (uintptr_t)r[i].c.fullname;
		RETURN_CATCH(saved) {
			/* (init_file_links' message said why: a name too long) */
			pgfree(ebp);
			if (hOut) pgfree(hOut);
			close_cur_handle();
			return (HANDLE)-1;
		}
		bool ok = init_file_links(sizeof(handle_zip), edi2->c.NumFiles, sizeof(file_in_zip),
					  offsetof(handle_common, root));
		RETURN_UNCATCH(saved);
		if (!ok) {
			pgfree(ebp);
			close_cur_handle();
			return (HANDLE)-1;
		}
	}
	handle_zip *eax2 = (handle_zip *)hOut;
	eax2->c.curdir = 0;                     /* set root directory */
	eax2->host = inStream;
	eax2->password_len = -1;
	eax2->host_idPlugin = baseplugin_id;
	eax2->host_hPlugin = baseplugin_instance;
	memcpy(eax2->host_datetime, (const u8 *)attr + 8, 24);
	return eax2;

free_names_err:
	pgfree(ebp);
free_err:
	pgfree(hOut);
close_err:
	close_cur_handle();
	return (HANDLE)-1;
}

/* zip.inc: close_cur_handle */
static void close_cur_handle(void)
{
	zip_cur_disk = (u32)-1;
	if (zip_cur_handle) {
		close(zip_cur_handle);
		zip_cur_handle = 0;
	}
}

/* zip.inc: zip.open_splitted: the part ecx of the archive name: the last
 * is the archive itself ([inStream]), the others name.zNN (name.NNN from
 * 100 on; the original adds them to the whole name) */
static HANDLE zip_open_splitted(u32 ecx, void *id, HANDLE instance, const char *name, int mode)
{
	close_cur_handle();
	zip_cur_disk = ecx;
	u32 edx = ecx + 1;
	if (edx == zip_num_disks) {
		seek(inStream, 0, 0);
		return inStream;
	}
	u32 len = strlen(name);
	if (len >= 1024 || 1024 - len - 1 < 4 || (edx >= 100 && 1024 - len - 1 < 12)) return 0;   /* .ret0 */
	memcpy(zip_disk_name, name, len);
	char *edi = zip_disk_name + len;
	*edi++ = '.';
	if (edx < 100) {
		*edi++ = 'z';
		*edi++ = '0' + edx / 10;
		*edi++ = '0' + edx % 10;
		*edi = 0;
	} else {
		/* .big */
		char d[12];
		int n = 0;
		do d[n++] = '0' + edx % 10; while ((edx /= 10));
		while (n) *edi++ = d[--n];
		*edi = 0;
	}
	/* .nameok */
	zip_cur_handle = open2(id, instance, zip_disk_name, mode);
	return zip_cur_handle;
}

/* zip.inc: close_zip */
void close_zip(handle_common *p)
{
	handle_zip *esi = (handle_zip *)p;
	close(esi->host);
	pgfree(esi->names_buf);
	if (esi->cur_handle) close(esi->cur_handle);
	pgfree(esi);
}

/* zip.inc: getattr_zip: ebp = hPlugin, eax -> item, edi -> info (70.5's
 * 40 bytes) */
void getattr_zip(handle_common *ebp, file_common *eax, u8 *edi)
{
	memset(edi, 0, 40);
	if (eax->bPseudoFolder) {
		/* .pseudo: attributes: folder; the archive's times */
		*(u32 *)edi = 0x10;
		memcpy(edi + 8, ((handle_zip *)ebp)->host_datetime, 24);
		return;
	}
	file_in_zip *esi = (file_in_zip *)eax;
	*(u32 *)edi = esi->attr;
	/* the modification time and date from DOS's */
	u32 t = esi->time, d = esi->date;
	edi[24] = (t & 0x1F) * 2;
	edi[25] = t >> 5 & 0x3F;
	edi[26] = t >> 11;
	edi[28] = d & 0x1F;
	edi[29] = d >> 5 & 0xF;
	u32 year = (d >> 9) + 1980;
	edi[30] = year;
	edi[31] = year >> 8;
	memcpy(edi + 32, &esi->uncompressed_size, 8);
}

static HANDLE zip_get_cur_handle(file_handle_zip *ebx);
static u8 *zip_read(file_handle_zip *ebx, u8 *edi, u32 ecx);
static void zip_setpos(file_handle_zip *ebx, u64 newpos);
static void zip_update_start(file_handle_zip *ebx, u32 esi);
static void zip_init_keys(file_handle_zip *ebx);
static void zipin_fillBuf(streamInfo *eax, u8 *edi, u32 ecx);

/* zip.inc: open_file_zip: ebp = hPlugin, eax -> item, edi = mode */
void *open_file_zip(handle_common *p, file_common *eax, int edi)
{
	handle_zip *ebp = (handle_zip *)p;
	file_in_zip *ebx = (file_in_zip *)eax;
	/* what to free on an error: the decoder's block, then the file's
	 * handle and the memory stream (the original's hOut, hOut.allocated) */
	u8 *volatile block = 0;
	file_handle_zip *volatile fh = 0;
	mem_stream *volatile ms = 0;
	streamInfo *dec = 0;
	RETURN_CATCH(saved) {
		if (return_kind == RETURN_ERR) {
			/* .error */
			SayErr(1, aArchiveDataError_ptr, 1, ContinueBtn);
		}
		/* .clear */
		if (ms) pgfree(ms);
		if (block && !fh) pgfree(block);
		/* .1 */
		if (fh) close_file_zip(fh);
		return 0;
	}
	bPasswordDefined = ebp->password_len != -1;
	if (bPasswordDefined) {
		password_size = ebp->password_len;
		memcpy(password_ansi, ebp->password, password_size);
	}
	u32 method = ebx->method;
	if (method) {
		/* .notcopy */
		if (method != 8 && method != 9) {
			/* "Unknown method: N" */
			char *z = aUnknownMethod_z, d[12];
			int n = 0;
			*z++ = ' ';
			do d[n++] = '0' + method % 10; while ((method /= 10));
			while (n) *z++ = d[--n];
			*z = 0;
			RETURN_UNCATCH(saved);
			SayErr(1, aUnknownMethod_ptr, 1, ContinueBtn);
			return 0;                       /* .ret1 */
		}
		/* .deflate: the packed data's stream, its buffer, the decoder
		 * and its buffer in one block of 0x12000 bytes */
		block = pgalloc(0x12000);
		if (!block) {
			RETURN_UNCATCH(saved);
			return 0;
		}
		zipin *in = (zipin *)block;
		in->s.fullSize = ebx->compressed_size;
		in->s.fillBuf = zipin_fillBuf;
		in->s.bufSize = 0x2000 - sizeof(zipin) - sizeof(deflate_decoder);
		in->s.bufDataLen = 0;
		in->s.bufPtr = block + sizeof(zipin);
		deflate_decoder *d = (deflate_decoder *)(in->s.bufPtr + in->s.bufSize);
		d->outStream.fullSize = ebx->uncompressed_size;
		d->outStream.bufSize = 0x10000;
		d->outStream.bufDataLen = 0;
		d->inStream = &in->s;
		d->outStream.bufPtr = (u8 *)(d + 1);
		deflate_init_decoder(&d->outStream);
		d->bDeflate64 = ebx->method == 9;
		dec = &d->outStream;
	}
	/* .common */
	file_handle_zip *h = alloc_handle(0, sizeof(file_handle_zip));
	if (!h) return_clear();
	if (block) ((zipin *)block)->handle = h;
	h->type = type_zip;
	h->pos = 0;
	h->posc = 0;
	h->item = ebx;
	h->base = ebp;
	h->decoder = dec;
	h->bError = 0;
	h->bPassInited = 0;
	h->bPassProtected = ebx->flags & 1;
	h->start_disk = ebx->disk;
	h->cur_disk = ebx->disk;
	h->start = ebx->local_hdr;
	h->cur_disk_start = 0;
	fh = h;
	/* the local header, then the data after its name and extra field */
	u8 lh[file_header_sz_local];
	zip_read(h, lh, file_header_sz_local);
	if (get32(lh) != 0x04034B50) return_err();
	u32 esi = get16(lh + 26) + get16(lh + 28) + file_header_sz_local;
	zip_setpos(h, esi);
	zip_update_start(h, esi);
	if (h->bPassProtected) {
		for (;;) {
			/* .redo_pass */
			if (!query_password()) return_clear();
			zip_init_keys(h);
			zip_read(h, buffer, 12);
			if (buffer[11] == (u8)(ebx->crc >> 24)) break;
			if (SayErr(1, aArchiveDataErrorPass_ptr, 2, CancelPassBtn) != 1) return_clear();
			h->posc = 0;
			h->cur_disk_start = 0;
			h->cur_disk = h->start_disk;
			bPasswordDefined = 0;
		}
		/* .passok */
		ebp->password_len = password_size;
		memcpy(ebp->password, password_ansi, password_size);
		memcpy(h->saved_keys, h->keys, sizeof(h->keys));
		zip_update_start(h, 12);
	}
	/* .nopass */
	if (ebx->uncompressed_size >> 32) {
		RETURN_UNCATCH(saved);
		return h;
	}
	u32 ecx = ebx->uncompressed_size;
	if (((ecx + 0x3FF) >> 10) > getfreemem() >> 2) {
		RETURN_UNCATCH(saved);
		return h;
	}
	/* create memory stream and unpack to memory */
	ms = pgalloc(ecx + sizeof(mem_stream));
	if (!ms) return_clear();
	ms->type = type_mem_stream;
	ms->size = ecx;
	ms->pos = 0;
	if (read_zip(h, ms->buf, ecx) == (unsigned)-1) return_clear();     /* (read_zip has said why) */
	RETURN_UNCATCH(saved);
	close_file_zip(h);                      /* .1 */
	return ms;
}

/* zip.inc: close_file_zip */
void close_file_zip(void *p)
{
	file_handle_zip *ebx = p;
	if (ebx->decoder) pgfree(((decoder_common *)ebx->decoder)->inStream);  /* the block */
	free_handle(ebx);
}

/* zip.inc: zip.init_keys: the traditional PKWARE encryption's keys from
 * the password */
static void zip_update_keys(file_handle_zip *ebx, u8 al);
static void zip_init_keys(file_handle_zip *ebx)
{
	ebx->keys[0] = 305419896;
	ebx->keys[1] = 591751049;
	ebx->keys[2] = 878082192;
	for (u32 i = 0; i < password_size; i++) zip_update_keys(ebx, password_ansi[i]);
	ebx->bPassInited = 1;
}

/* zip.inc: zip.update_keys */
static void zip_update_keys(file_handle_zip *ebx, u8 al)
{
	ebx->keys[0] = ebx->keys[0] >> 8 ^ crc_table[(u8)(ebx->keys[0] ^ al)];
	ebx->keys[1] = (ebx->keys[1] + (u8)ebx->keys[0]) * 134775813 + 1;
	ebx->keys[2] = ebx->keys[2] >> 8 ^ crc_table[(u8)(ebx->keys[2] ^ ebx->keys[1] >> 24)];
}

/* zip.inc: zip.decrypt_byte: out: ah = result */
static u8 zip_decrypt_byte(file_handle_zip *ebx)
{
	u32 eax = ebx->keys[2] | 2;
	return eax * (eax ^ 1) >> 8;
}

/* zip.inc: zip.get_cur_handle: the handle of the part cur_disk */
static HANDLE zip_get_cur_handle(file_handle_zip *ebx)
{
	handle_zip *ebp = ebx->base;
	if (ebp->num_disks - 1 == ebx->cur_disk) return ebp->host;
	zip_num_disks = ebp->num_disks;
	if (ebp->cur_disk == ebx->cur_disk) return ebp->cur_handle;
	zip_cur_handle = ebp->cur_handle;
	HANDLE eax = zip_open_splitted(ebx->cur_disk, ebp->host_idPlugin, ebp->host_hPlugin, ebp->names_buf, 1);
	ebp->cur_handle = eax;
	ebp->cur_disk = eax ? ebx->cur_disk : (u32)-1;
	if (!eax) {
		const char *strings[2] = { aCannotOpenFile, zip_disk_name };
		SayErr(2, strings, 1, ContinueBtn);
		return_clear();
	}
	return eax;
}

/* zip.inc: zip.read: in: ebx = hFile, edi -> buf, ecx = size: the packed
 * data at posc (decrypted); returns edi after it */
static u8 *zip_read(file_handle_zip *ebx, u8 *edi, u32 ecx)
{
	u8 *start = edi;
	for (;;) {
		/* .s */
		HANDLE edx = zip_get_cur_handle(ebx);
		u64 p = ebx->posc - ebx->cur_disk_start;
		if (ebx->cur_disk == ebx->start_disk) p += ebx->start;
		seek(edx, 0, p);
		u32 eax = read(edx, edi, ecx);
		if (eax == (u32)-1) {
			/* .error */
			SayErr(1, aReadError_ptr, 1, ContinueBtn);
			return_clear();
		}
		edi += eax;
		ebx->posc += eax;
		ecx -= eax;
		if (!ecx) break;
		/* the rest in the next part */
		ebx->cur_disk++;
		ebx->cur_disk_start = ebx->posc;
	}
	/* .ok */
	if (ebx->bPassInited)
		for (u8 *q = start; q < edi; q++) {
			/* .decrypt */
			*q ^= zip_decrypt_byte(ebx);
			zip_update_keys(ebx, *q);
		}
	return edi;
}

/* zip.inc: zip.setpos: in: ebx = hFile, edi:esi = newpos (in the packed
 * data): the part and its start for it; encrypted data is read up to it
 * (from the start again if it is behind) */
static void zip_setpos(file_handle_zip *ebx, u64 newpos)
{
	if (ebx->bPassInited) {
		/* .pass */
		if (newpos < ebx->posc) {
			memcpy(ebx->keys, ebx->saved_keys, sizeof(ebx->keys));
			ebx->posc = 0;
			ebx->cur_disk = ebx->start_disk;
			ebx->cur_disk_start = 0;
		}
		while (newpos != ebx->posc) {
			/* .pfwd */
			u64 d = newpos - ebx->posc;
			zip_read(ebx, buffer, d < 1024 ? d : 1024);
		}
		return;
	}
	if (newpos < ebx->cur_disk_start) {
		ebx->cur_disk = ebx->start_disk;
		ebx->cur_disk_start = 0;
	}
	for (;;) {
		/* .fwd */
		u64 sz = filesize(zip_get_cur_handle(ebx));
		if (ebx->cur_disk == ebx->start_disk) sz -= ebx->start;
		if (newpos - ebx->cur_disk_start < sz) break;
		ebx->cur_disk++;
		ebx->cur_disk_start += sz;
	}
	ebx->posc = newpos;
}

/* zip.inc: zip.update_start: edi = 0, esi = pos: the data starts here */
static void zip_update_start(file_handle_zip *ebx, u32 esi)
{
	esi -= (u32)ebx->cur_disk_start;
	ebx->cur_disk_start = 0;
	ebx->start += esi;
	ebx->posc = 0;
	if (ebx->cur_disk != ebx->start_disk) {
		ebx->start_disk = ebx->cur_disk;
		ebx->start = esi;
	}
}

/* unsigned __stdcall read(HANDLE hFile, void* buf, unsigned size) */
unsigned read_zip(void *p, void *buf, unsigned size)
{
	file_handle_zip *volatile ebx = p;
	RETURN_CATCH(saved) {
		if (return_kind == RETURN_ERR) {
			/* .error */
			SayErr(1, aArchiveDataError_ptr, 1, ContinueBtn);
		}
		/* .clear */
		ebx->bError = 1;
		return (unsigned)-1;
	}
	if (ebx->bError) {
		RETURN_UNCATCH(saved);
		return (unsigned)-1;            /* .reterr */
	}
	u8 *edi = buf;
	u32 ecx = size;
	u64 rest = ebx->item->uncompressed_size - ebx->pos;
	if (rest < ecx) ecx = rest;
	if (ecx) {
		streamInfo *eax = ebx->decoder;
		if (!eax) {
			edi = zip_read(ebx, edi, ecx);
		} else {
			/* .notcopy */
			eax->fullSize = ecx;
			edi = read_7z_to_buf(eax, edi);
		}
	}
	/* .done */
	RETURN_UNCATCH(saved);
	u32 n = edi - (u8 *)buf;
	ebx->pos += n;
	return n;
}

/* zip.inc: zipin.fillBuf */
static void zipin_fillBuf(streamInfo *eax, u8 *edi, u32 ecx)
{
	zip_read(((zipin *)eax)->handle, edi, ecx);
}

/* void __stdcall setpos(HANDLE hFile, __int64 pos) */
void setpos_zip(void *p, u64 pos)
{
	file_handle_zip *volatile ebx = p;
	RETURN_CATCH(saved) {
		if (return_kind == RETURN_ERR) SayErr(1, aArchiveDataError_ptr, 1, ContinueBtn);
		ebx->bError = 1;
		return;
	}
	if (!ebx->decoder) {
		zip_setpos(ebx, pos);
		ebx->pos = pos;
	} else if (!ebx->bError && pos >= ebx->pos) {
		/* move forward: decode and drop */
		ebx->decoder->fullSize = pos - ebx->pos;
		skip_7z(ebx->decoder);
		ebx->pos = pos;
	} else {
		/* .backward: the decoder from the start again */
		ebx->bError = 0;
		ebx->pos = 0;
		ebx->posc = 0;
		ebx->cur_disk_start = 0;
		memcpy(ebx->keys, ebx->saved_keys, sizeof(ebx->keys));
		ebx->cur_disk = ebx->start_disk;
		deflate_decoder *d = (deflate_decoder *)ebx->decoder;
		streamInfo *in = d->inStream;
		in->fullSize = ebx->item->compressed_size;
		in->bufDataLen = 0;
		d->outStream.bufDataLen = 0;
		deflate_init_decoder(&d->outStream);
		d->bDeflate64 = ebx->item->method == 9;
		d->outStream.fullSize = pos;
		skip_7z(&d->outStream);
		ebx->pos = pos;
	}
	RETURN_UNCATCH(saved);
}

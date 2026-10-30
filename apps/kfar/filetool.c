/* kfar for KolibriOS for ARM: the delete, the copy and the folders of
 * the panels (F5, F6, F7, F8), and kfar's file layer (_FILE, open, read,
 * seek, ...) that the panels, the viewer and the editor read through.
 *
 * Translated to C from kfar's filetool.inc (delete_file_worker ...
 * copy_AddFile, _FILE ... getattr_default), diamond (Evgeny Grechnikov)
 * and the KolibriOS team, GPL-2.0. The file search of filetool.inc is in
 * filesearch.c.
 *
 * The original's results in the flags become return values:
 * delete_file_worker returns 0 OK, 1 retry, 2 skip, -1 cancel (the
 * original's "cmp al, 2"); delete_file, copy_file and copy_file_worker
 * return true for "cancel the job" (the original's "ja cancel_label").
 * The plugins' branches are the original's: source_hModule is the
 * source panel's plugin (its PluginInfo), source_hPlugin the plugin's
 * handle, source_hFile the file being copied; copy_AddDir and
 * copy_AddFile are the callbacks of a plugin's GetFiles; the file layer
 * reads a plugin's files through its open, read, setpos, close. */
#include "kfar.h"

/* kfar.asm: panels_OnKey.bigfilename (with .l2's restore_semicolon):
 * "Full file name is too long" */
static void bigfilename(void)
{
	char *esi = restore_semicolon;
	restore_semicolon = 0;
	if (esi) *esi = ';';
	const char *eax = aFileNameTooBig;
	SayErr(1, &eax, 1, ContinueBtn);
}

/* filetool.inc: delete_file_worker: delinfo (execdata) deleted; in:
 * ecx = flags: 1 = deleting a folder; out: 0 OK, 1 retry, 2 skip, -1
 * cancel */
static int delete_file_worker(int ecx)
{
	u32 eax = fs_call(&delinfo, 0);
	if (!eax) return 0;
	if (del_bSkipAll) return 2;
	const char *strings[3] = { get_error_msg(eax), ecx & 1 ? aCannotDeleteFolder : aCannotDeleteFile, execdata };
	int al = SayErr(3, strings, 4, DeleteErrorBtn);
	if (al != -1) {
		al++;                           /* 1 retry, 2 skip, 3 skip all, 4 cancel */
		if (al == 4) al = -1;           /* the "cancel" button */
	}
	if (al == 3) {                          /* the "skip all" button */
		del_bSkipAll = 1;
		al = 2;
	}
	return al;
}

/* filetool.inc: delete_file: in: eax -> the BDFE; out: true <=> cancel
 * the job */
bool delete_file(PanelData *ebp, u8 *eax)
{
	del_dir_stack_ptr = del_dir_stack;
	char *edi = execdata;
	for (const char *esi = ebp->dir; *esi;) *edi++ = *esi++;
	const char *esi = bdfe_name(eax);
	if (edi[-1] != '/') *edi++ = '/';
	/* .l1 */
	for (;;) {
		char al = *esi++;
		if (edi >= execdataend) {
			bigfilename();
			return false;
		}
		*edi++ = al;
		if (!al) break;
	}
	if (!(bdfe_attr(eax) & 0x10)) {
		/* .retrydel */
		int r;
		while ((r = delete_file_worker(0)) == 1);
		return r == -1;
	}
	/* .delete_dir: recursive delete of the folder */
	u32 n = 0;                              /* ebp: the number of undeletable items */
	int r;
delete_dir:
	n = 0;
return_from_recursion:
	dirinfo.first = n;
	dirinfo.size = del_dir_query_size;
	dirinfo.data = del_dir_query_area;
	dirinfo.name = execdata;
	u32 ebx;
	u32 err = fs_call(&dirinfo, &ebx);
	/* if we get read error, the best available action is try to delete
	 * the folder itself */
	if (err && err != 6) goto do_delete_dir;
	/* loop through the folder and delete items */
	u8 *edx = del_dir_query_area + 32;
	u8 *end = edx + ebx * 304;
	for (;; edx += 304) {
		/* .delete_dir_entry_loop */
		if (edx >= end) {
			if (end != del_dir_query_area + 32 + 304 * del_dir_query_size)
				goto do_delete_dir;     /* .delete_dir_entry_done */
			goto return_from_recursion;
		}
		/* .do_delete_dir_entry: ignore the special entries "." and ".." */
		if ((edx[40] == '.' && edx[41] == 0) || bdfe_is_dotdot(edx)) {
			n++;
			continue;
		}
		char *p = execdata + strlen(execdata);
		*p++ = '/';
		const char *s = bdfe_name(edx);
		bool big = false;
		for (;;) {
			if (p >= execdataend) {
				big = true;
				break;
			}
			if (!(*p++ = *s++)) break;
		}
		if (big) {
			/* .fullname_big: we will just ignore such files and
			 * continue - in real life this situation can not happen */
			n++;
			delete_last_name(execdataend - 1);
			continue;
		}
		if (bdfe_attr(edx) & 0x10) {
			/* .entry_is_folder: a new item in the folder stack */
			*del_dir_stack_ptr++ = n;
			/* do recursive deleting */
			goto delete_dir;
		}
		/* .retry2 */
		while ((r = delete_file_worker(0)) == 1);
		if (r == -1) goto cancel;
		if (r == 2) n++;                /* .skip */
		/* .restore_name */
		delete_last_name_from_end(execdata);
	}
do_delete_dir:
	r = delete_file_worker(1);
	if (r == -1) goto cancel;
	if (r == 1) goto delete_dir;
	/* al = 0: OK, al = 2: skip this folder; return to the parent folder:
	 * pop the item from the folder stack */
	if (del_dir_stack_ptr <= del_dir_stack) {
		dirinfo.first = 0;              /* .done */
		return false;
	}
	n = *--del_dir_stack_ptr;
	if (r == 2) n++;                        /* "cmp al, 2; sbb ebp, -1" */
	/* restore the parent's name */
	delete_last_name_from_end(execdata);
	goto return_from_recursion;
cancel:
	dirinfo.first = 0;
	return true;
}

/* filetool.inc: makedir: the folder CopyDestEditBuf.data; on an error a
 * message with the buttons; returns 0 when made, otherwise the button
 * chosen (-1 Esc); with DeleteErrorBtn and copy_bSkipAll: 1 (skip) */
int makedir(int num_buttons, const char **buttons)
{
	for (;;) {
		u32 eax = fs_call(&mkdirinfo, 0);
		if (!eax) return 0;
		if (buttons == DeleteErrorBtn && copy_bSkipAll) return 1;
		const char *strings[3] = { get_error_msg(eax), aCannotMakeFolder, CopyDestEditBuf.data };
		int r = SayErr(3, strings, num_buttons, buttons);
		if (r) return r;
		/* the first button: retry */
	}
}

/* copy_file_worker2's end: the plugin's file closed if it was opened
 * there (not GetFiles') */
static void close_source_hFile(void *own_hFile)
{
	PluginInfo *mod = source_hModule;
	if (!own_hFile && mod && source_hFile) mod->close(source_hFile);
}

/* filetool.inc: copy_file_worker2: in: execdata = the source's name,
 * CopyDestEditBuf.data = the destination's name, edx = the source's
 * BDFE, edi = the name to add to the destination when bDestIsFolder;
 * out: true <=> cancel the job */
static bool copy_file_worker2(const u8 *edx, const char *edi)
{
	/* the destination's name is restored at the end (the zero put back);
	 * without a name added, the original puts it at +513, one byte after
	 * the buffer (SearchStringEditBuf's maxlength): here its last byte */
	char *restore = CopyDestEditBuf.data + 512;
	if (bDestIsFolder) {
		char *esi = CopyDestEditBuf.data + strlen(CopyDestEditBuf.data);
		restore = esi;
		if (esi[-1] != '/') *esi++ = '/';
		const char *s = edi;
		for (;;) {
			if (esi >= CopyDestEditBuf.data + 513) {
				*restore = 0;   /* .overflow: .ret_zf */
				return false;
			}
			if (!(*esi++ = *s++)) break;
		}
	}
	/* .noaddtoname: a file cannot be copied over itself! */
	const char *esi = execdata;
	char *dst = CopyDestEditBuf.data;
	if (!source_hModule && strcmpi(esi, dst) == 0) {
		const char *strings[2] = { aCannotCopyToSelf, esi };
		SayErr(2, strings, 1, ContinueBtn);
		*restore = 0;                   /* .ret_zf */
		return false;
	}
	/* the copy itself: esi -> the source's name, edi -> the destination's */
	writeinfo.code = 2;
	writeinfo.name = dst;
	writeinfo.first = writeinfo.first_hi = 0;
	writeinfo.data = copy_buffer;
	readinfo.first = readinfo.first_hi = 0;
	readinfo.size = copy_buffer_size;
	readinfo.data = copy_buffer;
	readinfo.name = esi;
	/* the source's handle: GetFiles' (copy_AddFile, the plugin closes
	 * it) or none: opened here from the plugin's panel (.source_reopen) */
	void *own_hFile = source_hFile;
	PluginInfo *mod = source_hModule;
	if (!own_hFile && mod) source_hFile = mod->open(source_hPlugin, esi, O_READ + O_SEQUENTIAL_ONLY);
	bool cancel;
	int al;
	for (;;) {
		/* .copyloop */
		u32 ebx, eax;
		if (mod) {
			/* (a plugin's error: get_error_msg(-1), as the original's
			 * read result) */
			eax = source_hFile ? mod->read(source_hFile, readinfo.data, readinfo.size) : (u32)-1;
			ebx = eax;
			if (eax != (u32)-1) eax = 0;
		} else {
			/* .native */
			eax = fs_call(&readinfo, &ebx);
		}
		if (eax && eax != 6) {
			/* .readerr */
			if (copy_bSkipAll2) goto copyfailed_del2;
			const char *strings[3] = { get_error_msg(eax), aCannotReadFile, esi };
			al = SayErr(3, strings, 4, DeleteErrorBtn);
			if (!al) {              /* retry */
				if (mod && !source_hFile)
					source_hFile = mod->open(source_hPlugin, esi, O_READ + O_SEQUENTIAL_ONLY);
				continue;
			}
			goto copyfailed_parseuser;
		}
		/* .copyreadok */
		u64 first = (readinfo.first | (u64)readinfo.first_hi << 32) + ebx;
		readinfo.first = first;
		readinfo.first_hi = first >> 32;
		writeinfo.size = ebx;
		if (!ebx && writeinfo.code != 2) break;         /* .copydone */
		/* .copywrite */
		for (;;) {
			eax = fs_call(&writeinfo, 0);
			if (!eax) break;
			if (copy_bSkipAll2) goto copyfailed_del2;
			const char *strings[3] = { get_error_msg(eax), aCannotWriteFile, dst };
			al = SayErr(3, strings, 4, DeleteErrorBtn);
			if (al) goto copyfailed_parseuser;
		}
		/* .copywriteok */
		first = (writeinfo.first | (u64)writeinfo.first_hi << 32) + writeinfo.size;
		writeinfo.first = first;
		writeinfo.first_hi = first >> 32;
		writeinfo.code = 3;
		if (writeinfo.size != copy_buffer_size) break;
	}
	/* .copydone */
	close_source_hFile(own_hFile);
	/* now try to set the source's attributes, ignore errors */
	memcpy(attrinfo_attr, edx, 32);
	/* replace zero dates with default values */
	u8 *def = default_attr;
	if (!*(u32 *)(attrinfo_attr + 8)) memcpy(attrinfo_attr + 8, def + 8, 8);
	if (!*(u32 *)(attrinfo_attr + 16)) memcpy(attrinfo_attr + 16, def + 16, 8);
	if (!*(u32 *)(attrinfo_attr + 24)) memcpy(attrinfo_attr + 24, def + 24, 8);
	attrinfo.name = CopyDestEditBuf.data;
	attrinfo.code++;                        /* 6: set the attributes */
	fs_call(&attrinfo, 0);
	attrinfo.code--;
	*restore = 0;                           /* .ret */
	return false;

copyfailed_parseuser:
	if (al == 2) {                          /* "skip all" */
		copy_bSkipAll2 = 1;
		al = 1;
	}
	cancel = al != 1;                       /* "cmp al, 1": skip, or cancel */
	/* .copyfailed */
	close_source_hFile(own_hFile);
	if (!bConfirmDeleteIncomplete) goto copyfailed_del;
	if (writeinfo.code == 2) goto copydone2;
	{
		const char *strings[1] = { aIncompleteFile };
		if (Message(aCopyCaption, 1, strings, 2, DeleteOrKeepBtn)) goto copydone2;
	}
copyfailed_del:
	/* the incomplete destination deleted, the errors ignored */
	delinfo.name = dst;
	fs_call(&delinfo, 0);
	delinfo.name = execdata;
copydone2:
	*restore = 0;
	return cancel;

copyfailed_del2:
	/* (the original jumps here with the source's handle still on its
	 * stack, which unbalances it, and the plugin's file open; here:
	 * skip, as intended) */
	close_source_hFile(own_hFile);
	cancel = false;
	goto copyfailed_del;
}

/* filetool.inc: copy_file_worker: copy_file_worker2 with the source's
 * own name */
static bool copy_file_worker(const u8 *edx)
{
	source_hFile = 0;
	return copy_file_worker2(edx, bdfe_name(edx));
}

/* filetool.inc: recursive_read_folder_err: returns the button chosen
 * (0 retry, 1 skip, 2 skip all, 3 cancel, -1 Esc) */
static int recursive_read_folder_err(const char *edx, u32 eax)
{
	const char *strings[3] = { get_error_msg(eax), aCannotReadFolder, edx };
	return SayErr(3, strings, 4, DeleteErrorBtn);
}

/* filetool.inc: copy_file: in: eax -> the source's BDFE,
 * CopyDestEditBuf.data the full name of the destination; out: true <=>
 * cancel the job */
bool copy_file(PanelData *ebp, u8 *eax)
{
	copy_dir_stack_ptr = copy_dir_stack;
	bNeedRestoreName = 0;
	char *edi = execdata;
	for (const char *s = ebp->dir; *s;) *edi++ = *s++;
	const char *esi = bdfe_name(eax);
	if (edi[-1] != '/') *edi++ = '/';
	/* .l1 */
	for (;;) {
		char al = *esi++;
		if (edi >= execdataend) {
			bigfilename();
			return false;           /* stc: not "ja" */
		}
		*edi++ = al;
		if (!al) break;
	}
	u8 *edx = eax;
	if (!(bdfe_attr(edx) & 0x10))
		return copy_file_worker(edx);
	/* .copy_dir: recursive copy of the folder */
	u32 n;                                  /* ebp: the number of copied items */
	int al;
	char *p;
	const char *s;
	bool was_folder = bDestIsFolder;
	bDestIsFolder = 0;
	if (!was_folder) goto target_created;
	bNeedRestoreName = 1;
	p = CopyDestEditBuf.data + strlen(CopyDestEditBuf.data);
	if (p[-1] != '/') *p++ = '/';
	s = bdfe_name(edx);
	for (;;) {
		if (p >= CopyDestEditBuf.data + 513) {
			p[-1] = 0;              /* .biiig */
			return false;
		}
		if (!(*p++ = *s++)) break;
	}
enter_recursion:
	/* .create_target */
	al = makedir(4, DeleteErrorBtn);
	if (al) {
		if (al == 2) {
			al = 1;
			copy_bSkipAll = 1;
		}
		if (al == 1) goto copy_dir_entry_done;
		goto cancel2;
	}
target_created:
	if (source_hModule) {
		/* into the folder on the plugin's panel */
		for (;;) {
			const char *rel = strrchr(execdata, '/') + 1;
			if (((PluginInfo *)source_hModule)->SetFolder(source_hPlugin, rel, execdata)) break;
			/* (the original skips quietly unless "skip all" was
			 * chosen, and asks then: the test reversed) */
			if (copy_bSkipAll3) goto skip2;
			const char *strings[2] = { aCannotSetFolder, execdata };
			al = SayErr(2, strings, 4, DeleteErrorBtn);
			if (!al) continue;      /* retry */
			copy_bSkipAll3 = al == 2;
			if (al == 1 || al == 2) goto skip2;
			goto cancel2;
		}
	}
	/* .nosetdir */
	n = 0;
return_from_recursion:
	for (;;) {
		/* .read_retry */
		dirinfo.first = n;
		dirinfo.size = copy_dir_query_size;
		dirinfo.data = copy_dir_query_area;
		dirinfo.name = execdata;
		u32 ebx, err;
		if (source_hModule) {
			err = ((PluginInfo *)source_hModule)->ReadFolder(source_hPlugin, n, copy_dir_query_size,
									 copy_dir_query_area);
			ebx = *(u32 *)(copy_dir_query_area + 4);
		} else {
			/* .readfolder_native */
			err = fs_call(&dirinfo, &ebx);
		}
		if (!err || err == 6) {
			/* .readok: loop through the folder and copy items */
			u8 *e = copy_dir_query_area + 32;
			u8 *end = e + ebx * 304;
			for (;; e += 304) {
				/* .copy_dir_entry_loop */
				if (e >= end) {
					if (end == copy_dir_query_area + 32 + copy_dir_query_size * 304)
						goto return_from_recursion;
					goto copy_dir_entry_done;
				}
				/* .do_copy_dir_entry */
				n++;
				/* ignore the special entries "." and ".." */
				if ((e[40] == '.' && e[41] == 0) || bdfe_is_dotdot(e)) continue;
				p = execdata + strlen(execdata);
				if (p[-1] != '/') *p++ = '/';
				s = bdfe_name(e);
				bool big = false;
				for (;;) {
					if (p >= execdataend) {
						big = true;
						break;
					}
					if (!(*p++ = *s++)) break;
				}
				if (big) {
					/* .fullname_big: we will just ignore such files
					 * and continue - in real life this situation
					 * can not happen */
					delete_last_name(execdataend - 1);   /* .do_restore_name2 */
					continue;
				}
				p = CopyDestEditBuf.data + strlen(CopyDestEditBuf.data);
				if (p[-1] != '/') *p++ = '/';
				s = bdfe_name(e);
				for (;;) {
					/* (the original's limit: CopyDestEditBuf+513) */
					if (p >= CopyDestEditBuf.data + 513 - 12) {
						big = true;
						break;
					}
					if (!(*p++ = *s++)) break;
				}
				if (big) {
					/* .fullname2_big */
					delete_last_name(CopyDestEditBuf.data + 512);   /* .restore_name2 */
					delete_last_name_from_end(execdata);
					continue;
				}
				if (bdfe_attr(e) & 0x10) {
					/* .entry_is_folder: a new item in the folder
					 * stack, then the recursive copy */
					*copy_dir_stack_ptr++ = n;
					goto enter_recursion;
				}
				if (copy_file_worker(e)) goto cancel3;
				/* .restore_name */
				delete_last_name_from_end(CopyDestEditBuf.data);
				delete_last_name_from_end(execdata);
			}
		}
		/* read error */
		if (copy_bSkipAll) break;               /* .skip1 */
		al = recursive_read_folder_err(execdata, err);
		if (!al) continue;                      /* retry */
		if (al == 2) {
			al = 1;
			copy_bSkipAll = 1;
		}
		if (al == 1) break;                     /* .skip1 */
		goto cancel;
	}
copy_dir_entry_done:
	/* .skip1: return to the parent folder */
	delete_last_name_from_end(execdata);
	if (source_hModule) ((PluginInfo *)source_hModule)->SetFolder(source_hPlugin, aDotDot, execdata);
	if (0) {
	skip2:
		delete_last_name_from_end(execdata);
	}
	/* pop the item from the folder stack */
	if (copy_dir_stack_ptr <= copy_dir_stack) {
		dirinfo.first = 0;                      /* .done */
		return false;
	}
	n = *--copy_dir_stack_ptr;
	/* restore the parent's name */
	delete_last_name_from_end(CopyDestEditBuf.data);
	goto return_from_recursion;
	/* depth: the folders entered on a plugin's panel, less one (the
	 * original's copy_dir_stack_ptr, which .cancel2 takes below the
	 * stack for the first folder not entered) */
	int depth;
cancel2:
	depth = copy_dir_stack_ptr - copy_dir_stack - 1;
	if (copy_dir_stack_ptr > copy_dir_stack) copy_dir_stack_ptr--;
	goto cancel3_depth;
cancel3:
	depth = copy_dir_stack_ptr - copy_dir_stack;
cancel3_depth:
	delete_last_name_from_end(execdata);
	goto cancel_depth;
cancel:
	depth = copy_dir_stack_ptr - copy_dir_stack;
cancel_depth:
	/* on a plugin's panel back to the folder of the start */
	if (source_hModule)
		for (; depth >= 0; depth--) {
			delete_last_name_from_end(execdata);
			((PluginInfo *)source_hModule)->SetFolder(source_hPlugin, aDotDot, execdata);
		}
	/* .cancel.ret */
	dirinfo.first = 0;
	return true;
}

/* filetool.inc: copy_AddDir: GetFiles' adddir: the folder name (relative
 * to the panel's) made in the destination; false: stop the copy */
bool copy_AddDir(const char *name, void *bdfe_info)
{
	bool eax = true;                        /* for "return true" */
	char *esi = CopyDestEditBuf.data + strlen(CopyDestEditBuf.data) + 1;
	if (esi[-2] == '/') esi--;
	char saved[4];
	memcpy(saved, esi - 1, 4);
	esi[-1] = '/';
	char *edi = esi;
	for (;;) {
		/* .0 */
		char al = *name++;
		*edi++ = al;
		if (edi >= CopyDestEditBuf.data + 512) goto done;
		if (!al) break;
	}
	if (makedir(2, RetryOrCancelBtn)) eax = false;
done:
	memcpy(esi - 1, saved, 4);
	return eax;
}

/* filetool.inc: copy_AddFile: GetFiles' addfile: the plugin's file hFile
 * (the plugin opens and closes it) copied as name; false: stop */
bool copy_AddFile(const char *name, void *bdfe_info, HANDLE hFile)
{
	source_hFile = hFile;
	return !copy_file_worker2(bdfe_info, name);
}

/* filetool.inc: delete_last_name_from_end: esi -> the name */
void delete_last_name_from_end(char *esi)
{
	esi += strlen(esi);
	delete_last_name(esi - 1);
}

/* filetool.inc: delete_last_name: esi -> in the last part of the name:
 * the name cut at its last '/' (the root's '/' kept) */
void delete_last_name(char *esi)
{
	while (*esi-- != '/');
	esi++;
	if (esi != execdata && esi != CopyDestEditBuf.data) esi--;
	esi[1] = 0;
}

/* ---- filetool.inc: the file layer ---- */

struct _FILE {
	u64 pos;
	u64 bufpos;
	u32 bufsize;
	u32 mode;
	void *hPlugin;
	void *hFile;
	fileinfo fileinfo;      /* fimode, fioffset, fisize, fibuf, the name */
	char finame[1024];
	u8 attr[40] __attribute__((aligned(4)));   /* (the fileinfo before it is packed) */
	u8 buf[2048] __attribute__((aligned(512)));
};

/* HANDLE __stdcall open(const char* name, int mode): opens a physical
 * file */
_FILE *open(const char *name, int mode)
{
	_FILE *ecx = xpgalloc(sizeof(_FILE));
	if (!ecx) return 0;
	if (strlen(name) >= sizeof(ecx->finame)) {
		pgfree(ecx);                    /* .ret0 */
		return 0;
	}
	strcpy(ecx->finame, name);
	ecx->mode = mode;
	fileinfo *ebx = &ecx->fileinfo;
	ebx->name = ecx->finame;
	for (;;) {
		/* .getattr */
		ebx->code = 5;
		ebx->first = ebx->first_hi = 0;
		ebx->size = 0;
		ebx->data = ecx->attr;
		u32 eax = fs_call(ebx, 0);
		if (eax == 0) {
			/* .found */
			if (!(ecx->mode & O_TRUNCATE) || (!*(u32 *)(ecx->attr + 36) && !*(u32 *)(ecx->attr + 32)))
				break;
		} else if (eax != 5 || !(ecx->mode & O_CREATE)) {
			pgfree(ecx);            /* .ret0 */
			return 0;
		}
		/* file does not exist (or O_TRUNCATE): .truncate */
		ebx->code = 2;
		if (fs_call(ebx, 0)) {
			pgfree(ecx);
			return 0;
		}
	}
	ecx->pos = 0;
	ecx->bufpos = 0;
	ecx->bufsize = 0;
	ecx->hPlugin = 0;
	ecx->hFile = 0;
	ebx->first = ebx->first_hi = 0;         /* fioffset */
	return ecx;
}

/* HANDLE __stdcall open2(int plugin_id, HANDLE plugin_instance, const
 * char* name, int mode): opens a file on a plugin's panel (plugin_id
 * its PluginInfo, plugin_instance its handle), or a physical file
 * (plugin_id 0). The plugin's getattr fills attr (the original hands it
 * the fileinfo instead, and filesize then says 0) */
_FILE *open2(void *plugin_id, void *plugin_instance, const char *name, int mode)
{
	if (!plugin_id) return open(name, mode);
	/* .plugin */
	_FILE *ecx = xpgalloc(sizeof(_FILE));
	if (!ecx) return 0;
	if (strlen(name) >= sizeof(ecx->finame)) {
		pgfree(ecx);                    /* .ret0 */
		return 0;
	}
	strcpy(ecx->finame, name);
	PluginInfo *edx = plugin_id;
	ecx->hPlugin = edx;
	ecx->mode = mode;
	void *eax = edx->open(plugin_instance, ecx->finame, mode);
	if (!eax) {
		pgfree(ecx);
		return 0;
	}
	ecx->hFile = eax;
	memset(ecx->attr, 0, sizeof(ecx->attr));
	edx->getattr(plugin_instance, ecx->finame, ecx->attr);
	ecx->pos = 0;
	ecx->bufpos = 0;
	ecx->bufsize = 0;
	ecx->fileinfo.first = ecx->fileinfo.first_hi = 0;      /* fioffset */
	return ecx;
}

/* read.doread: fisize bytes at fioffset into fibuf; returns the error
 * (0, end of file included), *ebx the bytes read */
static u32 read_doread(_FILE *ecx, u32 *ebx)
{
	u32 eax;
	if (ecx->hPlugin) {
		eax = ((PluginInfo *)ecx->hPlugin)->read(ecx->hFile, ecx->fileinfo.data, ecx->fileinfo.size);
		if (eax == (u32)-1) return eax;
		*ebx = eax;
		eax = 0;
	} else {
		/* .native */
		eax = fs_call(&ecx->fileinfo, ebx);
		if (eax && eax != 6) return eax;
	}
	/* .addpos */
	u64 off = (ecx->fileinfo.first | (u64)ecx->fileinfo.first_hi << 32) + *ebx;
	ecx->fileinfo.first = off;
	ecx->fileinfo.first_hi = off >> 32;
	return 0;
}

static void read_bufpos_from_fioffset(_FILE *ecx)
{
	ecx->bufpos = ecx->fileinfo.first | (u64)ecx->fileinfo.first_hi << 32;
}

/* unsigned __stdcall read(HANDLE hFile, void* buf, unsigned size):
 * through a buffer of 2048 bytes, aligned reads straight into buf */
u32 read(_FILE *ecx, void *buf, u32 size)
{
	u32 result = 0;
	u8 *dst = buf;
	if (!(ecx->mode & O_READ) || !size) return 0;
	fileinfo *fi = &ecx->fileinfo;
	fi->code = 0;
	u32 ebx;
	u32 eax = ecx->pos & 2047;
	if (ecx->bufsize > eax) {
		ebx = ecx->bufsize - eax;
		if (ebx > size) ebx = size;
		memcpy(dst, ecx->buf + eax, ebx);
		dst += ebx;
		result += ebx;
		ecx->pos += ebx;
		if (!(ecx->pos & 2047)) ecx->bufsize = 0;
		size -= ebx;
		if (!size) return result;
	}
	/* .nobuf0 */
	if (ecx->pos & 2047) {
		if (ecx->bufsize) return result;
		fi->size = 2048;
		fi->data = ecx->buf;
		read_bufpos_from_fioffset(ecx);
		if (read_doread(ecx, &ebx)) return result;
		ecx->bufsize = ebx;
		eax = ecx->pos & 2047;
		if (ebx <= eax) return result;
		ebx -= eax;
		if (ebx > size) ebx = size;
		memcpy(dst, ecx->buf + eax, ebx);
		ecx->pos += ebx;
		dst += ebx;
		result += ebx;
		size -= ebx;
		if (!size || (ecx->pos & 2047)) return result;
	}
	/* .aligned */
	eax = size & ~2047u;
	if (eax) {
		ecx->bufsize = 0;
		fi->size = eax;
		fi->data = dst;
		if (read_doread(ecx, &ebx)) return result;
		ecx->pos += ebx;
		result += ebx;
		dst += ebx;
		size -= ebx;
		if (!size || ebx < fi->size) return result;
	}
	/* .finish */
	fi->size = 2048;
	fi->data = ecx->buf;
	ecx->bufsize = 0;
	read_bufpos_from_fioffset(ecx);
	if (read_doread(ecx, &ebx)) return result;
	ecx->bufsize = ebx;
	if (ebx > size) ebx = size;
	result += ebx;
	ecx->pos += ebx;
	memcpy(dst, ecx->buf, ebx);
	return result;
}

/* void __stdcall seek(HANDLE hFile, int method, __int64 newpos): method
 * 0 from the start, 1 from the position, 2 from the end */
void seek(_FILE *ecx, int method, s64 newpos)
{
	u64 pos = newpos;
	if (method == 1) pos += ecx->pos;
	else if ((u32)method > 1) pos += filesize(ecx);   /* .end */
	/* .set */
	ecx->pos = pos;
	u64 eax = pos & ~(u64)2047;
	if (eax != ecx->bufpos) {
		ecx->bufsize = 0;
		ecx->bufpos = eax;
	}
	/* .bufposok */
	if (ecx->bufsize) return;
	u64 fioffset = ecx->fileinfo.first | (u64)ecx->fileinfo.first_hi << 32;
	if (eax == fioffset) return;
	ecx->fileinfo.first = eax;
	ecx->fileinfo.first_hi = eax >> 32;
	if (ecx->hPlugin) ((PluginInfo *)ecx->hPlugin)->setpos(ecx->hFile, eax);
}

/* filetool.inc: setpos_default, the plugins' default setpos: seek from
 * the start (for plugins whose files are kfar's, read being kfar's too) */
void setpos_default(_FILE *hFile, s64 pos)
{
	seek(hFile, 0, pos);
}

/* __int64 __stdcall tell(HANDLE hFile) */
s64 tell(_FILE *hFile)
{
	return hFile->pos;
}

/* __int64 __stdcall filesize(HANDLE hFile) */
s64 filesize(_FILE *hFile)
{
	return *(u32 *)(hFile->attr + 32) | (u64)*(u32 *)(hFile->attr + 36) << 32;
}

/* void __stdcall close(HANDLE hFile) */
void close(_FILE *hFile)
{
	if (hFile->hPlugin) ((PluginInfo *)hFile->hPlugin)->close(hFile->hFile);
	pgfree(hFile);
}

/* filetool.inc: getattr_default, the plugins' default getattr: "not
 * supported" */
int getattr_default(void *hPlugin, const char *name, void *info)
{
	return 2;
}

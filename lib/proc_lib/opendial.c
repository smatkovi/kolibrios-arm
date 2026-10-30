/* KolibriOS for ARM: OpenDialog (lib/proc_lib/proc_lib.h), translated
 * to C from programs/develop/libraries/proc_lib/opendial.mac (Marat
 * Zakiyanov aka Mario79, use_OpenDialog: .init, .start, .set_file_name,
 * .set_file_ext; GPL-2.0).
 *
 * The dialog is the program opendial (apps/opendial). OpenDialog_init
 * makes the shared memory "PPPPPPPP_open_dialog" of the caller's PID;
 * OpenDialog_start writes the dialog's type, its window's place (in the
 * middle of the caller's window), the folder, the file's name and the
 * filter there (OD_* in proc_lib.h), starts opendial with the area's
 * name as its parameter (70.7) and polls the area's flag until opendial
 * writes 1 (the path chosen, at OD_PATH, the name at OD_NAME) or 3
 * (cancelled); the caller's window is redrawn (draw_window) on its
 * redraws meanwhile, and when it is the active one opendial's window is
 * activated again.
 *
 * What differs: the filter is copied whole (the original's ecx + 1 bytes
 * from the filter's size dword leave its last three bytes behind), a
 * caller without a filter or without the area does not read address 0,
 * and when opendial's process is gone without an answer (ended from
 * outside) the dialog counts as cancelled, where the original waits on. */
#include <string.h>
#include <kos.h>
#include "proc_lib.h"

static inline u16 rd16(const u8 *p) { return p[0] | p[1] << 8; }
static inline void wr16(u8 *p, u16 v) { p[0] = v; p[1] = v >> 8; }
static inline u32 rd32(const u8 *p) { return rd16(p) | (u32)rd16(p + 2) << 16; }
static inline void wr32(u8 *p, u32 v) { wr16(p, v); wr16(p + 2, v >> 16); }

/* .copy_dir_path: the path esi into edi, without its file's name (but
 * for a folder's dialog) */
static void copy_dir_path(open_dialog *od, const char *esi, char *edi)
{
	if (!esi || !edi) return;
	if (!*esi) {
		*edi = 0;
		return;
	}
	strcpy(edi, esi);
	if (od->type == 2) return;
	char *s = strrchr(edi, '/');
	if (s) *s = 0;
}

/* .init */
void OpenDialog_init(open_dialog *od)
{
	u8 *pi = od->procinfo;
	kos_call(9, (u32)pi, -1, 0, 0, 0);
	u32 pid = rd32(pi + 30);
	/* the PID into the name's first eight characters, backwards from the
	 * eighth: each byte's low nibble, then its high one, as '0' + the
	 * nibble (the original's; 10 to 15 become ':' to '?') */
	char *edi = od->com_area_name + 7;
	for (int i = 0; i < 4; i++, pid >>= 8) {
		*edi-- = '0' + (pid & 15);
		*edi-- = '0' + (pid >> 4 & 15);
	}
	od->com_area = kos_shm_open(od->com_area_name, OD_AREA_SIZE, KOS_SHM_CREATE | KOS_SHM_WRITE, NULL);
	copy_dir_path(od, od->openfile_path, od->opendir_path);
}

/* .get_filter_data */
static void get_filter_data(open_dialog *od, u8 *a)
{
	const u8 *f = od->filter_area;
	u32 n = f ? rd32(f) : 0;
	if (!n || n > OD_AREA_SIZE - OD_FILTER - 4) {   /* none, or longer than the area (4091) */
		wr32(a + OD_FILTER_FLAG, 0);
		return;
	}
	memcpy(a + OD_FILTER, f, n + 4);
	wr32(a + OD_FILTER_FLAG, 1);
}

/* .prepare_PID: the caller's slot */
static int prepare_PID(open_dialog *od)
{
	kos_call(9, (u32)od->procinfo, -1, 0, 0, 0);
	return kos_slot_of_pid(rd32((u8 *)od->procinfo + 30));
}

/* half the caller's window's size, its corner, half the dialog's: the
 * dialog's corner, 0 if that is negative (16 bits, as there) */
static u16 centre(u32 start, u32 size, u16 dsize)
{
	u16 v = (u16)(size / 2 + start) - dsize / 2;
	return v & 0x8000 ? 0 : v;
}

/* .start */
void OpenDialog_start(open_dialog *od)
{
	u8 *a = od->com_area;
	if (!a) {
		od->status = 2;
		return;
	}
	wr16(a + OD_TYPE, od->type);
	wr16(a + OD_FLAG, 2);                  /* folder for open */
	u8 *pi = od->procinfo;
	kos_call(9, (u32)pi, -1, 0, 0, 0);
	od->x_start = centre(rd32(pi + 34), rd32(pi + 42), od->x_size);
	od->y_start = centre(rd32(pi + 38), rd32(pi + 46), od->y_size);
	wr32(a + OD_X, (u32)od->x_start << 16 | od->x_size);
	wr32(a + OD_Y, (u32)od->y_start << 16 | od->y_size);
	const char *dir = od->opendir_path && od->opendir_path[0] ? od->opendir_path : od->dir_default_path;
	strcpy((char *)a + OD_PATH, dir ? dir : "");
	if (od->type != 2) {
		/* the file's name into the area */
		strcpy((char *)a + OD_NAME, od->filename_area ? od->filename_area : "");
		get_filter_data(od, a);
	}
	int pid = kos_run(od->start_path, od->com_area_name);
	if (pid < 0) {
		od->status = 2;
		goto out;
	}
	for (;;) {                             /* .still */
		/* the caller's window may not be there really: functions 5 and
		 * 11 (event 1 stays until function 0 is called otherwise) */
		kos_call(68, 1, 0, 0, 0, 0);
		kos_delay(10);
		int ev = kos_check_event();
		if (ev == 1) {                     /* .draw */
			if (od->draw_window) od->draw_window();
			int slot = prepare_PID(od);
			if (slot && kos_active_slot() == slot && rd32(a + OD_SLOT))
				kos_activate_slot(rd32(a + OD_SLOT));
		} else if (ev == 2) {
			kos_get_key();
		} else if (ev == 3) {
			kos_get_button();
		}
		/* .no_draw */
		u16 flag = rd16(a + OD_FLAG);
		if (flag == 3) {
			od->status = 0;
			break;
		}
		if (flag == 1) {
			strcpy(od->openfile_path, (char *)a + OD_PATH);
			od->status = 1;
			copy_dir_path(od, od->openfile_path, od->opendir_path);
			if (od->type != 2 && od->filename_area)   /* the file's name from the area */
				strcpy(od->filename_area, (char *)a + OD_NAME);
			break;
		}
		if (!kos_slot_of_pid(pid)) {       /* (an addition: opendial gone without an answer) */
			od->status = 0;
			break;
		}
	}
out:                                       /* .3: its window as it was left */
	od->x_size = rd16(a + OD_X);
	od->x_start = rd16(a + OD_X + 2);
	od->y_size = rd16(a + OD_Y);
	od->y_start = rd16(a + OD_Y + 2);
	int slot = prepare_PID(od);
	if (slot) kos_activate_slot(slot);
}

/* .set_file_name: the name into filename_area (256 bytes) */
void OpenDialog_set_file_name(open_dialog *od, const char *name)
{
	strncpy(od->filename_area, name, 255);
	od->filename_area[255] = 0;
}

/* .set_file_ext: the extension after the name's last '.' replaced (a
 * name without one stays) */
void OpenDialog_set_file_ext(open_dialog *od, const char *ext)
{
	char *dot = strrchr(od->filename_area, '.');
	if (!dot) return;
	strcpy(dot + 1, ext);
}

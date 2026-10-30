/* KolibriOS for ARM: proc_lib's OpenDialog, the dialog to open and save
 * files (programs/develop/libraries/proc_lib/opendial.mac, Marat
 * Zakiyanov aka Mario79; GPL-2.0), with its structure and functions.
 *
 * As in KolibriOS: OpenDialog_init opens the named shared memory (68.22)
 * of the caller's PID; OpenDialog_start fills it, starts the program
 * opendial (start_path, apps/opendial) with the area's name as its
 * parameter (70.7) and waits for its answer there, redrawing the
 * caller's window (draw_window) meanwhile. */
#ifndef PROC_LIB_H
#define PROC_LIB_H

typedef struct open_dialog {
	unsigned type;                 /* +0  0 open, 1 save, 2 a folder */
	void *procinfo;                /* +4  a buffer for function 9 (1 KiB) */
	char *com_area_name;           /* +8  the area's name: "FFFFFFFF_open_dialog", the PID put in */
	void *com_area;                /* +12 the area (OpenDialog_init) */
	char *opendir_path;            /* +16 the folder shown first (kept for the next time) */
	char *dir_default_path;        /* +20 ... if that is empty */
	char *start_path;              /* +24 opendial's path */
	void (*draw_window)(void);     /* +28 the caller's redraw */
	unsigned status;               /* +32 0 cancelled, 1 chosen, 2 the dialog did not start */
	char *openfile_path;           /* +36 the path chosen (4 KiB) */
	char *filename_area;           /* +40 the file's name (in and out, 256 bytes) */
	void *filter_area;             /* +44 u32 size, then extensions ended by 0; or NULL */
	unsigned short x_size;         /* +48 its window, as it was left last time */
	unsigned short x_start;        /* +50 */
	unsigned short y_size;         /* +52 */
	unsigned short y_start;        /* +54 */
} open_dialog;   /* the C layer's name (kolibri_opendialog.h) */

void OpenDialog_init(open_dialog *od);
void OpenDialog_start(open_dialog *od);
void OpenDialog_set_file_name(open_dialog *od, const char *name);
void OpenDialog_set_file_ext(open_dialog *od, const char *ext);

/* the communication area (opendial.mac's and opendial.asm's "Some
 * documentation for memory"), 8 KiB */
#define OD_AREA_SIZE 8192
#define OD_FLAG 0              /* u16: 0 empty, 2 the dialog runs ("folder for open"), 1 a file chosen, 3 cancelled */
#define OD_TYPE 2              /* u16: the dialog's type */
#define OD_X 4                 /* u32: x_start << 16 | x_size of its window */
#define OD_Y 8                 /* u32: y_start << 16 | y_size */
#define OD_SLOT 12             /* u32: opendial's window's slot */
#define OD_PATH 16             /* the folder to show; the path chosen (3824 bytes) */
#define OD_NAME 3840           /* the file's name (256) */
#define OD_FILTER_FLAG 4096    /* u32: 1 a filter follows */
#define OD_FILTER 4100         /* the filter: u32 its size, the extensions (4092) */

#endif

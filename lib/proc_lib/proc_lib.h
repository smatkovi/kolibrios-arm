/* KolibriOS for ARM: proc_lib's OpenDialog, the dialog to open and save
 * files (programs/develop/libraries/proc_lib/opendial.mac, Marat
 * Zakiyanov aka Mario79; the dialog itself programs/fs/opendial, the
 * KolibriOS team; GPL-2.0), with its structure and functions.
 *
 * In KolibriOS OpenDialog_start starts the program opendial, talks to it
 * through shared memory (68.22) and waits, redrawing the caller's window
 * (draw_window) meanwhile. This kernel has neither shared memory nor
 * threads, so the dialog runs in the caller's window: OpenDialog_start
 * draws it over the window (draw_window first on each redraw), takes
 * the events until a file is chosen or the dialog cancelled, then
 * redraws the window without it. The fields of shared memory and the
 * dialog's program (com_area_name, com_area, start_path) are unused. */
#ifndef PROC_LIB_H
#define PROC_LIB_H

typedef struct open_dialog {
	unsigned type;                 /* +0  0 open, 1 save, 2 a folder */
	void *procinfo;                /* +4  a buffer for function 9 (1 KiB) */
	char *com_area_name;           /* +8  (unused) */
	void *com_area;                /* +12 (unused) */
	char *opendir_path;            /* +16 the folder shown first (kept for the next time) */
	char *dir_default_path;        /* +20 ... if that is empty */
	char *start_path;              /* +24 (opendial's path: unused) */
	void (*draw_window)(void);     /* +28 the caller's redraw */
	unsigned status;               /* +32 0 cancelled, 1 chosen, 2 the dialog did not start */
	char *openfile_path;           /* +36 the path chosen (4 KiB) */
	char *filename_area;           /* +40 the file's name (in and out, 256 bytes) */
	void *filter_area;             /* +44 u32 size, then extensions ended by 0, a 0; or NULL */
	unsigned short x_size;         /* +48 */
	unsigned short x_start;        /* +50 */
	unsigned short y_size;         /* +52 */
	unsigned short y_start;        /* +54 */
} open_dialog;   /* the C layer's name (kolibri_opendialog.h) */

void OpenDialog_init(open_dialog *od);
void OpenDialog_start(open_dialog *od);

#endif

/* KolibriOS for ARM: opendial, KolibriOS's open and save dialog,
 * translated to C from programs/fs/opendial/opendial.asm (Marat
 * Zakiyanov aka Mario79, 2009-2013, and the KolibriOS team; GPL-2.0).
 * The labels are the functions' names, the variables keep theirs.
 *
 * A program of its own, as there: proc_lib's OpenDialog_start
 * (lib/proc_lib) starts it with the name of the caller's shared memory as
 * its parameter. It reads the dialog's type (open, save, a folder), its
 * window's place, the folder, the file's name and the filter from there
 * (OD_* in proc_lib.h), shows its window ("Open Dialog", "Save Dialog",
 * "Select Dir": the folder's path, the menus "Select Disk", "Sort",
 * "Filter", the buttons Refresh and ^ (the folder above), the list of
 * the folder, the file's name when saving, Open/Save/Select and Cancel),
 * and when a file is chosen writes its path and name back with the flag
 * 1 (cancelled: 3), its window's place and size for the next time, and
 * ends. Without a parameter it shows /sys and answers no one.
 *
 * Keys (scan codes in the list, as there): Up, Down, Page Up, Page Down,
 * Home, End, Enter (into the folder, or the file), Backspace (the folder
 * above), Esc (cancel), Ctrl+R (read again), Alt+F1, Alt+F2, Alt+F3 (the
 * menus), Tab (the list and the name when saving, the name's box then
 * takes characters).
 *
 * What differs: box_lib's FileBrowser, menu bar and PathShow are drawn by
 * fb.c (opendial.h); the icons, libimg's /sys/icons18.png there, are
 * built in (tools/png2bgr.py); sort.obj's SortDir is a sort here with
 * its orders (name, extension, the newer first, the larger first; the
 * folders first; ".." stays on top); the disks' table holds longer
 * names than the original's 10 bytes an entry (the Linux personality's
 * /linux/home has 11); the filter's extensions are compared whole (the
 * original compares from any character on, so a suffix of one matches).
 * In the Hildon look the window is maximised like every program's: a
 * file chooser over the whole work area. */
#include <string.h>
#include <stdlib.h>
#include "opendial.h"

#define x_minimal_size 350
#define y_minimal_size 250
#define DEV_LEN 32                     /* a disk's path in the table */
#define DEV_MAX 20

extern char __params[], __path[];      /* lib/crt0.S: the header's param and path */
extern const unsigned char icons18[];  /* tools/png2bgr.py: 18 x 18 pixels, one under the other */
extern const unsigned icons18_w, icons18_h;

static inline u16 rd16(const u8 *p) { return p[0] | p[1] << 8; }
static inline void wr16(u8 *p, u16 v) { p[0] = v; p[1] = v >> 8; }
static inline u32 rd32(const u8 *p) { return rd16(p) | (u32)rd16(p + 2) << 16; }
static inline void wr32(u8 *p, u32 v) { wr16(p, v); wr16(p + 2, v >> 16); }

/* --- the data (opendial.asm's, from window_high on) --------------------- */

static u32 window_high, window_width, window_status;
static u32 active_process;             /* its window's slot */
static u32 sort_type = 2;
static u32 retrieved_devices_table_counter;
static u8 *communication_area;
static u32 open_dialog_type;           /* 0 open, 1 save, 2 a folder */
static const char *const open_dialog_title_pointer[] = { "Open Dialog", "Save Dialog", "Select Dir" };
static const char *const message_open_dialog_button[] = { " Open ", " Save ", "Select" };
static u32 N_error, error_type;
static const char *error_path;
static u32 error_window_x = 100 << 16 | 250, error_window_y = 100 << 16 | 120;
static u8 *image_file;                 /* the ini as read */
static u32 img_size;
static u8 extended_key, shift_flag, ctrl_flag, alt_flag, error_window, Tab_key, Tab_key_block;
static u8 filter_flag = 1;
static u8 focus_pointer;               /* 1: the file's name (saving), 0: the list */
static const char start_pach[] = "/sys";
static const char root_pach[] = "/";
static const char ini_file_name[] = "/sys/File managers/icon2ext.ini";
static const char message_cancel_button[] = "Cancel";
static const char message_ReloadDir_button[] = "Refresh";
static const char message_ExitDir_button[] = "^";
static const char message_file_name[] = "File name:";
static const char example_name_temp[] = "temp1.asm";
static const char load_ini_error_type[] = "Error loading INI file";
static const char memory_free_error_type[] = "Error of free memory";
static const char memory_get_error_type[] = "Memory allocation error";
static const char load_directory_error_type[] = "Error loading directory";
static const char error_help_text[] = "For continue press <Esc> key or <Cancel>";
static const char *const error_fs_text_pointers[12] = {
	"0 - Success full", "1 - Base and/or partition of a hard disk is not defined",
	"2 - Function is not supported for the given file system", "3 - Unknown file system",
	"4 - Reserved, is never returned in the current implementation", "5 - File not found",
	"6 - End of file, EOF", "7 - Pointer lies outside of application memory", "8 - Disk is full",
	"9 - FAT table is destroyed", "10 - Access denied", "11 - Device error",
};

static char menu_text_area_1[] = "Select Disk";
static char menu_text_area_2[] = "Sort\0Name\0Type\0Date\0Size\0";
static char menu_text_area_3[] = "Filter\0*.* - show all\0Only supported\0";
static char menu_text_area_1_1[DEV_MAX * DEV_LEN + 1];
static struct menu_bar menu_data_1 = {
	.type = 1, .size_x = 80, .start_x = 10, .size_y = 15, .start_y = 26,
	.text_pointer = menu_text_area_1, .pos_pointer = menu_text_area_1_1, .text_end = menu_text_area_1_1,
	.size_x1 = 80, .start_x1 = 10, .size_y1 = 100, .start_y1 = 41, .bckg_col = 0xffffff, .frnt_col = 0xff,
	.menu_col = 0xeef0ff, .interval = 16, .menu_sel_col = 0x00cc00, .frnt_text_col = 0xffffff, .font_height = 8,
};
static struct menu_bar menu_data_2 = {
	.type = 0, .size_x = 30, .start_x = 95, .size_y = 15, .start_y = 26,
	.text_pointer = menu_text_area_2, .pos_pointer = menu_text_area_2 + 5, .text_end = menu_text_area_2 + 25,
	.size_x1 = 30, .start_x1 = 95, .size_y1 = 100, .start_y1 = 41, .bckg_col = 0xffffff, .frnt_col = 0xff,
	.menu_col = 0xeef0ff, .interval = 16, .menu_sel_col = 0x00cc00, .frnt_text_col = 0xffffff, .font_height = 8,
};
static struct menu_bar menu_data_3 = {
	.type = 0, .size_x = 45, .start_x = 130, .size_y = 15, .start_y = 26,
	.text_pointer = menu_text_area_3, .pos_pointer = menu_text_area_3 + 7, .text_end = menu_text_area_3 + 37,
	.size_x1 = 95, .start_x1 = 130, .size_y1 = 100, .start_y1 = 41, .bckg_col = 0xffffff, .frnt_col = 0xff,
	.menu_col = 0xeef0ff, .interval = 16, .menu_sel_col = 0x00cc00, .frnt_text_col = 0xffffff, .font_height = 8,
};

static scrollbar scroll_bar_data_vertical = SCROLLBAR(15, 500, 300, 45, 15, 10, 2, 0, 0xeeeeee, 0xbbddff, 0, 1);

static struct file_browser file_browser_data_1 = {
	.size_x = 400, .start_x = 10, .size_y = 550, .start_y = 45, .icon_size_y = 16, .icon_size_x = 16,
	.line_size_y = 18, .background_color = 0xffffff, .select_color = 0xbbddff, .reduct_text_color = 0xff0000,
	.select_panel_counter = 1, .font_size_y = 9, .font_size_x = 6, .mouse_key_delay = 50,
};

static char text_work_area[1024];
static char dir_path[4096];
static struct path_show PathShow_data_1 = {
	.start_y = 11, .start_x = 10, .font_size_x = 6, .area_size_x = 200,
	.text_pointer = dir_path, .work_area_pointer = text_work_area,
};

static char user_selected_name[256];
static u32 mouse_dd;
/* (the original's 4095 characters at most, into its 256 bytes: 255 here) */
static edit_box edit1 = EDIT_BOX(200, 10, 7, 0xffffff, 0xbbddff, 0, 0, 0, 255, user_selected_name, &mouse_dd, 0, 0, 0);

/* window_x: x_start << 16 | x_size, window_y */
static u32 window_x = 10 << 16 | 420, window_y = 10 << 16 | 320;

static u8 do_not_draw_open_button_label;
static struct { u32 frame, grab, work_dark, work_light, grab_text, work, work_button, work_button_text, work_text,
		    work_graph; } app_colours;
#define w_work app_colours.work
#define w_work_button app_colours.work_button
#define w_work_button_text app_colours.work_button_text
#define w_work_text app_colours.work_text
static u32 open_button_coordinates;
static u32 error_handler_store_input_mode;
static u8 thread_stack[8192] __attribute__((aligned(16)));   /* the error window's thread */
static char retrieved_devices_table[DEV_MAX * DEV_LEN];
static char file_name[4096], previous_dir_path[4096], dir_path_temp[4096];
static char converted_ini_buffer[8192];
static u8 procinfo[1024];

/* dirinfo: function 70.1 (dirinfo.name: dir_path or dir_path_temp) */
static const char *dirinfo_name = dir_path;
static u8 *dirinfo_return;

static void draw_window(void);
static void draw_draw_file_browser1(void);
static void load_next_dir(void);
static void load_next_dir_1(void);
static void error_handler(void);

static void rect(int x, int y, int w, int h, u32 c)
{
	if (w > 0 && h > 0) kos_call(13, (u32)x << 16 | (u16)w, (u32)y << 16 | (u16)h, c & 0xffffff, 0, 0);
}

/* --- strings (copy_dir_name, copy_dir_path, copy_exit_dir) ------------- */

/* copy_dir_name: src into dst (when src is not empty), dst's old text
 * into previous_dir_path first (when that is not empty) */
static void copy_dir_name(char *dst, const char *src)
{
	if (dst[0] && dst != previous_dir_path) strcpy(previous_dir_path, dst);
	if (src[0]) strcpy(dst, src);
}

/* copy_dir_path: '/' and name after the path (one '/' after the root) */
static void copy_dir_path(char *path, const char *name)
{
	size_t n = strlen(path);
	if (n > 1) path[n++] = '/';
	else n = 1, path[0] = '/';
	strcpy(path + n, name);
}

/* copy_exit_dir: the path's last part off ("/rd" from "/rd/1", "/" from "/rd") */
static void copy_exit_dir(char *path)
{
	char *s = strrchr(path, '/');
	if (!s) return;
	if (s == path) s[1] = 0;
	else *s = 0;
}

/* char_todown: cp866 */
static u8 char_todown(u8 c)
{
	if (c >= 'A' && c <= 'Z') return c + 0x20;
	if (c >= 0x80 && c <= 0x8f) return c + 0x20;
	if (c >= 0x90 && c <= 0x9f) return c + 0x50;
	return c;
}

static int strcmpi866(const char *a, const char *b)
{
	for (;; a++, b++) {
		int r = (int)char_todown(*a) - (int)char_todown(*b);
		if (r || !*a) return r;
	}
}

/* --- the exits ---------------------------------------------------------- */

/* draw_error_window, thread_start: the error that ended the dialog in a
 * window of a thread of its own (the main thread ends meanwhile) */
static void type_title(void);
static void draw_error_window(void)
{
	kos_begin_draw();
	kos_call(0, error_window_x, error_window_y, 0x03ff0000, 0, 0);
	type_title();
	kos_call(4, 10 << 16 | 30, 0x90ffffff, (u32)(N_error ? (const char *)N_error : ""), 0, 0);
	if (error_path) kos_call(4, 10 << 16 | 50, 0x90ffffff, (u32)error_path, 0, 0);
	if (error_type) kos_call(4, 10 << 16 | 70, 0x90ffffff, error_type, 0, 0);
	kos_end_draw();
}

static void thread_start(void)
{
	/* N_error: 1 the ini, 3 freeing memory, 4 getting memory, 5 a folder */
	switch (N_error) {
	case 1: N_error = (u32)load_ini_error_type; error_path = ini_file_name; goto type;
	case 3: N_error = (u32)memory_free_error_type; error_path = NULL; error_type = 0; break;
	case 4: N_error = (u32)memory_get_error_type; error_path = NULL; error_type = 0; break;
	case 5: N_error = (u32)load_directory_error_type; error_path = dir_path; goto type;
	default: kos_exit();
	type:
		error_type = (u32)(error_type < 12 ? error_fs_text_pointers[error_type] : "");
	}
	for (;;) {                             /* .red, .still */
		int ev = kos_wait_event();
		if (ev == 1) draw_error_window();
		else if (ev == 2) kos_get_key();
		else if (ev == 3) kos_exit();      /* .button */
	}
}

/* start_error_window_thread: in the middle of the dialog's window */
static void start_error_window_thread(void)
{
	kos_call(9, (u32)procinfo, -1, 0, 0, 0);
	u16 y = rd32(procinfo + 46) / 2 + rd32(procinfo + 38) - 40;
	u16 x = rd32(procinfo + 42) / 2 + rd32(procinfo + 34) - 125;
	error_window_y = (u32)y << 16 | (error_window_y & 0xffff);
	error_window_x = (u32)x << 16 | (error_window_x & 0xffff);
	kos_thread_create(thread_start, thread_stack + sizeof(thread_stack));
}

/* get_window_param */
static void get_window_param(void)
{
	kos_call(9, (u32)procinfo, -1, 0, 0, 0);
	window_high = rd32(procinfo + 66) + 1;
	window_width = rd32(procinfo + 62) + 1;
	window_status = procinfo[70];
}

/* button.exit: cancelled (unless a file was chosen), the error's window,
 * the window's place and size into the area, the end */
static void __attribute__((noreturn)) button_exit(void)
{
	u8 *a = communication_area;
	if (a && rd16(a + OD_FLAG) != 1) wr16(a + OD_FLAG, 3);
	if (N_error) start_error_window_thread();
	get_window_param();
	if (a) {
		wr32(a + OD_X, rd32(procinfo + 34) << 16 | (u16)rd32(procinfo + 42));
		wr32(a + OD_Y, rd32(procinfo + 38) << 16 | (u16)rd32(procinfo + 46));
	}
	kos_exit();
	for (;;);
}

static void __attribute__((noreturn)) memory_get_error(void)
{
	N_error = 4;
	button_exit();
}

/* --- the folders --------------------------------------------------------- */

/* delete_point_dir: the entry "." off the start */
static void delete_point_dir(u8 *d)
{
	u8 *e = d + 32;
	u32 n = rd32(d + 4);
	if (n && e[40] == '.' && !e[41]) {
		memmove(e, e + 304, (n - 1) * 304);
		wr32(d + 4, n - 1);
	}
}

/* search_expansion, compare_expansion: the file's extension among the
 * filter's (the area's OD_FILTER: its size, then the extensions), the
 * case of Latin and Cyrillic letters ignored */
static int supported(const char *name)
{
	const char *dot = strrchr(name, '.');
	if (!dot) return 0;
	const u8 *f = communication_area + OD_FILTER;
	u32 size = rd32(f);
	const char *e = (const char *)f + 4, *end = e + size;
	while (e < end && *e) {
		if (!strcmpi866(dot + 1, e)) return 1;
		e += strlen(e) + 1;
	}
	return 0;
}

/* delete_unsupported_BDFE: the entries "." and the files the filter does
 * not take (all files when a folder is chosen) off the list */
static void delete_unsupported_BDFE(u8 *d)
{
	u32 n = rd32(d + 4), i = 0;
	while (i < n) {
		u8 *e = d + 32 + i * 304;
		const char *name = (const char *)e + 40;
		int del = !strcmp(name, ".") ||
			  (!(e[0] & 0x10) && (open_dialog_type == 2 || !supported(name)));
		if (!del) {
			i++;
			continue;
		}
		memmove(e, e + 304, (n - i - 1) * 304);
		n--;
	}
	wr32(d + 4, n);
}

/* check_filter */
static void check_filter(u8 *d)
{
	if (open_dialog_type == 2 ||
	    (filter_flag && communication_area && rd32(communication_area + OD_FILTER_FLAG)))
		delete_unsupported_BDFE(d);
}

/* load_directory: dirinfo_name read whole (70.1: how many first) into
 * file_browser_data_1.folder_data; N_error 5 and error_type if not */
static void load_directory(void)
{
	N_error = 0;
	free(file_browser_data_1.folder_data);
	file_browser_data_1.folder_data = NULL;
	dirinfo_return = NULL;
	u8 dir_header[32];
	struct kos_r2 r = kos_read_folder(dirinfo_name, dir_header, 0, 0);
	if (r.eax && r.eax != 6) {
		N_error = 5;
		error_type = r.eax;
		return;
	}
	u32 total = rd32(dir_header + 8);
	u8 *d = malloc(32 + total * 304 + 304);
	if (!d) memory_get_error();
	memset(d, 0, 32);
	dirinfo_return = d;
	file_browser_data_1.folder_data = d;
	r = kos_read_folder(dirinfo_name, d, 0, total);
	if (r.eax && r.eax != 6) {
		N_error = 5;
		error_type = r.eax;
		return;
	}
	if (!rd32(d + 4)) return;              /* an empty folder */
	delete_point_dir(d);
	check_filter(d);
	/* clear_data_fb_and_sb */
	file_browser_data_1.start_draw_cursor_line = 0;
	file_browser_data_1.start_draw_line = 0;
	scroll_bar_data_vertical.position = 0;
}

/* sort_directory: sort.obj's SortDir in sort_type's order (0 name, 2
 * extension, 4 the date, 6 the size), the folders first, ".." left first */
static const char *seek_ext(const char *s)
{
	const char *d = strrchr(s, '.');
	return d ? d + 1 : s + strlen(s);
}
static int cmpu(u32 a, u32 b) { return a > b ? 1 : a < b ? -1 : 0; }
static int compare(const void *pa, const void *pb)
{
	const u8 *a = pa, *b = pb;
	int da = a[0] & 0x10, db = b[0] & 0x10;
	if (da != db) return da ? -1 : 1;
	const char *na = (const char *)a + 40, *nb = (const char *)b + 40;
	int r = 0;
	switch (sort_type) {
	case 2: r = strcmpi866(seek_ext(na), seek_ext(nb)); break;
	case 4:                                /* the newer first: the date, the time */
		r = cmpu(rd32(b + 28), rd32(a + 28));
		if (!r) r = cmpu(rd32(b + 24), rd32(a + 24));
		break;
	case 6:                                /* the larger first */
		r = cmpu(rd32(b + 36), rd32(a + 36));
		if (!r) r = cmpu(rd32(b + 32), rd32(a + 32));
		break;
	}
	return r ? r : strcmpi866(na, nb);
}
static void sort_directory(void)
{
	u8 *d = file_browser_data_1.folder_data;
	if (!d) return;
	u32 n = rd32(d + 4);
	u8 *e = d + 32;
	if (n && !strcmp((const char *)e + 40, "..")) {
		n--;
		e += 304;
	}
	qsort(e, n, 304, compare);
}

/* load_root_directory: the disks, each folder of "/" with its folders
 * ("/rd/1", "/tmp0/1"), for the menu "Select Disk" */
static void load_root_directory(void)
{
	u8 *saved = file_browser_data_1.folder_data;
	file_browser_data_1.folder_data = NULL;
	dirinfo_name = dir_path_temp;
	strcpy(dir_path_temp, root_pach);
	load_directory();
	if (N_error) button_exit();
	u8 *root_folder_area = file_browser_data_1.folder_data;
	u32 root_folder_block = rd32(root_folder_area + 4);
	file_browser_data_1.folder_data = NULL;
	retrieved_devices_table_counter = 0;
	for (u32 i = 0; i < root_folder_block; i++) {
		const char *dev = (const char *)root_folder_area + 32 + i * 304 + 40;
		dir_path_temp[0] = '/';
		strncpy(dir_path_temp + 1, dev, sizeof(dir_path_temp) - 2);
		load_directory();
		if (N_error) button_exit();
		u8 *root1 = file_browser_data_1.folder_data;
		u32 root1_folder_block = rd32(root1 + 4);
		for (u32 j = 0; j < root1_folder_block && retrieved_devices_table_counter < DEV_MAX; j++) {
			char *t = retrieved_devices_table + retrieved_devices_table_counter * DEV_LEN;
			const char *sub = (const char *)root1 + 32 + j * 304 + 40;
			if (strlen(dev) + strlen(sub) + 3 > DEV_LEN) continue;
			strcpy(t, "/");
			strcat(t, dev);
			strcat(t, "/");
			strcat(t, sub);
			retrieved_devices_table_counter++;
		}
	}
	free(root_folder_area);
	free(file_browser_data_1.folder_data);
	/* the menu's items */
	char *edi = menu_text_area_1_1;
	for (u32 i = 0; i < retrieved_devices_table_counter; i++) {
		strcpy(edi, retrieved_devices_table + i * DEV_LEN);
		edi += strlen(edi) + 1;
	}
	menu_data_1.text_end = edi;
	*edi = 0;
	file_browser_data_1.folder_data = saved;
	dirinfo_return = saved;
	dirinfo_name = dir_path;
}

/* load_start_directory: the area's folder and file's name (temp1.asm if
 * none), else /sys; tried until a folder reads */
static void load_start_directory(void)
{
	if (communication_area) {
		const char *name = (const char *)communication_area + OD_NAME;
		copy_dir_name(user_selected_name, name[0] ? name : example_name_temp);
		copy_dir_name(dir_path, (const char *)communication_area + OD_PATH);
	} else {
		copy_dir_name(dir_path, start_pach);
	}
	for (;;) {                             /* .3 */
		load_directory();
		if (!N_error) return;
		error_handler();
	}
}

/* get_communication_area: the area named by the parameter, its type
 * (saving: the file's name in focus), its window's size if not too small */
static void get_communication_area(void)
{
	if (!__params[0]) return;
	communication_area = kos_shm_open(__params, 0, KOS_SHM_OPEN | KOS_SHM_WRITE, NULL);
	u8 *a = communication_area;
	if (!a) return;
	open_dialog_type = rd16(a + OD_TYPE);
	if (open_dialog_type == 1) {
		focus_pointer = 1;
		file_browser_data_1.select_panel_counter = 0;
		edit1.flags |= ed_focus;
		edit1.color = 0xffffb0;            /* yellow */
		kos_call(66, 1, 0, 0, 0, 0);
	}
	u32 ebx = rd32(a + OD_X);
	if ((u16)ebx < x_minimal_size) return;
	window_x = ebx;
	ebx = rd32(a + OD_Y);
	if ((u16)ebx < y_minimal_size) return;
	window_y = ebx;
}

/* get_active_pocess: its window's slot, also into the area */
static void get_active_pocess(void)
{
	kos_call(9, (u32)procinfo, -1, 0, 0, 0);
	active_process = kos_slot_of_pid(rd32(procinfo + 30));
	if (communication_area) wr32(communication_area + OD_SLOT, active_process);
}

/* load_icons, convert_icons: the icons are built in */
static void convert_icons(void)
{
	file_browser_data_1.icon_raw_area = icons18;
	file_browser_data_1.icon_size_x = icons18_w;
	file_browser_data_1.icon_size_y = icons18_w;
	file_browser_data_1.line_size_y = icons18_w + 1;
}

/* load_ini: /sys/File managers/icon2ext.ini into image_file */
static void load_ini(void)
{
	u8 info[40];
	struct kos_fileinfo fi = { 5, 0, 0, 0, (u32)info, 0, ini_file_name };
	u32 e = kos_call(70, (u32)&fi, 0, 0, 0, 0);
	if (e) {
		N_error = 1;
		error_type = e;
		button_exit();
	}
	img_size = rd32(info + 32);
	image_file = malloc(img_size + 1);
	if (!image_file) memory_get_error();
	struct kos_r2 r = kos_read_file(ini_file_name, image_file, img_size, 0);
	if (r.eax && r.eax != 6) {
		N_error = 1;
		error_type = r.eax;
		button_exit();
	}
}

/* calc_ini: the section [icons18]'s "ID=ext,ext,..." lines as "ext=ID"
 * lines for the list's icons */
static void calc_ini(void)
{
	const char *s = (const char *)image_file, *end = s + img_size;
	char *o = converted_ini_buffer, *oend = converted_ini_buffer + sizeof(converted_ini_buffer) - 16;
	file_browser_data_1.ini_file_start = o;
	/* .find_section */
	for (;;) {
		if (s >= end) goto finished;
		if (*s++ == '[' && end - s >= 4 && !memcmp(s, "icon", 4)) break;
	}
	while (s < end && *s++ != 10);         /* .skip_header */
	while (s < end && *s != '[') {         /* .parse_line */
		const char *id = s;
		while (s < end && *s != '=') s++;
		if (s >= end) break;
		int idlen = s - id;
		s++;
		for (;;) {                         /* .next_ext */
			const char *ext = s;
			while (s < end && *s != ',' && *s != 13 && *s != 10) s++;
			int n = s - ext;
			if (n && o + n + idlen + 3 < oend) {   /* .emit_ext */
				memcpy(o, ext, n);
				o += n;
				*o++ = '=';
				memcpy(o, id, idlen);
				o += idlen;
				*o++ = 13;
				*o++ = 10;
			}
			if (s >= end || *s != ',') break;
			s++;
		}
		while (s < end && (*s == 13 || *s == 10)) s++;   /* .skip_eol */
	}
finished:
	file_browser_data_1.ini_file_end = o;
}

/* --- drawing ------------------------------------------------------------- */

/* type_title */
static void type_title(void)
{
	kos_set_caption(open_dialog_title_pointer[open_dialog_type < 3 ? open_dialog_type : 0]);
}

/* prepare_system_colors */
static void prepare_system_colors(void)
{
	kos_call(48, 3, (u32)&app_colours, sizeof(app_colours), 0, 0);
	struct menu_bar *m[3] = { &menu_data_1, &menu_data_2, &menu_data_3 };
	for (int i = 0; i < 3; i++) {
		m[i]->bckg_col = m[i]->menu_col = w_work;
		m[i]->frnt_col = m[i]->menu_sel_col = w_work_button;
		m[i]->bckg_text_col = w_work_text;
		m[i]->frnt_text_col = w_work_button_text;
	}
	scroll_bar_data_vertical.bg_color = w_work;
	scroll_bar_data_vertical.front_color = w_work_button;
	scroll_bar_data_vertical.line_color = w_work_button_text;
}

/* draw_dir_path */
static void draw_dir_path(void)
{
	struct file_browser *fb = &file_browser_data_1;
	PathShow_data_1.area_size_x = fb->size_x - 5;
	PathShow_data_1.start_x = fb->start_x + 3;
	rect(fb->start_x + 1, 8, fb->size_x - 2, 13, 0xffffcc);
	PathShow_prepare(&PathShow_data_1);
	PathShow_draw(&PathShow_data_1);
}

/* draw_file_name: the name's box under the list (saving) */
static void draw_file_name(void)
{
	struct file_browser *fb = &file_browser_data_1;
	edit1.size = edit1.pos = strlen(user_selected_name);
	edit1.width = fb->size_x - 200;
	edit1.left = fb->start_x + 70;
	edit1.top = fb->start_y + fb->size_y + 5;
	edit_box_draw(&edit1);
	kos_call(4, (u32)(fb->start_x + 5) << 16 | (u16)(fb->start_y + fb->size_y + 9),
		 0x80000000u | (w_work_text & 0xffffff), (u32)message_file_name, 0, 0);
}

/* the label of button 4 (x << 16 | y at open_button_coordinates) */
static void draw_label(const char *s)
{
	kos_call(4, open_button_coordinates, 0xd0000000u | (w_work_button_text & 0xffffff), (u32)s, 0,
		 w_work_button & 0xffffff);
}

/* copy_new_file_name: the cursor's file into the name's box (saving) */
static void copy_new_file_name(void)
{
	const char *n = (const char *)file_browser_data_1.selected_BDVK_adress + 40;
	if (!n[0]) return;
	strncpy(user_selected_name, n, sizeof(user_selected_name) - 1);
	edit1.size = edit1.pos = strlen(user_selected_name);
	edit_box_draw(&edit1);
}

/* draw_open_button_label: saving, " Open " on a folder, " Save " on a
 * file (its name into the box) or with the name in focus */
static void draw_open_button_label(void)
{
	if (do_not_draw_open_button_label || open_dialog_type != 1) return;
	if (focus_pointer == 1) {              /* draw_save_button_label */
		draw_label(message_open_dialog_button[1]);
		return;
	}
	if (!open_button_coordinates) return;
	const char *s = message_open_dialog_button[1];
	if (file_browser_data_1.folder_block && file_browser_data_1.selected_BDVK_adress) {
		if (file_browser_data_1.selected_BDVK_adress[0] & 0x10) s = message_open_dialog_button[0];
		else copy_new_file_name();
	}
	draw_label(s);
}

/* draw_scrollbar: when the list does not fit */
static void draw_scrollbar(void)
{
	scrollbar *sb = &scroll_bar_data_vertical;
	if (sb->max_area > sb->cur_area && sb->cur_area) scrollbar_v_draw(sb);
}

/* draw_draw_file_browser1: the path, the name, the list, its scroll bar */
static void draw_draw_file_browser1(void)
{
	draw_dir_path();
	if (open_dialog_type == 1) draw_file_name();
	file_browser_data_1.all_redraw = 1;
	scroll_bar_data_vertical.all_redraw = 1;
	FileBrowser_draw(&file_browser_data_1);
	draw_open_button_label();
	/* prepare_scrollbar_data */
	scroll_bar_data_vertical.max_area = file_browser_data_1.folder_block;
	scroll_bar_data_vertical.cur_area = file_browser_data_1.max_panel_line;
	scroll_bar_data_vertical.position = file_browser_data_1.start_draw_line;
	draw_scrollbar();
	file_browser_data_1.all_redraw = 0;
	scroll_bar_data_vertical.all_redraw = 0;
}

/* draw_draw_file_browser2: the list from the scroll bar's position */
static void draw_draw_file_browser2(void)
{
	file_browser_data_1.all_redraw = 2;
	file_browser_data_1.start_draw_line = scroll_bar_data_vertical.position;   /* get_scrollbar_data */
	FileBrowser_draw(&file_browser_data_1);
	draw_open_button_label();
	file_browser_data_1.all_redraw = 0;
}

/* draw_scrollbar1 */
static void draw_scrollbar1(void)
{
	scroll_bar_data_vertical.position = file_browser_data_1.start_draw_line;
	draw_scrollbar();
}

/* draw_for_fs_errors: a folder's error in the list's place, button 5 */
static void draw_for_fs_errors(void)
{
	struct file_browser *fb = &file_browser_data_1;
	draw_dir_path();
	rect(fb->start_x, fb->start_y, fb->size_x, fb->size_y, fb->background_color);
	int x = fb->start_x + 10 + 5, y = fb->start_y + 10 + 15;
	kos_call(4, (u32)x << 16 | (u16)y, 0x90000000u, (u32)load_directory_error_type, 0, 0);
	kos_call(4, (u32)x << 16 | (u16)(y + 20), 0x90000000u, (u32)dir_path, 0, 0);
	kos_call(4, (u32)x << 16 | (u16)(y + 40), 0x90000000u,
		 (u32)(error_type < 12 ? error_fs_text_pointers[error_type] : ""), 0, 0);
	kos_call(4, (u32)x << 16 | (u16)(y + 60), 0x90000000u, (u32)error_help_text, 0, 0);
	int bx = fb->start_x + fb->size_x / 2 - 25, by = fb->start_y + fb->size_y - 40;
	kos_call(8, (u32)bx << 16 | 50, (u32)by << 16 | 15, 5, w_work_button, 0);
	kos_call(4, (u32)(bx + 4) << 16 | (u16)by, 0x90000000u | (w_work_button_text & 0xffffff),
		 (u32)message_cancel_button, 0, 0);
}

/* a button of the window's: its text at x + dx (font 1, 16 high) */
static void button(int x, int y, int w, u32 id, const char *s, int dx)
{
	kos_call(8, (u32)x << 16 | (u16)w, (u32)y << 16 | 16, id, w_work_button, 0);
	if (s) kos_call(4, (u32)(x + dx) << 16 | (u16)y, 0x90000000u | (w_work_button_text & 0xffffff), (u32)s, 0, 0);
}

/* draw_window */
static void draw_window(void)
{
	struct file_browser *fb = &file_browser_data_1;
	kos_begin_draw();
	prepare_system_colors();
	kos_call(0, window_x, window_y, 0x63aabbcc, 0, 0);
	type_title();
	get_window_param();
	if (window_status & 4) {               /* rolled up */
		kos_end_draw();
		return;
	}
	fb->size_y = window_high - 25 - 45;
	scroll_bar_data_vertical.y_size = fb->size_y;
	fb->size_x = window_width - 10 - 20;
	scroll_bar_data_vertical.x_pos = fb->size_x + 10;
	scroll_bar_data_vertical.y_pos = fb->start_y;
	/* the work area around the list */
	rect(0, 0, window_width, 45, w_work);
	rect(0, 45 + fb->size_y, window_width, 25, w_work);
	rect(0, 45, 10, fb->size_y, w_work);
	rect(fb->size_x + 10, 45, 20, fb->size_y, w_work);
	if (error_window) {
		draw_for_fs_errors();
	} else {
		do_not_draw_open_button_label = 1;
		draw_draw_file_browser1();
		do_not_draw_open_button_label = 0;
	}
	menu_bar_draw(&menu_data_1);
	menu_bar_draw(&menu_data_2);
	menu_bar_draw(&menu_data_3);
	int right = fb->start_x + fb->size_x;
	/* 2: the folder above (three '^' one under the other), 6: Refresh */
	button(right - 50, 25, 50, 2, NULL, 0);
	for (int i = 0; i < 3; i++)
		kos_call(4, (u32)(right - 50 + 21) << 16 | (u16)(25 + 4 * i),
			 0x90000000u | (w_work_button_text & 0xffffff), (u32)message_ExitDir_button, 0, 0);
	button(right - 120, 25, 60, 6, message_ReloadDir_button, 2);
	/* 3: Cancel, 4: Open, Save, Select */
	int by = fb->start_y + fb->size_y + 3;
	button(right - 55, by, 55, 3, message_cancel_button, 4);
	button(right - 120, by, 55, 4, NULL, 0);
	const char *label = message_open_dialog_button[open_dialog_type < 3 ? open_dialog_type : 0];
	if (open_dialog_type == 1 && focus_pointer != 1 && !error_window && fb->folder_block &&
	    fb->selected_BDVK_adress && (fb->selected_BDVK_adress[0] & 0x10))
		label = message_open_dialog_button[0];   /* Open: a folder */
	open_button_coordinates = (u32)(right - 120 + 4) << 16 | (u16)by;
	kos_call(4, open_button_coordinates, 0x90000000u | (w_work_button_text & 0xffffff), (u32)label, 0, 0);
	kos_end_draw();
}

/* control_minimal_window_size: not smaller than 350 x 250 (unless
 * maximised, rolled up or minimised) */
static void control_minimal_window_size(void)
{
	get_window_param();
	if (window_status & 7) return;
	if (rd32(procinfo + 46) < y_minimal_size) kos_call(67, -1, -1, -1, y_minimal_size, 0);
	if (rd32(procinfo + 42) < x_minimal_size) kos_call(67, -1, -1, x_minimal_size, -1, 0);
}

/* --- the answer ------------------------------------------------------------ */

/* file_no_folder: the file (or the folder, choosing one) into the area,
 * the flag 1, the end; name: the cursor's entry's name */
static void file_no_folder(const char *name)
{
	copy_dir_name(file_name, dir_path);
	if (focus_pointer) {
		name = user_selected_name;
		if (!user_selected_name[0]) goto exit;
	}
	if (open_dialog_type != 2) copy_dir_path(file_name, name);
	if (!communication_area) {
		control_minimal_window_size();
		draw_window();
		return;
	}
	strcpy((char *)communication_area + OD_PATH, file_name);
	if (name[0]) strcpy((char *)communication_area + OD_NAME, name);
exit:
	if (communication_area) wr16(communication_area + OD_FLAG, 1);
	button_exit();
}

/* error_handler: the folder's error shown until Esc or Cancel, then the
 * folder before (saving: the root) */
static void error_handler(void)
{
	error_handler_store_input_mode = kos_call(66, 2, 0, 0, 0, 0);
	kos_call(66, 1, 1, 0, 0, 0);
	for (int redraw = 1;;) {
		if (redraw) {                      /* .draw_window */
			error_window = 1;
			control_minimal_window_size();
			draw_window();
			error_window = 0;
			redraw = 0;
		}
		int ev = kos_wait_event();
		if (ev == 1) {
			redraw = 1;
		} else if (ev == 2) {
			u32 k = kos_get_key();
			if ((k & 0xff) == 0 && (k >> 8 & 0xff) == 129) break;   /* Esc let go */
		} else if (ev == 3) {
			u32 id = kos_get_button() >> 8;
			if (id == 5) break;
			if (id == 1) {
				N_error = 0;
				button_exit();
			}
		}
	}
	/* .exit */
	if (open_dialog_type == 1) {
		strcpy(dir_path, root_pach);
		strcpy(previous_dir_path, root_pach);
	} else {
		strcpy(dir_path, previous_dir_path);
		strcpy(previous_dir_path, start_pach);
	}
	kos_call(66, 1, error_handler_store_input_mode, 0, 0, 0);
}

/* load_next_dir: Enter on the cursor's entry: into the folder, to the
 * folder above (".."), or the file chosen */
static void load_next_dir(void)
{
	struct file_browser *fb = &file_browser_data_1;
	if (!fb->folder_block || !fb->selected_BDVK_adress) return;
	const u8 *e = fb->selected_BDVK_adress;
	const char *name = (const char *)e + 40;
	if (!(e[0] & 0x10)) {
		file_no_folder(name);
		return;
	}
	if (!strcmp(name, "..")) {
		copy_exit_dir(dir_path);           /* .exit_dir */
	} else {
		char n[264];
		strncpy(n, name, sizeof(n) - 1);
		n[sizeof(n) - 1] = 0;
		copy_dir_path(dir_path, n);
	}
	load_next_dir_1();
}

/* load_next_dir.1: the folder dir_path read and shown */
static void load_next_dir_1(void)
{
	for (;;) {
		load_directory();
		if (!N_error) break;
		error_handler();
	}
	sort_directory();
	rect(scroll_bar_data_vertical.x_pos, scroll_bar_data_vertical.y_pos, scroll_bar_data_vertical.x_size + 1,
	     scroll_bar_data_vertical.y_size + 1, 0xcccccc);   /* the scroll bar's place */
	edit1.flags = 0;
	edit1.color = 0xffffff;
	draw_draw_file_browser1();
}

/* --- the keys ---------------------------------------------------------------- */

/* key.key_action: a FileBrowser key (1 down ... 7 Enter) */
static void key_action(int action, u8 num)
{
	struct file_browser *fb = &file_browser_data_1;
	fb->key_action = action;
	fb->key_action_num = num;
	FileBrowser_key(fb);
	draw_open_button_label();
	if (fb->draw_scroll_bar) {
		draw_scrollbar1();
		fb->draw_scroll_bar = 0;
	}
}

/* change_focus_area.1: the list (focus_pointer 0, scan codes) or the
 * name's box (1, characters) */
static void change_focus_area_1(void)
{
	if (!focus_pointer) {
		file_browser_data_1.select_panel_counter = 1;
		edit1.flags &= ~ed_focus;
		edit1.color = 0xffffff;
		draw_draw_file_browser1();
		kos_call(66, 1, 1, 0, 0, 0);
	} else {
		file_browser_data_1.select_panel_counter = 0;
		edit1.flags |= ed_focus;
		edit1.color = 0xffffb0;
		draw_draw_file_browser1();
		kos_call(66, 1, 0, 0, 0, 0);
	}
}

static void change_focus_area(void)
{
	focus_pointer = (focus_pointer + 1) & 1;
	change_focus_area_1();
}

static void clear_control_key_flag(void)
{
	shift_flag = ctrl_flag = alt_flag = 0;
}

/* the menus by their keys, and from one to the one beside it */
static void analyse_out_menu_1(void);
static void analyse_out_menu_2(void);
static void analyse_out_menu_3(void);
static void select_menu(struct menu_bar *m)
{
	for (;;) {
		if (m == &menu_data_1) load_root_directory();   /* select_disk.1 */
		m->ret_key = 0;
		menu_bar_activate(m);
		clear_control_key_flag();
		u32 ret = m->ret_key;
		m->ret_key = 0;
		/* Left, Right: the menu beside (the disks, the sort, the filter) */
		if (ret == 1) { m = m == &menu_data_1 ? &menu_data_3 : m == &menu_data_2 ? &menu_data_1 : &menu_data_2; continue; }
		if (ret == 2) { m = m == &menu_data_1 ? &menu_data_2 : m == &menu_data_2 ? &menu_data_3 : &menu_data_1; continue; }
		if (m->click != 1 || !m->cursor_out) return;
		if (m == &menu_data_1) analyse_out_menu_1();
		else if (m == &menu_data_2) analyse_out_menu_2();
		else analyse_out_menu_3();
		return;
	}
}

/* analyse_out_menu_1: a disk */
static void analyse_out_menu_1(void)
{
	copy_dir_name(dir_path, retrieved_devices_table + (menu_data_1.cursor_out - 1) * DEV_LEN);
	load_next_dir_1();
}

/* analyse_out_menu_2: the sort */
static void analyse_out_menu_2(void)
{
	u32 k = menu_data_2.cursor_out;
	if (k < 1 || k > 4) return;
	sort_type = (k - 1) * 2;
	sort_directory();
	draw_draw_file_browser1();
}

/* analyse_out_menu_3: the filter */
static void analyse_out_menu_3(void)
{
	if (menu_data_3.cursor_out == 1) filter_flag = 0;
	else if (menu_data_3.cursor_out == 2) filter_flag = 1;
	else return;
	load_next_dir_1();
}

/* key_ASCII: the name's box (saving) */
static void key_ASCII(void)
{
	u32 k = kos_get_key();
	u8 ah = k >> 8;
	if (ah == 9) {                         /* change_focus_area_Tab_key_ASCII */
		Tab_key_block = 1;
		change_focus_area();
		return;
	}
	if (ah == 13) {
		file_no_folder(user_selected_name);
		return;
	}
	if (ah == 27) button_exit();
	edit_box_key(&edit1, k);
}

/* key: scan codes in the list */
static void key(void)
{
	if (focus_pointer) {
		key_ASCII();
		return;
	}
	u32 k = kos_get_key();
	if (k & 0xff) return;
	u8 ah = k >> 8;
	int action = 0;
	if (extended_key == 1) {               /* .extended_key */
		extended_key = 0;
		switch (ah) {
		case 80: action = 1; break;        /* down */
		case 72: action = 2; break;        /* up */
		case 81: action = 3; break;        /* Page Down */
		case 73: action = 4; break;        /* Page Up */
		case 71: action = 5; break;        /* Home */
		case 79: action = 6; break;        /* End */
		case 28: action = 7; break;        /* Enter */
		case 82: action = 8; break;        /* Insert */
		case 29: ctrl_flag = 1; return;
		case 157: ctrl_flag = 0; return;
		case 56: alt_flag = 1; return;
		case 184: alt_flag = 0; return;
		default: return;
		}
	} else {
		switch (ah) {
		case 0xe0: extended_key = 1; return;
		case 72: action = 2; break;
		case 80: action = 1; break;
		case 28: action = 7; break;
		case 1: button_exit();             /* Esc */
		case 14:                           /* Backspace: button.exit_dir */
			copy_exit_dir(dir_path);
			load_next_dir_1();
			return;
		case 187: if (alt_flag) select_menu(&menu_data_1); return;   /* Alt+F1 */
		case 188: if (alt_flag) select_menu(&menu_data_2); return;   /* Alt+F2 */
		case 189: if (alt_flag) select_menu(&menu_data_3); return;   /* Alt+F3 */
		case 19:                           /* R: with Ctrl button.reload_dir */
			if (ctrl_flag) load_next_dir_1();
			return;
		case 42: case 54: shift_flag = 1; return;
		case 170: case 182: shift_flag = 0; return;
		case 29: ctrl_flag = 1; return;
		case 157: ctrl_flag = 0; return;
		case 56: alt_flag = 1; return;
		case 184: alt_flag = 0; return;
		case 15:                           /* Tab pressed */
			if (open_dialog_type == 1 && !Tab_key_block) Tab_key = 1;
			return;
		case 143:                          /* Tab let go */
			if (open_dialog_type != 1) return;
			Tab_key_block = 0;
			if (!Tab_key) return;
			Tab_key = 0;
			change_focus_area();
			return;
		default:
			return;                        /* the marks and the search by a letter: not here */
		}
	}
	key_action(action, ah);
	struct file_browser *fb = &file_browser_data_1;
	if (fb->mouse_keys_delta == 3) {
		fb->mouse_keys_delta = 0;
		load_next_dir();
	}
}

/* --- the buttons and the mouse ---------------------------------------------- */

static void button_event(void)
{
	u32 ah = kos_get_button() >> 8;
	struct file_browser *fb = &file_browser_data_1;
	switch (ah) {
	case 6: load_next_dir_1(); return;     /* .reload_dir_1 */
	case 4:                                /* .open_dir_or_file */
		if (open_dialog_type == 0 && !(fb->folder_data && rd32(fb->folder_data + 4))) button_exit();
		if (open_dialog_type == 2 || (open_dialog_type == 1 && focus_pointer)) {
			file_no_folder(user_selected_name);
			return;
		}
		key_action(7, 0);
		if (fb->mouse_keys_delta == 3) {
			fb->mouse_keys_delta = 0;
			load_next_dir();
		}
		return;
	case 3: case 1: button_exit();         /* .exit */
	case 2:                                /* .exit_dir */
		copy_exit_dir(dir_path);
		load_next_dir_1();
		return;
	}
}

static void mouse(void)
{
	struct file_browser *fb = &file_browser_data_1;
	scrollbar *sb = &scroll_bar_data_vertical;
	if ((u32)kos_active_slot() != active_process) return;
	s32 wheel = (s16)kos_call(37, 7, 0, 0, 0, 0);
	u32 pos = kos_call(37, 1, 0, 0, 0, 0);
	if (!sb->delta2) {
		fb->select_flag = 0;
		FileBrowser_mouse(fb);
		draw_open_button_label();
		if (fb->mouse_keys_delta == 3) {
			fb->mouse_keys_delta = 0;
			load_next_dir();
			return;
		}
		/* .check_focus: a press in the list takes the focus from the name */
		if (fb->select_flag && focus_pointer) {
			focus_pointer = 0;
			change_focus_area_1();
			return;
		}
	}
	/* .scrollbar */
	if (sb->max_area > sb->cur_area) {
		scrollbar_v_mouse(sb);
		if (sb->redraw) {
			sb->redraw = 0;
			draw_draw_file_browser2();
			return;
		}
	}
	if (sb->delta2) return;
	/* .menu_bar */
	menu_data_1.get_mouse_flag = menu_data_2.get_mouse_flag = 1;
	menu_bar_mouse(&menu_data_1);
	if (menu_data_1.click == 1) {
		if (menu_data_1.cursor_out) select_menu(&menu_data_1);
		return;
	}
	menu_bar_mouse(&menu_data_2);
	if (menu_data_2.click == 1) {
		if (menu_data_2.cursor_out) analyse_out_menu_2();
		return;
	}
	menu_bar_mouse(&menu_data_3);
	if (menu_data_3.click == 1) {
		if (menu_data_3.cursor_out) analyse_out_menu_3();
		return;
	}
	/* .check_editboxes: saving, a press in the name's box takes the focus */
	if (open_dialog_type == 1) {
		u32 was = edit1.flags & ed_focus;
		edit_box_mouse(&edit1);
		u32 now = edit1.flags & ed_focus;
		if (was != now && !focus_pointer) {
			focus_pointer = now ? 1 : 0;
			change_focus_area_1();
			return;
		}
	}
	/* .check_scroll_event: the wheel over the list */
	int x = (s16)(pos >> 16), y = (s16)pos;
	if (!wheel || x < fb->start_x || y < fb->start_y || x > fb->start_x + fb->size_x || y > fb->start_y + fb->size_y)
		return;
	for (int n = wheel < 0 ? -wheel : wheel; n > 0; n--) key_action(wheel < 0 ? 2 : 1, 0);
}

/* START */
int main(int argc, char **argv)
{
	(void)argc;
	(void)argv;
	scroll_bar_data_vertical.type = 2;
	kos_call(68, 11, 0, 0, 0, 0);
	kos_call(66, 1, 1, 0, 0, 0);
	kos_event_mask(0x27);
	get_communication_area();
	get_active_pocess();
	strcpy(previous_dir_path, start_pach);
	load_root_directory();
	load_start_directory();
	sort_directory();
	convert_icons();
	load_ini();
	calc_ini();
	draw_window();                         /* red_1 */
	for (;;) {                             /* still */
		int ev = kos_wait_event();
		if (ev == 1) {                     /* red */
			control_minimal_window_size();
			draw_window();
		} else if (ev == 2) {
			key();
		} else if (ev == 3) {
			button_event();
		} else if (ev == 6) {
			mouse();
		}
	}
}

/* KolibriOS for ARM: terminal, a terminal on a serial port, translated to
 * C from KolibriOS's programs/system/terminal/terminal.asm ("terminal for
 * KolibriOS", written by hidnplayr@kolibrios.org, Copyright (C) KolibriOS
 * team 2014-2026, GPL-2.0). Its labels are the functions and variables
 * here.
 *
 * A window to choose the port and its settings (box_lib's edit boxes,
 * option boxes and check box: lib/boxlib); "Open" (or Enter) opens the
 * port through the serial driver's service (function 68.16 "SERIAL",
 * 68.17: kernel/serial.c, as drivers/serial/common.inc calls it) and
 * hands the window to the console (lib/console): what is typed goes to
 * the port (and to the screen with local echo), what comes from it to the
 * screen. Port 0 is the kernel's console UART; QEMU's realview has 1-3
 * too. */
#include <string.h>
#include <kos.h>
#include <box_lib.h>
#include <conio.h>

/* drivers/serial/common.inc */
#define SERIAL_COMPATIBLE_API_VER 1
enum { SERIAL_API_GET_VERSION = 0, SERIAL_API_OPEN_PORT = 4, SERIAL_API_CLOSE_PORT = 5,
       SERIAL_API_SETUP_PORT = 6, SERIAL_API_READ = 7, SERIAL_API_WRITE = 8 };
enum { SERIAL_API_ERR_PORT_INVALID = 1, SERIAL_API_ERR_PORT_BUSY = 2, SERIAL_API_ERR_CONF = 3 };
enum { SERIAL_CONF_PARITY_NONE, SERIAL_CONF_PARITY_EVEN, SERIAL_CONF_PARITY_ODD, SERIAL_CONF_PARITY_MARK,
       SERIAL_CONF_PARITY_SPACE };
enum { SERIAL_CONF_STOP_BITS_1, SERIAL_CONF_STOP_BITS_1_5, SERIAL_CONF_STOP_BITS_2 };
#define SERIAL_CONF_FLOW_CTRL_NONE 0

struct sp_conf {
	u32 size, baudrate;
	u8 word_size, stop_bits, parity, flow_ctrl;
};

static u32 serial_drv_handle;

/* the service's request: handler, io_code, input, inp_size, output, out_size */
static int serial_ioctl(u32 code, const void *in, u32 insz, void *out, u32 outsz)
{
	u32 ctl[6] = { serial_drv_handle, code, (u32)in, insz, (u32)out, outsz };
	return kos_call(68, 17, (u32)ctl, 0, 0, 0);
}

static u32 serial_port_init(void)
{
	serial_drv_handle = kos_call(68, 16, (u32)"SERIAL", 0, 0, 0);   /* SSF_LOAD_DRIVER */
	return serial_drv_handle;
}

static int serial_port_get_version(u32 *version) { return serial_ioctl(SERIAL_API_GET_VERSION, 0, 0, version, 4); }

static int serial_port_open(u32 port_id, const struct sp_conf *conf, u32 *handle)
{
	u32 in[2] = { port_id, (u32)conf };
	return serial_ioctl(SERIAL_API_OPEN_PORT, in, 8, handle, 4);
}

static int serial_port_close(u32 handle) { return serial_ioctl(SERIAL_API_CLOSE_PORT, &handle, 4, 0, 0); }

static int serial_port_read(u32 handle, void *dest, u32 *count)
{
	u32 in[3] = { handle, (u32)dest, *count };
	return serial_ioctl(SERIAL_API_READ, in, 12, count, 4);
}

static int serial_port_write(u32 handle, const void *src, u32 *count)
{
	u32 in[3] = { handle, (u32)src, *count };
	return serial_ioctl(SERIAL_API_WRITE, in, 12, count, 4);
}

/* --- DATA ---------------------------------------------------------- */

static u32 mouse_dd;
static char ed_port[7] = "0", ed_speed[7] = "9600", ed_data[2] = "8", ed_stop[2] = "1";

static edit_box edits[4] = {
	EDIT_BOX(60, 112, 10, 0xffffff, 0x6f9480, 0, 0, 0x10000000, 5, ed_port, &mouse_dd, ed_focus, 1, 1),
	EDIT_BOX(60, 112, 35, 0xffffff, 0x6a9480, 0, 0, 0x10000000, 7, ed_speed, &mouse_dd, ed_figure_only, 4, 4),
	EDIT_BOX(60, 112, 60, 0xffffff, 0x6a9480, 0, 0, 0x10000000, 1, ed_data, &mouse_dd, ed_figure_only, 1, 1),
	EDIT_BOX(60, 112, 85, 0xffffff, 0x6a9480, 0, 0, 0x10000000, 1, ed_stop, &mouse_dd, ed_figure_only, 1, 1),
};
#define edit1 edits[0]

static const char str_none[] = "None", str_odd[] = "Odd", str_even[] = "Even", str_mark[] = "Mark",
		  str_space[] = "Space";

static option_box *option_group1, *option_group2;
static option_box op1 = OPTION_BOX(&option_group1, 195, 30, 6, 12, 0xffffff, 0, 0, str_none, 4, 0);
static option_box op2 = OPTION_BOX(&option_group1, 195, 47, 6, 12, 0xffffff, 0, 0, str_odd, 3, 0);
static option_box op3 = OPTION_BOX(&option_group1, 195, 64, 6, 12, 0xffffff, 0, 0, str_even, 4, 0);
static option_box op4 = OPTION_BOX(&option_group1, 195, 81, 6, 12, 0xffffff, 0, 0, str_mark, 4, 0);
static option_box op5 = OPTION_BOX(&option_group1, 195, 98, 6, 12, 0xffffff, 0, 0, str_space, 5, 0);
static option_box op6 = OPTION_BOX(&option_group2, 280, 30, 6, 12, 0xffffff, 0, 0, str_none, 4, 0);
static option_box *Option_boxs1[] = { &op1, &op2, &op3, &op4, &op5, NULL };
static option_box *Option_boxs2[] = { &op6, NULL };

static check_box2 ch1 = CHECK_BOX2(5 << 16 | 12, 119 << 16 | 12, 5, 0xffffff, 0x000000, 0, "Local echo", ch_flag_middle);

static const char str_title[] = "Terminal", str_port[] = "Serial port:", str_speed[] = "Speed (baud):",
		  str_data[] = "Data bits:", str_parity[] = "Parity:", str_flow[] = "Flow control:",
		  str_stop[] = "Stop bits:", str_open[] = "Open";

static const char err_none[] = "", err_port[] = "Invalid serial port.",
		  err_reserve[] = "The port is already in use.", err_conf[] = "Incorrect port setting.",
		  err_stopbits[] = "Invalid number of stop bits. Must be 1 or 2.",
		  err_databits[] = "Invalid number of data bits. Must be between 5 and 8.",
		  err_unknown[] = "An unknown error.", err_serial_sys[] = "Error loading serial driver.",
		  err_serial_ver[] = "Incompatible serial driver version.";
static const char *errormsg = err_none;

static struct sp_conf port_conf = { sizeof(struct sp_conf), 9600, 8, SERIAL_CONF_STOP_BITS_1,
				    SERIAL_CONF_PARITY_NONE, SERIAL_CONF_FLOW_CTRL_NONE };
static system_colors sc;
static u32 port_handle;
static u32 port_buf, port_data_cnt;

/* --- the window ----------------------------------------------------- */

static void draw_window(void)
{
	kos_call(48, 3, (u32)&sc, sizeof(sc), 0, 0);   /* SSF_GET_COLORS */
	kos_begin_draw();
	kos_call(0, 50 << 16 | 415, 30 << 16 | 195, sc.work | 0x34000000, 0, (u32)str_title);

	u32 c = 0x90000000 | sc.work_text;
	static const char *const labels[4] = { str_port, str_speed, str_data, str_stop };
	for (int i = 0; i < 4; i++) kos_call(4, 5 << 16 | (12 + 25 * i), c, (u32)labels[i], 0, 0);
	kos_call(4, 195 << 16 | 12, c, (u32)str_parity, 0, 0);
	kos_call(4, 280 << 16 | 12, c, (u32)str_flow, 0, 0);

	edit_boxes_set_sys_color(edits, 4, &sc);
	for (int i = 0; i < 4; i++) edit_box_draw(&edits[i]);
	option_boxes_set_sys_color(&sc, Option_boxs1);
	option_boxes_set_sys_color(&sc, Option_boxs2);
	option_box_draw(Option_boxs1);
	option_box_draw(Option_boxs2);
	check_boxes_set_sys_color2(&ch1, 1, &sc);
	check_box_draw2(&ch1);

	kos_call(8, 280 << 16 | 100, 115 << 16 | 22, 0x10, sc.work_button, 0);
	kos_call(4, 315 << 16 | 119, 0x90000000 | sc.work_button_text, (u32)str_open, 0, 0);
	kos_call(38, 0 << 16 | 405, 145 << 16 | 145, sc.work_graph, 0, 0);
	kos_call(4, 5 << 16 | 150, 0x90000000 | sc.work_text, (u32)errormsg, 0, 0);
	kos_end_draw();
}

/* 0 and the number, or not 0: a character that is not a digit */
static int str_to_uint(const char *s, u32 *v)
{
	*v = 0;
	for (; *s; s++) {
		if (*s < '0' || *s > '9') return 1;
		*v = *v * 10 + *s - '0';
	}
	return 0;
}

static void console_loop(void)
{
	for (;;) {
		kos_delay(1);                          /* wait 10 ms */
		if (con_get_flags() & CON_WINDOW_CLOSED) break;   /* the console window closed? */
		while (con_kbhit()) {                  /* tx: did the user press a key? */
			port_buf = con_getch2() & 0xff;
			port_data_cnt = 1;
			serial_port_write(port_handle, &port_buf, &port_data_cnt);
			if (ch1.flags & ch_flag_en)        /* does the user want us to echo locally? */
				con_write_asciiz((const char *)&port_buf);
		}
		for (;;) {                             /* rx */
			port_buf = 0;
			port_data_cnt = 3;
			if (serial_port_read(port_handle, &port_buf, &port_data_cnt)) break;   /* an error */
			if (!port_data_cnt) break;         /* no data yet */
			con_write_asciiz((const char *)&port_buf);
		}
	}
	serial_port_close(port_handle);
	kos_exit();
}

/* "Open" or Enter: the settings checked, the port opened (open_connection) */
static void open_connection(void)
{
	errormsg = err_none;                       /* clear previous error message */
	u32 v;
	/* baud rate */
	if (str_to_uint(ed_speed, &v) || !v) { errormsg = err_conf; return; }
	port_conf.baudrate = v;
	/* the number of data bits */
	if (ed_data[0] < '5' || ed_data[0] > '8') { errormsg = err_databits; return; }
	port_conf.word_size = ed_data[0] - '0';
	/* the parity type */
	if (option_group1 == &op1) port_conf.parity = SERIAL_CONF_PARITY_NONE;
	else if (option_group1 == &op2) port_conf.parity = SERIAL_CONF_PARITY_ODD;
	else if (option_group1 == &op3) port_conf.parity = SERIAL_CONF_PARITY_EVEN;
	else if (option_group1 == &op4) port_conf.parity = SERIAL_CONF_PARITY_MARK;
	else if (option_group1 == &op5) port_conf.parity = SERIAL_CONF_PARITY_SPACE;
	else kos_exit();                           /* something went terribly wrong */
	/* the number of stop bits */
	if (ed_stop[0] == '1') port_conf.stop_bits = SERIAL_CONF_STOP_BITS_1;
	else if (ed_stop[0] == '2') port_conf.stop_bits = SERIAL_CONF_STOP_BITS_2;
	else { errormsg = err_stopbits; return; }
	/* the port's id */
	if (!ed_port[0] || str_to_uint(ed_port, &v)) { errormsg = err_port; return; }

	int e = serial_port_open(v, &port_conf, &port_handle);
	if (e) {
		errormsg = e == SERIAL_API_ERR_PORT_INVALID ? err_port : e == SERIAL_API_ERR_PORT_BUSY ? err_reserve :
			   e == SERIAL_API_ERR_CONF ? err_conf : err_unknown;
		return;
	}
	/* our GUI window hidden, the console opened */
	kos_event_mask(0);                         /* disable all events */
	kos_move_window(0, 0, 0, 0);               /* hide window */
	kos_begin_draw();
	kos_end_draw();
	con_init_opt(80, 25, 80, 25, str_title);
	console_loop();
}

int main(int argc, char **argv)
{
	(void)argc, (void)argv;
	kos_call(68, 11, 0, 0, 0, 0);              /* SSF_HEAP_INIT */
	option_group1 = &op1;
	option_group2 = &op6;
	if (!serial_port_init()) {
		errormsg = err_serial_sys;
	} else {
		u32 version = 0;
		serial_port_get_version(&version);
		if ((version >> 16) > SERIAL_COMPATIBLE_API_VER) errormsg = err_serial_ver;
	}
	kos_event_mask(0x80000027);                /* EVM_MOUSE + EVM_MOUSE_FILTER + EVM_REDRAW + EVM_BUTTON + EVM_KEY */
	init_checkbox2(&ch1);
	draw_window();
	for (;;) {                                 /* mainloop */
		int ev = kos_wait_event();
		if (ev == 1) {
			draw_window();
		} else if (ev == 2) {                  /* key */
			u32 k = kos_get_key();
			if (((k >> 8) & 0xff) == 13) { open_connection(); draw_window(); continue; }   /* Enter */
			for (int i = 0; i < 4; i++) edit_box_key(&edits[i], k);
		} else if (ev == 3) {                  /* button */
			u32 id = (kos_get_button() >> 8) & 0xff;
			if (id == 0x10) { open_connection(); draw_window(); continue; }   /* connect button */
			if (id) kos_exit();
		} else {
			for (int i = 0; i < 4; i++) edit_box_mouse(&edits[i]);
			option_box_mouse(Option_boxs1);
			option_box_mouse(Option_boxs2);
			check_box_mouse2(&ch1);
		}
	}
}

/* purple-host -stdio: libpurple without a user interface, with tdlib-purple
 * (Telegram) built in, for KolibriOS's messenger (docs/messengers.md). It
 * speaks the JSON lines of yappari-wa -stdio on standard input and output:
 *
 *   in:  {"id": 1, "call": "Login", "args": ["+43664..."]}
 *   out: {"id": 1, "result": []}            {"id": 1, "error": "..."}
 *        {"signal": "StateChanged", "args": ["connecting", "Connecting"]}
 *
 * Methods: Status() -> (state, account); Login(phone): the account (its
 * name is the phone number, as tdlib-purple has it) enabled; InputReply(id,
 * text), InputCancel(id): an answer to an Input signal; SendText(who, text);
 * Logout(). Signals: StateChanged(state, text); Input(id, title, primary,
 * secondary, default): what the plugin asks (Telegram's login code, the
 * password); Notice(title, primary, secondary); Message(chat, who, time,
 * outgoing, text); Error(text). Log output goes to standard error
 * (PURPLE_DEBUG=1: libpurple's too). */
#include <glib.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <libpurple/purple.h>

#define UI_ID "kolibri"
#define PRPL "telegram-tdlib"

gboolean purple_host_init_telegram(void);   /* tdshim.cpp */

static PurpleAccount *account;
static const char *state = "logged-out";

/* --- output ---------------------------------------------------------------- */

static void put_str(GString *o, const char *s)
{
	g_string_append_c(o, '"');
	for (; s && *s; s++) {
		unsigned char c = *s;
		if (c == '"' || c == '\\') g_string_append_printf(o, "\\%c", c);
		else if (c == '\n') g_string_append(o, "\\n");
		else if (c < 32) g_string_append_printf(o, "\\u%04x", c);
		else g_string_append_c(o, c);
	}
	g_string_append_c(o, '"');
}

static void send_line(GString *o)
{
	g_string_append_c(o, '\n');
	fwrite(o->str, 1, o->len, stdout);
	fflush(stdout);
	g_string_free(o, TRUE);
}

/* a signal: its arguments as strings ('s'), numbers ('n'), booleans ('b') */
static void signal_out(const char *name, const char *types, ...)
{
	GString *o = g_string_new("{\"signal\":");
	va_list ap;
	put_str(o, name);
	g_string_append(o, ",\"args\":[");
	va_start(ap, types);
	for (const char *t = types; *t; t++) {
		if (t != types) g_string_append_c(o, ',');
		if (*t == 's') put_str(o, va_arg(ap, const char *));
		else if (*t == 'n') g_string_append_printf(o, "%ld", va_arg(ap, long));
		else g_string_append(o, va_arg(ap, int) ? "true" : "false");
	}
	va_end(ap);
	g_string_append(o, "]}");
	send_line(o);
}

static void reply(const char *id, const char *error, const char *result)
{
	GString *o = g_string_new("{\"id\":");
	g_string_append(o, id);
	if (error) {
		g_string_append(o, ",\"error\":");
		put_str(o, error);
	} else {
		g_string_append_printf(o, ",\"result\":[%s]", result ? result : "");
	}
	g_string_append_c(o, '}');
	send_line(o);
}

static void set_state(const char *s, const char *text)
{
	state = s;
	signal_out("StateChanged", "ss", s, text ? text : "");
}

/* --- the event loop: GLib's ------------------------------------------------- */

typedef struct { PurpleInputFunction fn; gpointer data; guint id; } Watch;

static gboolean watch_cb(GIOChannel *ch, GIOCondition cond, gpointer p)
{
	Watch *w = p;
	int c = 0;
	if (cond & (G_IO_IN | G_IO_HUP | G_IO_ERR)) c |= PURPLE_INPUT_READ;
	if (cond & G_IO_OUT) c |= PURPLE_INPUT_WRITE;
	w->fn(w->data, g_io_channel_unix_get_fd(ch), c);
	return TRUE;
}

static guint input_add(gint fd, PurpleInputCondition cond, PurpleInputFunction fn, gpointer data)
{
	Watch *w = g_new0(Watch, 1);
	GIOChannel *ch = g_io_channel_unix_new(fd);
	int c = 0;
	if (cond & PURPLE_INPUT_READ) c |= G_IO_IN | G_IO_HUP | G_IO_ERR;
	if (cond & PURPLE_INPUT_WRITE) c |= G_IO_OUT | G_IO_HUP | G_IO_ERR;
	w->fn = fn;
	w->data = data;
	w->id = g_io_add_watch_full(ch, G_PRIORITY_DEFAULT, c, watch_cb, w, g_free);
	g_io_channel_unref(ch);
	return w->id;
}

static PurpleEventLoopUiOps loop_ops = {
	g_timeout_add, g_source_remove, input_add, g_source_remove, NULL, g_timeout_add_seconds,
	NULL, NULL, NULL,
};

/* --- what the plugin asks ------------------------------------------------- */

typedef struct { guint id; GCallback ok, cancel; void *data; } Ask;
static GHashTable *asks;
static guint next_ask = 1;

static void *request_input(const char *title, const char *primary, const char *secondary,
			   const char *def, gboolean multiline, gboolean masked, gchar *hint,
			   const char *ok_text, GCallback ok_cb, const char *cancel_text, GCallback cancel_cb,
			   PurpleAccount *acc, const char *who, PurpleConversation *conv, void *data)
{
	Ask *a = g_new0(Ask, 1);
	a->id = next_ask++;
	a->ok = ok_cb;
	a->cancel = cancel_cb;
	a->data = data;
	g_hash_table_insert(asks, GUINT_TO_POINTER(a->id), a);
	signal_out("Input", "nssss", (long)a->id, title ? title : "", primary ? primary : "",
		   secondary ? secondary : "", def ? def : "");
	return a;
}

static void *request_action(const char *title, const char *primary, const char *secondary,
			    int def, PurpleAccount *acc, const char *who, PurpleConversation *conv,
			    void *data, size_t count, va_list choices)
{
	/* (leaving a group and the like: not asked here yet) */
	signal_out("Notice", "sss", title ? title : "", primary ? primary : "", secondary ? secondary : "");
	return NULL;
}

static void close_request(PurpleRequestType type, void *handle)
{
	Ask *a = handle;
	if (type == PURPLE_REQUEST_INPUT && a) g_hash_table_remove(asks, GUINT_TO_POINTER(a->id));
}

static PurpleRequestUiOps request_ops = {
	request_input, NULL, request_action, NULL, NULL, close_request, NULL, NULL, NULL, NULL, NULL,
};

static void answer(guint id, const char *text, gboolean ok)
{
	Ask *a = g_hash_table_lookup(asks, GUINT_TO_POINTER(id));
	if (!a) return;
	GCallback cb = ok ? a->ok : a->cancel;
	void *data = a->data;
	g_hash_table_remove(asks, GUINT_TO_POINTER(id));
	if (cb) ((PurpleRequestInputCb)cb)(data, text);
}

static void *notify_message(PurpleNotifyMsgType type, const char *title, const char *primary,
			    const char *secondary)
{
	signal_out("Notice", "sss", title ? title : "", primary ? primary : "", secondary ? secondary : "");
	return NULL;
}

static PurpleNotifyUiOps notify_ops = { notify_message, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL };

/* --- the connection and the conversations ---------------------------------- */

static void connect_progress(PurpleConnection *gc, const char *text, size_t step, size_t count)
{
	set_state("connecting", text);
}

static void connected(PurpleConnection *gc)
{
	set_state("connected", "");
}

static void disconnected(PurpleConnection *gc)
{
	set_state("disconnected", "");
}

static void disconnect_reason(PurpleConnection *gc, PurpleConnectionError reason, const char *text)
{
	signal_out("Error", "s", text ? text : "");
}

static PurpleConnectionUiOps conn_ops = {
	connect_progress, connected, disconnected, NULL, NULL, NULL, NULL, disconnect_reason,
	NULL, NULL, NULL,
};

static void write_conv(PurpleConversation *conv, const char *who, const char *alias,
		       const char *message, PurpleMessageFlags flags, time_t mtime)
{
	if (flags & (PURPLE_MESSAGE_SYSTEM | PURPLE_MESSAGE_NO_LOG) && !(flags & PURPLE_MESSAGE_RECV)) return;
	signal_out("Message", "ssnbs", purple_conversation_get_name(conv), who ? who : "", (long)mtime,
		   (flags & PURPLE_MESSAGE_SEND) != 0, message ? message : "");
}

static PurpleConversationUiOps conv_ops = {
	NULL, NULL, NULL, NULL, write_conv, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
	NULL, NULL, NULL, NULL,
};

static void debug_print(PurpleDebugLevel level, const char *category, const char *arg)
{
	fprintf(stderr, "purple %s: %s", category ? category : "", arg);
}

static gboolean debug_enabled(PurpleDebugLevel level, const char *category)
{
	return getenv("PURPLE_DEBUG") != NULL || level >= PURPLE_DEBUG_WARNING;
}

static PurpleDebugUiOps debug_ops = { debug_print, debug_enabled, NULL, NULL, NULL, NULL };

static void ui_init(void)
{
	purple_request_set_ui_ops(&request_ops);
	purple_notify_set_ui_ops(&notify_ops);
	purple_connections_set_ui_ops(&conn_ops);
	purple_conversations_set_ui_ops(&conv_ops);
}

static PurpleCoreUiOps core_ops = { NULL, NULL, ui_init, NULL, NULL, NULL, NULL, NULL };

/* --- the calls: a little JSON ----------------------------------------------- */

#define MAXARGS 8

static const char *skip(const char *p)
{
	while (*p == ' ' || *p == '\t' || *p == '\r') p++;
	return p;
}

/* a JSON string at p (after its quote), into a new string; p moves on */
static char *json_str(const char **pp)
{
	const char *p = *pp;
	GString *s = g_string_new(NULL);
	for (; *p && *p != '"'; p++) {
		if (*p != '\\' || !p[1]) {
			g_string_append_c(s, *p);
			continue;
		}
		p++;
		switch (*p) {
		case 'n': g_string_append_c(s, '\n'); break;
		case 't': g_string_append_c(s, '\t'); break;
		case 'u': {
			gunichar c = (gunichar)strtoul((char[5]){ p[1], p[2], p[3], p[4], 0 }, NULL, 16);
			g_string_append_unichar(s, c);
			p += 4;
			break;
		}
		default: g_string_append_c(s, *p); break;
		}
	}
	*pp = *p ? p + 1 : p;
	return g_string_free(s, FALSE);
}

/* {"id": ..., "call": "...", "args": [...]}: the id as written, the call,
 * the arguments (strings; numbers and booleans as their text) */
static gboolean parse_call(const char *line, char **id, char **call, char **args, int *nargs)
{
	const char *p = skip(line);
	*id = *call = NULL;
	*nargs = 0;
	if (*p++ != '{') return FALSE;
	while (*(p = skip(p)) && *p != '}') {
		if (*p == ',') { p++; continue; }
		if (*p++ != '"') return FALSE;
		char *key = json_str(&p);
		p = skip(p);
		if (*p++ != ':') { g_free(key); return FALSE; }
		p = skip(p);
		if (!strcmp(key, "args") && *p == '[') {
			p++;
			while (*(p = skip(p)) && *p != ']') {
				if (*p == ',') { p++; continue; }
				char *v;
				if (*p == '"') { p++; v = json_str(&p); }
				else {
					const char *e = p;
					while (*e && *e != ',' && *e != ']') e++;
					v = g_strndup(p, e - p);
					p = e;
				}
				if (*nargs < MAXARGS) args[(*nargs)++] = v; else g_free(v);
			}
			if (*p) p++;
		} else if (*p == '"') {
			p++;
			char *v = json_str(&p);
			if (!strcmp(key, "call")) *call = v;
			else if (!strcmp(key, "id")) *id = g_strdup_printf("\"%s\"", v), g_free(v);
			else g_free(v);
		} else {
			const char *e = p;
			while (*e && *e != ',' && *e != '}') e++;
			if (!strcmp(key, "id")) *id = g_strndup(p, e - p);
			p = e;
		}
		g_free(key);
	}
	if (!*id) *id = g_strdup("null");
	return *call != NULL;
}

static void call(const char *id, const char *name, char **a, int n)
{
	if (!strcmp(name, "Status")) {
		GString *r = g_string_new(NULL);
		put_str(r, state);
		g_string_append_c(r, ',');
		put_str(r, account ? purple_account_get_username(account) : "");
		reply(id, NULL, r->str);
		g_string_free(r, TRUE);
	} else if (!strcmp(name, "Login") && n == 1) {
		if (!account) {
			account = purple_accounts_find(a[0], PRPL);
			if (!account) {
				account = purple_account_new(a[0], PRPL);
				purple_accounts_add(account);
			}
		}
		purple_account_set_enabled(account, UI_ID, TRUE);
		reply(id, NULL, NULL);
	} else if ((!strcmp(name, "InputReply") && n == 2) || (!strcmp(name, "InputCancel") && n == 1)) {
		answer((guint)strtoul(a[0], NULL, 10), n == 2 ? a[1] : NULL, n == 2);
		reply(id, NULL, NULL);
	} else if (!strcmp(name, "SendText") && n == 2) {
		PurpleConnection *gc = account ? purple_account_get_connection(account) : NULL;
		if (!gc) {
			reply(id, "not connected", NULL);
			return;
		}
		PurpleConversation *c = purple_conversation_new(PURPLE_CONV_TYPE_IM, account, a[0]);
		purple_conv_im_send(PURPLE_CONV_IM(c), a[1]);
		reply(id, NULL, NULL);
	} else if (!strcmp(name, "Logout")) {
		if (account) purple_account_set_enabled(account, UI_ID, FALSE);
		reply(id, NULL, NULL);
	} else {
		reply(id, "no such method", NULL);
	}
}

static GMainLoop *loop;

static gboolean stdin_cb(GIOChannel *ch, GIOCondition cond, gpointer p)
{
	char *line = NULL;
	gsize len;
	GIOStatus st = g_io_channel_read_line(ch, &line, &len, NULL, NULL);
	if (st == G_IO_STATUS_EOF || st == G_IO_STATUS_ERROR) {
		g_main_loop_quit(loop);
		return FALSE;
	}
	if (line) {
		char *id, *name, *args[MAXARGS];
		int n;
		if (parse_call(line, &id, &name, args, &n)) call(id, name, args, n);
		else reply(id ? id : "null", "not a call", NULL);
		for (int i = 0; i < n; i++) g_free(args[i]);
		g_free(id);
		g_free(name);
		g_free(line);
	}
	return TRUE;
}

int main(int argc, char **argv)
{
	if (argc < 2 || strcmp(argv[1], "-stdio")) {
		fprintf(stderr, "usage: purple-host -stdio\n");
		return 2;
	}
	asks = g_hash_table_new_full(g_direct_hash, g_direct_equal, NULL, g_free);
	const char *home = getenv("HOME");
	char *dir = g_build_filename(home ? home : "/tmp", ".purple", NULL);
	purple_util_set_user_dir(dir);
	g_free(dir);
	purple_debug_set_ui_ops(&debug_ops);
	purple_core_set_ui_ops(&core_ops);
	purple_eventloop_set_ui_ops(&loop_ops);
	if (!purple_core_init(UI_ID)) {
		fprintf(stderr, "purple-host: libpurple did not start\n");
		return 1;
	}
	if (!purple_host_init_telegram()) {
		fprintf(stderr, "purple-host: tdlib-purple did not register\n");
		return 1;
	}
	purple_set_blist(purple_blist_new());
	purple_blist_load();
	purple_prefs_load();
	purple_savedstatus_activate(purple_savedstatus_new(NULL, PURPLE_STATUS_AVAILABLE));
	GIOChannel *in = g_io_channel_unix_new(0);
	g_io_add_watch(in, G_IO_IN | G_IO_HUP | G_IO_ERR, stdin_cb, NULL);
	signal_out("StateChanged", "ss", state, "");
	loop = g_main_loop_new(NULL, FALSE);
	g_main_loop_run(loop);
	purple_core_quit();
	return 0;
}

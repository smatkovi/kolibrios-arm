/* purple-host -stdio: libpurple without a user interface, with tdlib-purple
 * (Telegram) built in, for KolibriOS's messenger (docs/messengers.md). It
 * speaks the JSON lines of yappari-wa -stdio on standard input and output,
 * the same methods and signals where Yappari has them:
 *
 *   in:  {"id": 1, "call": "Login", "args": ["+43664..."]}
 *   out: {"id": 1, "result": []}            {"id": 1, "error": "..."}
 *        {"signal": "StateChanged", "args": ["connecting", "Connecting"]}
 *
 * Methods: Status() -> (state, account); Login(phone): the account (its
 * name is the phone number, as tdlib-purple has it) enabled; InputReply(id,
 * text), InputCancel(id): an answer to an Input signal; SendText(id, chat,
 * text) -> (id) (also SendText(chat, text)); GetContacts() -> ([chat, name,
 * server name]...) and GetGroups() -> ([chat, title]...) from libpurple's
 * buddy list; MarkRead(chat, sender, ids): accepted (tdlib-purple marks
 * messages read as they are shown); Logout(). Signals: StateChanged(state,
 * text); Input(id, title, primary, secondary, default): what the plugin
 * asks (Telegram's login code, the password); Notice(title, primary,
 * secondary); Message(id, chat, sender, name, time, outgoing, "text",
 * text, "", 0), as Yappari's; Receipt(chat, sender, [id], "server" or
 * "error", time) for what SendText sent; Error(text).
 *
 * The chats are tdlib-purple's conversation names: "id<user id>" a
 * person's (its buddy name, getPurpleBuddyName), "chat<chat id>" a
 * group's (getPurpleChatName), "secret<id>" a secret chat's. Messages get
 * ids made here ("tg-T-N", T the start's time: unique across starts, as
 * the messenger keeps them): libpurple has none. A message sent comes back
 * from tdlib-purple once Telegram has it (tdlib-purple does not echo it
 * at once: "tdlib will shortly present it as a new message"); it keeps
 * SendText's id then and its Receipt "server" follows. Texts are
 * libpurple's markup outside: stripped and unescaped coming in, escaped
 * going out, as tdlib-purple strips it again. Log output goes to standard
 * error (PURPLE_DEBUG=1: libpurple's too). */
#include <glib.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <stdbool.h>
#include <libpurple/purple.h>

#define UI_ID "kolibri"
#define PRPL "telegram-tdlib"

gboolean purple_init_telegram_tdlib_plugin(void);   /* tdlib-purple, a static protocol */

static PurpleAccount *account;
static const char *state = "logged-out";
static unsigned long next_msg = 1, run_id;   /* ids: "tg-RUN-N", RUN this start's time */

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

/* what SendText sent and tdlib-purple has not shown yet: its id, in order
 * per conversation */
typedef struct { char *conv, *id; gint64 at; } Pending;
static GQueue pending = G_QUEUE_INIT;

static Pending *pending_take(const char *conv)
{
	for (GList *l = pending.head; l; l = l->next) {
		Pending *p = l->data;
		if (!strcmp(p->conv, conv)) {
			g_queue_delete_link(&pending, l);
			return p;
		}
	}
	return NULL;
}

static void pending_free(Pending *p)
{
	g_free(p->conv);
	g_free(p->id);
	g_free(p);
}

static void receipt(const char *chat, const char *id, const char *kind)
{
	GString *o = g_string_new("{\"signal\":\"Receipt\",\"args\":[");
	put_str(o, chat);
	g_string_append(o, ",\"\",[");
	put_str(o, id);
	g_string_append(o, "],");
	put_str(o, kind);
	g_string_append_printf(o, ",%ld]}", (long)time(NULL));
	send_line(o);
}

/* sent ones never shown (a minute): given up */
static gboolean pending_expire(gpointer unused)
{
	gint64 now = g_get_monotonic_time();
	for (GList *l = pending.head, *next; l; l = next) {
		Pending *p = l->data;
		next = l->next;
		if (now - p->at < 60 * G_USEC_PER_SEC) continue;
		receipt(p->conv, p->id, "error");
		g_queue_delete_link(&pending, l);
		pending_free(p);
	}
	return TRUE;
}

static void write_conv(PurpleConversation *conv, const char *who, const char *alias,
		       const char *message, PurpleMessageFlags flags, time_t mtime)
{
	const char *chat = purple_conversation_get_name(conv);
	if (flags & PURPLE_MESSAGE_ERROR) {
		/* tdlib-purple's errors in a conversation: a message sent there failed */
		Pending *p = pending_take(chat);
		if (p) receipt(chat, p->id, "error"), pending_free(p);
		signal_out("Error", "s", message ? message : "");
		return;
	}
	if (flags & (PURPLE_MESSAGE_SYSTEM | PURPLE_MESSAGE_NO_LOG) && !(flags & PURPLE_MESSAGE_RECV)) return;
	gboolean out = (flags & PURPLE_MESSAGE_SEND) != 0;
	char *plain = purple_markup_strip_html(message ? message : "");
	char *text = purple_unescape_html(plain);
	Pending *p = out ? pending_take(chat) : NULL;
	char *id = p ? g_strdup(p->id) : g_strdup_printf("tg-%lx-%lu", run_id, next_msg++);
	const char *sender = out && account ? purple_account_get_username(account) : who ? who : "";
	GString *o = g_string_new("{\"signal\":\"Message\",\"args\":[");
	put_str(o, id);
	g_string_append_c(o, ',');
	put_str(o, chat);
	g_string_append_c(o, ',');
	put_str(o, sender);
	g_string_append_c(o, ',');
	put_str(o, out ? "" : alias && strcmp(alias, who ? who : "") ? alias : "");
	g_string_append_printf(o, ",%ld,%s,\"text\",", (long)mtime, out ? "true" : "false");
	put_str(o, text ? text : "");
	g_string_append(o, ",\"\",0]}");
	send_line(o);
	if (p) receipt(chat, id, "server"), pending_free(p);
	g_free(id);
	g_free(text);
	g_free(plain);
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

/* the protocols registered before purple_core_init loads accounts.xml (as
 * Pidgin's static or probed ones are): an account of an unknown protocol
 * gets no statuses there, and would not log in again at the next start.
 * The core's hook before the accounts is the debug UI's start (after the
 * preferences and the debug output; libpurple's own subsystems that
 * tdlib-purple's registration touches, commands and signals, take what it
 * registers early) */
static bool protocols_ok;
static bool test_mode(void);
static void test_register(void);

static void register_protocols(void)
{
	protocols_ok = purple_init_telegram_tdlib_plugin();
	if (test_mode()) test_register();
}

static PurpleCoreUiOps core_ops = { NULL, register_protocols, ui_init, NULL, NULL, NULL, NULL, NULL };

/* --- a protocol for tests (PURPLE_HOST_TEST=1, on the PC) ----------------------
 * "prpl-kolibri-test": an account of any name logs in at once and has two
 * people ("id1001" Anna Berger, "id1002" Bernhard Koller) and a group
 * ("chat-100", Chor Probe) in its buddy list. What is sent comes back as
 * tdlib-purple brings it back (a moment later, PURPLE_MESSAGE_SEND), with
 * a reply "Re: ..." from the other side; after the login a message in the
 * group and one from Anna come in. So the libpurple side of this program
 * runs without Telegram: on the PC (tests/messenger/hosttest.py) and in
 * QEMU (tests/messenger/chatcheck.py: the ramdisk's /etc/purple-host-test
 * switches it on there, the Linux personality having no environment of
 * one's own). */

static bool test_mode(void)
{
	return getenv("PURPLE_HOST_TEST") || access("/etc/purple-host-test", F_OK) == 0;
}

#define TEST_PRPL "prpl-kolibri-test"

static GList *test_status_types(PurpleAccount *acc)
{
	GList *l = g_list_append(NULL, purple_status_type_new_full(PURPLE_STATUS_AVAILABLE, NULL, NULL, TRUE, TRUE, FALSE));
	return g_list_append(l, purple_status_type_new_full(PURPLE_STATUS_OFFLINE, NULL, NULL, TRUE, TRUE, FALSE));
}

static const char *test_list_icon(PurpleAccount *acc, PurpleBuddy *b) { return "test"; }

static gboolean test_hello(gpointer p);

static void test_login(PurpleAccount *acc)
{
	PurpleConnection *gc = purple_account_get_connection(acc);
	static const char *const people[2][2] = { { "id1001", "Anna Berger" }, { "id1002", "Bernhard Koller" } };
	for (int i = 0; i < 2; i++)
		if (!purple_find_buddy(acc, people[i][0]))
			purple_blist_add_buddy(purple_buddy_new(acc, people[i][0], people[i][1]), NULL, NULL, NULL);
	if (!purple_blist_find_chat(acc, "chat-100")) {
		GHashTable *c = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);
		g_hash_table_insert(c, g_strdup("id"), g_strdup("chat-100"));
		purple_blist_add_chat(purple_chat_new(acc, "Chor Probe", c), NULL, NULL);
	}
	purple_connection_set_state(gc, PURPLE_CONNECTED);
	g_timeout_add(500, test_hello, acc);
}

/* the first messages: the group's (its conversation joined), then Anna's */
static gboolean test_hello(gpointer p)
{
	PurpleAccount *acc = p;
	PurpleConnection *gc = purple_account_get_connection(acc);
	if (!gc) return FALSE;
	time_t now = time(NULL);
	serv_got_joined_chat(gc, 100, "chat-100");
	serv_got_chat_in(gc, 100, "Bernhard Koller", PURPLE_MESSAGE_RECV, "Probe morgen 19:00", now - 60);
	serv_got_im(gc, "id1001", "Hallo from the test protocol", PURPLE_MESSAGE_RECV, now);
	return FALSE;
}

static void test_close(PurpleConnection *gc) {}

typedef struct { PurpleAccount *acc; char *who, *text; int chat; } TestEcho;

static gboolean test_echo(gpointer p)
{
	TestEcho *e = p;
	PurpleConnection *gc = purple_account_get_connection(e->acc);
	char *re = g_strdup_printf("Re: %s", e->text);
	time_t now = time(NULL);
	if (gc && e->chat) {
		PurpleConversation *c = purple_find_chat(gc, e->chat);
		if (c) purple_conv_chat_write(PURPLE_CONV_CHAT(c), purple_account_get_username(e->acc), e->text,
					      PURPLE_MESSAGE_SEND, now);
		serv_got_chat_in(gc, e->chat, "Anna Berger", PURPLE_MESSAGE_RECV, re, now);
	} else if (gc) {
		PurpleConversation *c = purple_find_conversation_with_account(PURPLE_CONV_TYPE_IM, e->who, e->acc);
		if (!c) c = purple_conversation_new(PURPLE_CONV_TYPE_IM, e->acc, e->who);
		purple_conv_im_write(PURPLE_CONV_IM(c), purple_account_get_username(e->acc), e->text, PURPLE_MESSAGE_SEND, now);
		serv_got_im(gc, e->who, re, PURPLE_MESSAGE_RECV, now);
	}
	g_free(re);
	g_free(e->who);
	g_free(e->text);
	g_free(e);
	return FALSE;
}

static int test_send(PurpleConnection *gc, const char *who, int chat, const char *message)
{
	TestEcho *e = g_new0(TestEcho, 1);
	e->acc = purple_connection_get_account(gc);
	e->who = g_strdup(who ? who : "");
	e->text = g_strdup(message);
	e->chat = chat;
	g_timeout_add(200, test_echo, e);
	return 0;                                /* not echoed now: tdlib-purple's way */
}

static int test_send_im(PurpleConnection *gc, const char *who, const char *message, PurpleMessageFlags f)
{
	return test_send(gc, who, 0, message);
}

static int test_chat_send(PurpleConnection *gc, int id, const char *message, PurpleMessageFlags f)
{
	return test_send(gc, NULL, id, message);
}

static GList *test_chat_info(PurpleConnection *gc)
{
	struct proto_chat_entry *e = g_new0(struct proto_chat_entry, 1);
	e->label = "id";
	e->identifier = "id";
	e->required = TRUE;
	return g_list_append(NULL, e);
}

static void test_join_chat(PurpleConnection *gc, GHashTable *components)
{
	const char *id = g_hash_table_lookup(components, "id");
	if (id) serv_got_joined_chat(gc, 100, id);
}

static PurplePluginProtocolInfo test_prpl_info = {
	.options = OPT_PROTO_NO_PASSWORD,
	.list_icon = test_list_icon,
	.status_types = test_status_types,
	.chat_info = test_chat_info,
	.login = test_login,
	.close = test_close,
	.send_im = test_send_im,
	.join_chat = test_join_chat,
	.chat_send = test_chat_send,
	.struct_size = sizeof(PurplePluginProtocolInfo),
};

static PurplePluginInfo test_plugin_info = {
	.magic = PURPLE_PLUGIN_MAGIC,
	.major_version = PURPLE_MAJOR_VERSION,
	.minor_version = PURPLE_MINOR_VERSION,
	.type = PURPLE_PLUGIN_PROTOCOL,
	.priority = PURPLE_PRIORITY_DEFAULT,
	.id = TEST_PRPL,
	.name = "KolibriOS test",
	.version = "1",
	.summary = "purple-host's protocol for tests",
	.extra_info = &test_prpl_info,
};

static void test_register(void)
{
	PurplePlugin *plugin = purple_plugin_new(TRUE, NULL);
	plugin->info = &test_plugin_info;
	purple_plugin_load(plugin);
	purple_plugin_register(plugin);
}

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
				else if (*p == '[' || *p == '{') {
					/* an array or object (MarkRead's ids): its text as it is */
					const char *e = p;
					int depth = 0;
					for (; *e; e++) {
						if (*e == '"') {
							for (e++; *e && *e != '"'; e++)
								if (*e == '\\' && e[1]) e++;
							if (!*e) break;
						} else if (*e == '[' || *e == '{') depth++;
						else if ((*e == ']' || *e == '}') && !--depth) { e++; break; }
					}
					v = g_strndup(p, e - p);
					p = e;
				} else {
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
			const char *prpl = test_mode() ? TEST_PRPL : PRPL;
			account = purple_accounts_find(a[0], prpl);
			if (!account) {
				account = purple_account_new(a[0], prpl);
				purple_accounts_add(account);
			}
		}
		purple_account_set_enabled(account, UI_ID, TRUE);
		reply(id, NULL, NULL);
	} else if ((!strcmp(name, "InputReply") && n == 2) || (!strcmp(name, "InputCancel") && n == 1)) {
		answer((guint)strtoul(a[0], NULL, 10), n == 2 ? a[1] : NULL, n == 2);
		reply(id, NULL, NULL);
	} else if (!strcmp(name, "SendText") && (n == 2 || n == 3)) {
		/* (id, chat, text) as Yappari's, or (chat, text) */
		const char *chat = a[n - 2], *body = a[n - 1];
		PurpleConnection *gc = account ? purple_account_get_connection(account) : NULL;
		if (!gc || strcmp(state, "connected")) {
			reply(id, "not connected", NULL);
			return;
		}
		char *markup = g_markup_escape_text(body, -1);
		if (!strncmp(chat, "chat", 4)) {
			PurpleConversation *c = purple_find_conversation_with_account(PURPLE_CONV_TYPE_CHAT, chat, account);
			if (!c) {
				/* the group's conversation opened first (tdlib-purple joins it) */
				PurpleChat *bc = purple_blist_find_chat(account, chat);
				if (bc) serv_join_chat(gc, purple_chat_get_components(bc));
				reply(id, bc ? "the group is being opened: send again" : "no such group", NULL);
				g_free(markup);
				return;
			}
			purple_conv_chat_send(PURPLE_CONV_CHAT(c), markup);
		} else {
			PurpleConversation *c = purple_find_conversation_with_account(PURPLE_CONV_TYPE_IM, chat, account);
			if (!c) c = purple_conversation_new(PURPLE_CONV_TYPE_IM, account, chat);
			purple_conv_im_send(PURPLE_CONV_IM(c), markup);
		}
		g_free(markup);
		Pending *p = g_new0(Pending, 1);
		p->conv = g_strdup(chat);
		p->id = n == 3 && *a[0] ? g_strdup(a[0]) : g_strdup_printf("tg-%lx-%lu", run_id, next_msg++);
		p->at = g_get_monotonic_time();
		g_queue_push_tail(&pending, p);
		GString *r = g_string_new(NULL);
		put_str(r, p->id);
		reply(id, NULL, r->str);
		g_string_free(r, TRUE);
	} else if (!strcmp(name, "GetContacts") || !strcmp(name, "GetGroups")) {
		/* the buddy list: people (buddy name, alias, server alias), groups
		 * (the "id" component, which is the conversation's name; the title) */
		gboolean groups = name[3] == 'G';
		GString *r = g_string_new("[");
		int k = 0;
		for (PurpleBlistNode *b = purple_blist_get_root(); account && b; b = purple_blist_node_next(b, TRUE)) {
			if (!groups && PURPLE_BLIST_NODE_IS_BUDDY(b) && purple_buddy_get_account((PurpleBuddy *)b) == account) {
				PurpleBuddy *bd = (PurpleBuddy *)b;
				const char *al = purple_buddy_get_alias_only(bd), *sv = purple_buddy_get_server_alias(bd);
				g_string_append(r, k++ ? ",[" : "[");
				put_str(r, purple_buddy_get_name(bd));
				g_string_append_c(r, ',');
				put_str(r, al ? al : "");
				g_string_append_c(r, ',');
				put_str(r, sv ? sv : "");
				g_string_append_c(r, ']');
			} else if (groups && PURPLE_BLIST_NODE_IS_CHAT(b) && purple_chat_get_account((PurpleChat *)b) == account) {
				PurpleChat *ch = (PurpleChat *)b;
				const char *cid = g_hash_table_lookup(purple_chat_get_components(ch), "id");
				if (!cid) continue;
				g_string_append(r, k++ ? ",[" : "[");
				put_str(r, cid);
				g_string_append_c(r, ',');
				put_str(r, purple_chat_get_name(ch));
				g_string_append_c(r, ']');
			}
		}
		g_string_append_c(r, ']');
		reply(id, NULL, r->str);
		g_string_free(r, TRUE);
	} else if (!strcmp(name, "MarkRead") && n == 3) {
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
	run_id = (unsigned long)time(NULL);
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
	if (!protocols_ok) {
		fprintf(stderr, "purple-host: tdlib-purple did not register\n");
		return 1;
	}
	purple_set_blist(purple_blist_new());
	purple_blist_load();
	purple_prefs_load();
	/* an account from before (accounts.xml in ~/.purple): logged in again
	 * by itself when it is enabled (TDLib's database is in ~/.purple too) */
	for (GList *l = purple_accounts_get_all(); l && !account; l = l->next) {
		const char *id = purple_account_get_protocol_id(l->data);
		if (!strcmp(id, PRPL) || (test_mode() && !strcmp(id, TEST_PRPL))) account = l->data;
	}
	if (account && purple_account_get_enabled(account, UI_ID)) state = "connecting";
	purple_savedstatus_activate(purple_savedstatus_new(NULL, PURPLE_STATUS_AVAILABLE));
	purple_accounts_restore_current_statuses();     /* as Pidgin at its start: enabled ones connect */
	GIOChannel *in = g_io_channel_unix_new(0);
	g_io_add_watch(in, G_IO_IN | G_IO_HUP | G_IO_ERR, stdin_cb, NULL);
	signal_out("StateChanged", "ss", state, "");
	g_timeout_add_seconds(10, pending_expire, NULL);
	loop = g_main_loop_new(NULL, FALSE);
	g_main_loop_run(loop);
	purple_core_quit();
	return 0;
}

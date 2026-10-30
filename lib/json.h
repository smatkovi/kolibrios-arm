/* KolibriOS for ARM: a small JSON reader for programs (the messenger's
 * JSON lines, docs/messengers.md). A text is parsed into nodes in an
 * array the caller gives (no allocation): strings decoded into a buffer
 * the caller gives too (as large as the text: decoded strings are never
 * longer), "\uXXXX" escapes into UTF-8 (surrogate pairs joined), numbers
 * as 64-bit integers (a fraction or an exponent read past), objects with
 * their keys. Header only: static functions. */
#ifndef JSON_H
#define JSON_H

enum { J_NULL, J_FALSE, J_TRUE, J_NUM, J_STR, J_ARR, J_OBJ };

struct jv {
	unsigned char type;
	int child, next, n;            /* the first element, the next sibling (-1: none), elements */
	const char *key;               /* in an object: its key; else NULL */
	const char *s;                 /* a string's text (decoded, NUL-terminated) */
	long long num;
};

struct json {
	struct jv *v;
	int nv, max;
	char *sb;
	unsigned sbn, sbmax;
	const char *p;
	int depth;
};

static void json_init(struct json *j, struct jv *nodes, int max, char *strbuf, unsigned strmax)
{
	j->v = nodes;
	j->max = max;
	j->sb = strbuf;
	j->sbmax = strmax;
	j->nv = 0;
	j->sbn = 0;
}

static void json_ws(struct json *j)
{
	while (*j->p == ' ' || *j->p == '\t' || *j->p == '\r' || *j->p == '\n') j->p++;
}

static int json_hex4(const char *p)
{
	int v = 0;
	for (int i = 0; i < 4; i++) {
		char c = p[i];
		int d = c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 :
			c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
		if (d < 0) return -1;
		v = v * 16 + d;
	}
	return v;
}

static void json_put(struct json *j, char c)
{
	if (j->sbn + 1 < j->sbmax) j->sb[j->sbn++] = c;
}

static void json_utf8(struct json *j, unsigned u)
{
	if (u < 0x80) json_put(j, (char)u);
	else if (u < 0x800) json_put(j, (char)(0xc0 | u >> 6)), json_put(j, (char)(0x80 | (u & 0x3f)));
	else if (u < 0x10000) {
		json_put(j, (char)(0xe0 | u >> 12));
		json_put(j, (char)(0x80 | (u >> 6 & 0x3f)));
		json_put(j, (char)(0x80 | (u & 0x3f)));
	} else {
		json_put(j, (char)(0xf0 | u >> 18));
		json_put(j, (char)(0x80 | (u >> 12 & 0x3f)));
		json_put(j, (char)(0x80 | (u >> 6 & 0x3f)));
		json_put(j, (char)(0x80 | (u & 0x3f)));
	}
}

/* a string at p (its opening quote): decoded into the buffer */
static const char *json_string(struct json *j)
{
	const char *start = j->sb + j->sbn;
	j->p++;
	while (*j->p && *j->p != '"') {
		char c = *j->p++;
		if (c != '\\') { json_put(j, c); continue; }
		c = *j->p++;
		switch (c) {
		case 'n': json_put(j, '\n'); break;
		case 't': json_put(j, '\t'); break;
		case 'r': json_put(j, '\r'); break;
		case 'b': json_put(j, '\b'); break;
		case 'f': json_put(j, '\f'); break;
		case 'u': {
			int u = json_hex4(j->p);
			if (u < 0) { json_put(j, '?'); break; }
			j->p += 4;
			if (u >= 0xd800 && u < 0xdc00 && j->p[0] == '\\' && j->p[1] == 'u') {
				int lo = json_hex4(j->p + 2);
				if (lo >= 0xdc00 && lo < 0xe000) {
					j->p += 6;
					u = 0x10000 + ((u - 0xd800) << 10) + (lo - 0xdc00);
				}
			}
			if (u >= 0xd800 && u < 0xe000) u = 0xfffd;   /* a lone half */
			json_utf8(j, (unsigned)u);
			break;
		}
		case 0: j->p--; break;
		default: json_put(j, c); break;
		}
	}
	if (*j->p == '"') j->p++;
	if (j->sbn < j->sbmax) j->sb[j->sbn++] = 0;
	else j->sb[j->sbmax - 1] = 0;
	return start;
}

static int json_value(struct json *j, const char *key);

/* the elements of an array or the members of an object: linked to v */
static int json_list(struct json *j, int v, char close)
{
	int last = -1;
	j->p++;
	for (;;) {
		json_ws(j);
		if (*j->p == close) { j->p++; return 0; }
		if (!*j->p) return -1;
		const char *key = 0;
		if (close == '}') {
			if (*j->p != '"') return -1;
			key = json_string(j);
			json_ws(j);
			if (*j->p++ != ':') return -1;
			json_ws(j);
		}
		int c = json_value(j, key);
		if (c < 0) return -1;
		if (last < 0) j->v[v].child = c; else j->v[last].next = c;
		last = c;
		j->v[v].n++;
		json_ws(j);
		if (*j->p == ',') j->p++;
		else if (*j->p != close) return -1;
	}
}

static int json_value(struct json *j, const char *key)
{
	if (j->nv >= j->max || j->depth > 32) return -1;
	int i = j->nv++;
	struct jv *v = &j->v[i];
	v->child = v->next = -1;
	v->n = 0;
	v->key = key;
	v->s = "";
	v->num = 0;
	char c = *j->p;
	if (c == '"') {
		v->type = J_STR;
		v->s = json_string(j);
	} else if (c == '[' || c == '{') {
		v->type = c == '[' ? J_ARR : J_OBJ;
		j->depth++;
		int r = json_list(j, i, c == '[' ? ']' : '}');
		j->depth--;
		if (r < 0) return -1;
	} else if (c == '-' || (c >= '0' && c <= '9')) {
		int neg = c == '-';
		if (neg) j->p++;
		long long n = 0;
		while (*j->p >= '0' && *j->p <= '9') n = n * 10 + (*j->p++ - '0');
		while (*j->p == '.' || *j->p == 'e' || *j->p == 'E' || *j->p == '+' || *j->p == '-' ||
		       (*j->p >= '0' && *j->p <= '9'))
			j->p++;
		v->type = J_NUM;
		v->num = neg ? -n : n;
	} else if (c == 't' && j->p[1] == 'r' && j->p[2] == 'u' && j->p[3] == 'e') {
		v->type = J_TRUE;
		j->p += 4;
	} else if (c == 'f' && j->p[1] == 'a' && j->p[2] == 'l' && j->p[3] == 's' && j->p[4] == 'e') {
		v->type = J_FALSE;
		j->p += 5;
	} else if (c == 'n' && j->p[1] == 'u' && j->p[2] == 'l' && j->p[3] == 'l') {
		v->type = J_NULL;
		j->p += 4;
	} else {
		return -1;
	}
	return i;
}

/* the text parsed: its root node (0), or -1 (not JSON, or too large for
 * the nodes or the buffer) */
static int json_parse(struct json *j, const char *text)
{
	j->nv = 0;
	j->sbn = 0;
	j->depth = 0;
	j->p = text;
	json_ws(j);
	return json_value(j, 0);
}

static int json_key_eq(const char *a, const char *b)
{
	while (*a && *a == *b) a++, b++;
	return *a == *b;
}

/* an object's member by its key, an array's element by its index: the
 * node, or -1 */
static int json_get(const struct json *j, int obj, const char *key)
{
	if (obj < 0 || j->v[obj].type != J_OBJ) return -1;
	for (int c = j->v[obj].child; c >= 0; c = j->v[c].next)
		if (json_key_eq(j->v[c].key, key)) return c;
	return -1;
}

static int json_at(const struct json *j, int arr, int i)
{
	if (arr < 0 || (j->v[arr].type != J_ARR && j->v[arr].type != J_OBJ)) return -1;
	int c = j->v[arr].child;
	while (c >= 0 && i-- > 0) c = j->v[c].next;
	return c;
}

static inline int json_len(const struct json *j, int v) { return v >= 0 ? j->v[v].n : 0; }
static inline int json_type(const struct json *j, int v) { return v >= 0 ? j->v[v].type : -1; }

/* a value as a string ("" when it is none), a number (0), a truth */
static const char *json_str(const struct json *j, int v)
{
	return v >= 0 && j->v[v].type == J_STR ? j->v[v].s : "";
}

static long long json_num(const struct json *j, int v)
{
	if (v < 0) return 0;
	if (j->v[v].type == J_NUM) return j->v[v].num;
	if (j->v[v].type == J_STR) {           /* numbers sent as strings (Signal's ids) */
		long long n = 0;
		for (const char *p = j->v[v].s; *p >= '0' && *p <= '9'; p++) n = n * 10 + (*p - '0');
		return n;
	}
	return 0;
}

static inline int json_true(const struct json *j, int v) { return v >= 0 && j->v[v].type == J_TRUE; }

/* writing: a string as JSON (quoted, escaped; UTF-8 kept) into out, at
 * most cap bytes with the NUL: the length */
static unsigned json_quote(char *out, unsigned cap, const char *s)
{
	unsigned o = 0;
	if (cap < 3) return 0;
	out[o++] = '"';
	for (; *s && o + 8 < cap; s++) {
		unsigned char c = (unsigned char)*s;
		if (c == '"' || c == '\\') out[o++] = '\\', out[o++] = (char)c;
		else if (c == '\n') out[o++] = '\\', out[o++] = 'n';
		else if (c == '\t') out[o++] = '\\', out[o++] = 't';
		else if (c == '\r') out[o++] = '\\', out[o++] = 'r';
		else if (c < 32) {
			static const char hx[] = "0123456789abcdef";
			out[o++] = '\\', out[o++] = 'u', out[o++] = '0', out[o++] = '0';
			out[o++] = hx[c >> 4], out[o++] = hx[c & 15];
		} else out[o++] = (char)c;
	}
	out[o++] = '"';
	out[o] = 0;
	return o;
}

#endif

/* See LICENSE file for copyright and license details. */
#include <sys/stat.h>

#include <ctype.h>
#include <dirent.h>
#include <fcntl.h>
#include <limits.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>

#include "desktop.h"
#include "util.h"

#define DESKTOP_SUFFIX     ".desktop"
#define DESKTOP_SUFFIX_LEN (sizeof DESKTOP_SUFFIX - 1)
#define MAX_DESKTOP_SIZE   (1024 * 1024) /* ignore larger .desktop files */

/* One parsed [Desktop Entry] */
struct entry {
	char *name;
	char *exec;
	char *icon;
	char *try_exec;
	char *only_show_in;
	char *not_show_in;
	int   no_display;
	int   hidden;
	int   terminal;
};

/* Next capacity when growing an array: doubles, without wrapping. */
static size_t
grown(size_t cap)
{
	if (cap > SIZE_MAX / 2)
		die("too many entries");
	return cap ? cap * 2 : 64;
}

/* ---- tiny string builder ------------------------------------------- */

struct sbuf { char *s; size_t len, cap; };

static void
sbuf_grow(struct sbuf *b, size_t need)
{
	size_t want, n;

	if (need > SIZE_MAX - b->len - 1)
		die("string too long");
	want = b->len + need + 1;
	if (want <= b->cap)
		return;
	n = b->cap ? b->cap : 64;
	while (n < want) {
		if (n > SIZE_MAX / 2) {
			n = want;
			break;
		}
		n *= 2;
	}
	b->s = ereallocarray(b->s, n, 1);
	b->cap = n;
}

static void
sbuf_add(struct sbuf *b, const char *s)
{
	size_t l = strlen(s);

	sbuf_grow(b, l);
	memcpy(b->s + b->len, s, l + 1);
	b->len += l;
}

static void
sbuf_addc(struct sbuf *b, char c)
{
	sbuf_grow(b, 1);
	b->s[b->len++] = c;
	b->s[b->len] = '\0';
}

/* Append s as one shell word: 'it'\''s' */
static void
sbuf_addquoted(struct sbuf *b, const char *s)
{
	sbuf_addc(b, '\'');
	for (; *s; s++) {
		if (*s == '\'')
			sbuf_add(b, "'\\''");
		else
			sbuf_addc(b, *s);
	}
	sbuf_addc(b, '\'');
}

/* Hand the buffer's contents to the caller (never NULL) and reset it. */
static char *
sbuf_take(struct sbuf *b)
{
	char *s = b->s ? b->s : estrdup("");

	memset(b, 0, sizeof *b);
	return s;
}

/* ---- .desktop parsing --------------------------------------------- */

static char *
trim(char *s)
{
	char *end;

	while (*s && isspace((unsigned char)*s))
		s++;
	if (!*s)
		return s;
	end = s + strlen(s) - 1;
	while (end > s && isspace((unsigned char)*end))
		*end-- = '\0';
	return s;
}

/* decode \n \t \r \s \\ \; according to the spec */
static char *
unescape(const char *in)
{
	struct sbuf b = {0};

	for (const char *p = in; *p; p++) {
		if (*p != '\\' || !p[1]) {
			sbuf_addc(&b, *p);
			continue;
		}
		switch (*++p) {
		case 'n':  sbuf_addc(&b, '\n'); break;
		case 't':  sbuf_addc(&b, '\t'); break;
		case 'r':  sbuf_addc(&b, '\r'); break;
		case 's':  sbuf_addc(&b, ' ');  break;
		default:   sbuf_addc(&b, *p);   break; /* \\ \; and unknown */
		}
	}
	return sbuf_take(&b);
}

static int
parse_bool(const char *v)
{
	return !strcmp(v, "true") || !strcmp(v, "1");
}

/* keys of [Desktop Entry] we care about, and where they are stored */
static const struct { const char *key; size_t off; } strkeys[] = {
	{ "Name",       offsetof(struct entry, name)         },
	{ "Exec",       offsetof(struct entry, exec)         },
	{ "Icon",       offsetof(struct entry, icon)         },
	{ "TryExec",    offsetof(struct entry, try_exec)     },
	{ "OnlyShowIn", offsetof(struct entry, only_show_in) },
	{ "NotShowIn",  offsetof(struct entry, not_show_in)  },
};
static const struct { const char *key; size_t off; } boolkeys[] = {
	{ "NoDisplay",  offsetof(struct entry, no_display)   },
	{ "Hidden",     offsetof(struct entry, hidden)       },
	{ "Terminal",   offsetof(struct entry, terminal)     },
};

static void
set_field(struct entry *e, const char *key, const char *val)
{
	size_t i;

	for (i = 0; i < LENGTH(strkeys); i++)
		if (!strcmp(key, strkeys[i].key)) {
			char **dst = (char **)((char *)e + strkeys[i].off);

			free(*dst);
			*dst = unescape(val);
			return;
		}
	for (i = 0; i < LENGTH(boolkeys); i++)
		if (!strcmp(key, boolkeys[i].key)) {
			*(int *)((char *)e + boolkeys[i].off) = parse_bool(val);
			return;
		}
}

/*
 * Open path only if it is a regular file of sane size. The data dirs are
 * user-writable, and dmenu holds the keyboard grab while it scans them, so a
 * FIFO or device named *.desktop must not be able to block (O_NONBLOCK makes
 * open() on a FIFO return at once) and a huge file must not eat all memory.
 * Symlinks are followed on purpose: distributions install .desktop files that way.
 */
static FILE *
open_regular(const char *path)
{
	struct stat st;
	FILE *f;
	int fd = open(path, O_RDONLY | O_NONBLOCK | O_CLOEXEC);

	if (fd < 0)
		return NULL;
	if (fstat(fd, &st) || !S_ISREG(st.st_mode) || st.st_size > MAX_DESKTOP_SIZE ||
	    !(f = fdopen(fd, "r"))) {
		close(fd);
		return NULL;
	}
	return f;
}

/* Parse one file. Return 0 on success and fill e; -1 otherwise. */
static int
parse_desktop_file(const char *path, struct entry *e)
{
	FILE *f = open_regular(path);
	char *line = NULL;
	size_t linesiz = 0, total = 0;
	ssize_t n;
	int in_section = 0, type_ok = 0, err;

	if (!f)
		return -1;

	while ((n = getline(&line, &linesiz, f)) != -1) {
		char *s, *eq;

		/* the file may have grown since fstat(): enforce the cap again */
		if ((total += (size_t)n) > MAX_DESKTOP_SIZE) {
			type_ok = 0;
			break;
		}
		s = trim(line);

		if (!*s || *s == '#')
			continue;
		if (*s == '[') {
			if (in_section) /* [Desktop Entry] is over: ignore actions */
				break;
			in_section = !strcmp(s, "[Desktop Entry]");
			continue;
		}
		if (!in_section || !(eq = strchr(s, '=')))
			continue;
		*eq = '\0';
		s = trim(s);
		eq = trim(eq + 1);

		if (!strcmp(s, "Type"))
			type_ok = !strcmp(eq, "Application");
		else
			set_field(e, s, eq);
	}
	free(line);
	err = ferror(f);
	fclose(f);

	if (err || !type_ok || !e->exec || !*e->exec || !e->name || !*e->name)
		return -1;
	return 0;
}

static void
entry_free(struct entry *e)
{
	free(e->name);
	free(e->exec);
	free(e->icon);
	free(e->try_exec);
	free(e->only_show_in);
	free(e->not_show_in);
}

/* ---- Exec parsing and field-code expansion ------------------------ */

/*
 * Exec= is not a shell command line. The Desktop Entry spec defines its own
 * quoting (double quotes, with \" \` \$ \\ escapes inside them) and field
 * codes (%f %F %u %U %i %c %k %%). We parse it into an argv that is run with
 * execvp(), so shell syntax in a .desktop file (; | & $(...) `...` > ...) is
 * plain argument text and is never interpreted.
 */
struct argvec { char **v; size_t n, cap; };

static void
argvec_push(struct argvec *a, char *s)
{
	/* room for the new entry and the NULL terminator */
	if (a->n + 2 > a->cap) {
		size_t cap = grown(a->cap);

		a->v = ereallocarray(a->v, cap, sizeof *a->v);
		a->cap = cap;
	}
	a->v[a->n++] = s;
	a->v[a->n] = NULL;
}

void
desktop_argv_free(char **argv)
{
	size_t i;

	if (!argv)
		return;
	for (i = 0; argv[i]; i++)
		free(argv[i]);
	free(argv);
}

static char *
dupn(const char *s, size_t n)
{
	char *p = ecalloc(n + 1, 1);

	memcpy(p, s, n);
	return p;
}

/*
 * Split exec into arguments, expanding field codes. Returns a NULL-terminated
 * vector, or NULL if exec is malformed (unterminated quote) or empty.
 */
static char **
parse_exec(const char *exec, const char *name, const char *icon, const char *path)
{
	struct argvec args = {0};
	const char *p = exec;

	for (;;) {
		struct sbuf w = {0};
		int quoted = 0, have = 0;

		while (*p == ' ' || *p == '\t')
			p++;
		if (!*p)
			break;

		/* a lone %i is two arguments: --icon <name> */
		if (p[0] == '%' && p[1] == 'i' && (!p[2] || p[2] == ' ' || p[2] == '\t')) {
			if (icon && *icon) {
				argvec_push(&args, estrdup("--icon"));
				argvec_push(&args, estrdup(icon));
			}
			p += 2;
			continue;
		}

		for (; *p && (quoted || (*p != ' ' && *p != '\t')); p++) {
			if (*p == '"') {
				quoted = !quoted;
				have = 1; /* "" is an empty argument */
				continue;
			}
			if (quoted && *p == '\\' && p[1] && strchr("\"`$\\", p[1])) {
				p++; /* escaped character: take it literally */
			} else if (*p == '%') {
				switch (*++p) {
				case '%':
					sbuf_addc(&w, '%');
					have = 1;
					break;
				case 'i':
					if (icon && *icon) {
						sbuf_add(&w, icon);
						have = 1;
					}
					break;
				case 'c':
					sbuf_add(&w, name);
					have = 1;
					break;
				case 'k':
					if (path) {
						sbuf_add(&w, path);
						have = 1;
					}
					break;
				case '\0':
					p--; /* trailing '%': let the loop end */
					break;
				default:
					/* %f %F %u %U (no file or URL is supplied) and
					 * the deprecated codes expand to nothing */
					break;
				}
				continue;
			}
			sbuf_addc(&w, *p);
			have = 1;
		}
		if (quoted) { /* unterminated quote */
			free(w.s);
			desktop_argv_free(args.v);
			return NULL;
		}
		if (have)
			argvec_push(&args, sbuf_take(&w));
		else
			free(w.s); /* e.g. a bare %U: no argument at all */
	}
	return args.v; /* NULL if there was nothing to run */
}

/* Does s need shell quoting to be shown as a command line? */
static int
needs_quote(const char *s)
{
	if (!*s)
		return 1;
	for (; *s; s++)
		if (!isalnum((unsigned char)*s) && !strchr("_@%+=:,./-", *s))
			return 1;
	return 0;
}

/* Join argv into one shell-safe line (for display and Shift+Enter output). */
static char *
argv_to_string(char **argv)
{
	struct sbuf b = {0};
	size_t i;

	for (i = 0; argv[i]; i++) {
		if (i)
			sbuf_addc(&b, ' ');
		if (needs_quote(argv[i]))
			sbuf_addquoted(&b, argv[i]);
		else
			sbuf_add(&b, argv[i]);
	}
	return sbuf_take(&b);
}

/* Prefix argv with $TERMINAL (default xterm) and -e. Takes over argv. */
static char **
wrap_in_terminal(char **argv)
{
	struct argvec out = {0};
	const char *term = getenv("TERMINAL"), *p;
	size_t i, n;

	for (p = term && *term ? term : ""; *p; p += n) {
		while (*p == ' ' || *p == '\t')
			p++;
		n = strcspn(p, " \t");
		if (n)
			argvec_push(&out, dupn(p, n));
	}
	if (!out.n)
		argvec_push(&out, estrdup("xterm"));
	argvec_push(&out, estrdup("-e"));
	for (i = 0; argv[i]; i++)
		argvec_push(&out, argv[i]);
	free(argv); /* the strings now belong to out */
	return out.v;
}

/* ---- XDG filtering ------------------------------------------------- */

/* Is tok[0..len) one of the items of the `sep`-separated list? */
static int
list_has(const char *list, char sep, const char *tok, size_t len)
{
	const char set[2] = { sep, '\0' };

	while (*list) {
		size_t n = strcspn(list, set);

		if (n == len && !strncmp(list, tok, len))
			return 1;
		list += n + (list[n] != '\0');
	}
	return 0;
}

/*
 * Does any component of `current` (XDG_CURRENT_DESKTOP, a ':' separated
 * list such as "ubuntu:GNOME") appear in `list` (OnlyShowIn/NotShowIn, a
 * ';' separated list)?
 */
static int
list_matches_desktop(const char *list, const char *current)
{
	while (*current) {
		size_t n = strcspn(current, ":");

		if (n && list_has(list, ';', current, n))
			return 1;
		current += n + (current[n] != '\0');
	}
	return 0;
}

static int
matches_current_desktop(const struct entry *e, const char *cur)
{
	if (!cur || !*cur)
		return 1;
	if (e->only_show_in && !list_matches_desktop(e->only_show_in, cur))
		return 0;
	if (e->not_show_in && list_matches_desktop(e->not_show_in, cur))
		return 0;
	return 1;
}

static int
binary_exists(const char *prog)
{
	const char *path, *p;
	char buf[PATH_MAX];
	size_t n;
	int r;

	if (!*prog)
		return 0;
	if (strchr(prog, '/') || !(path = getenv("PATH")))
		return access(prog, X_OK) == 0;

	for (p = path; *p; p += n + (p[n] != '\0')) {
		n = strcspn(p, ":");
		if (!n || n > PATH_MAX)
			continue;
		r = snprintf(buf, sizeof buf, "%.*s/%s", (int)n, p, prog);
		if (r > 0 && (size_t)r < sizeof buf && access(buf, X_OK) == 0)
			return 1;
	}
	return 0;
}

static int
app_is_visible(const struct entry *e)
{
	if (e->hidden || e->no_display)
		return 0;
	if (!matches_current_desktop(e, getenv("XDG_CURRENT_DESKTOP")))
		return 0;
	if (e->try_exec && !binary_exists(e->try_exec))
		return 0;
	return 1;
}

/* ---- convert entry -> DesktopApp ---------------------------------- */

/* Returns NULL if the entry's Exec= cannot be parsed into a command. */
static DesktopApp *
entry_to_app(const struct entry *e, const char *desktop_path)
{
	DesktopApp *app;
	char **argv = parse_exec(e->exec, e->name, e->icon, desktop_path);
	char *c;

	if (!argv)
		return NULL;
	if (e->terminal)
		argv = wrap_in_terminal(argv);

	app = ecalloc(1, sizeof *app);
	app->argv = argv;
	app->exec = argv_to_string(argv);
	app->name = estrdup(e->name);
	/* a menu item is one line: neutralise newlines and other controls */
	for (c = app->name; *c; c++)
		if ((unsigned char)*c < 0x20 || *c == 0x7f)
			*c = ' ';
	return app;
}

static int
cmp_apps(const void *a, const void *b)
{
	const DesktopApp *aa = *(const DesktopApp * const *)a;
	const DesktopApp *bb = *(const DesktopApp * const *)b;
	int r = strcasecmp(aa->name, bb->name);

	/* qsort is not stable: break ties so the order is deterministic */
	return r ? r : strcmp(aa->exec, bb->exec);
}

/* ---- directory scanning ------------------------------------------- */

struct app_list { DesktopApp **v; size_t count, cap; };

static void
app_list_push(struct app_list *l, DesktopApp *app)
{
	/* always keep one spare slot for the NULL terminator */
	if (l->count + 2 > l->cap) {
		size_t n = grown(l->cap);

		l->v = ereallocarray(l->v, n, sizeof *l->v);
		l->cap = n;
	}
	l->v[l->count++] = app;
}

/* Dedup: keep the first .desktop file seen for a given basename. */
struct seen_list { char **names; size_t count, cap; };

/* Record base; return 0 if it was already present, 1 if it is new. */
static int
seen_first(struct seen_list *s, const char *base)
{
	size_t i;

	for (i = 0; i < s->count; i++)
		if (!strcmp(s->names[i], base))
			return 0;
	if (s->count + 1 > s->cap) {
		size_t n = grown(s->cap);

		s->names = ereallocarray(s->names, n, sizeof *s->names);
		s->cap = n;
	}
	s->names[s->count++] = estrdup(base);
	return 1;
}

static int
is_desktop_file(const char *name)
{
	size_t len = strlen(name);

	return len > DESKTOP_SUFFIX_LEN &&
	       !strcmp(name + len - DESKTOP_SUFFIX_LEN, DESKTOP_SUFFIX);
}

static void
load_dir(const char *dir, struct seen_list *seen, struct app_list *apps)
{
	DIR *d = opendir(dir);
	struct dirent *de;

	if (!d)
		return;

	while ((de = readdir(d))) {
		char path[PATH_MAX];
		struct entry e = {0};
		int r;

		if (!is_desktop_file(de->d_name) || !seen_first(seen, de->d_name))
			continue;
		r = snprintf(path, sizeof path, "%s/%s", dir, de->d_name);
		if (r < 0 || (size_t)r >= sizeof path)
			continue;

		if (parse_desktop_file(path, &e) == 0 && app_is_visible(&e)) {
			DesktopApp *app = entry_to_app(&e, path);

			if (app)
				app_list_push(apps, app);
		}
		entry_free(&e);
	}
	closedir(d);
}

/* Scan <base>/applications, where base is the first len bytes of `base`. */
static void
scan_data_dir(const char *base, size_t len, struct seen_list *seen,
              struct app_list *apps)
{
	char path[PATH_MAX];
	int r;

	if (len > PATH_MAX)
		return;
	r = snprintf(path, sizeof path, "%.*s/applications", (int)len, base);
	if (r > 0 && (size_t)r < sizeof path)
		load_dir(path, seen, apps);
}

/* ---- public API ---------------------------------------------------- */

DesktopApp **
desktop_apps_load(void)
{
	struct app_list apps = {0};
	struct seen_list seen = {0};
	const char *data_home = getenv("XDG_DATA_HOME");
	const char *home = getenv("HOME");
	const char *data_dirs = getenv("XDG_DATA_DIRS");
	const char *p;
	char buf[PATH_MAX];
	size_t i, n;
	int r;

	/* $XDG_DATA_HOME (default ~/.local/share) wins over the system dirs */
	if (data_home && *data_home) {
		scan_data_dir(data_home, strlen(data_home), &seen, &apps);
	} else if (home && *home) {
		r = snprintf(buf, sizeof buf, "%s/.local/share", home);
		if (r > 0 && (size_t)r < sizeof buf)
			scan_data_dir(buf, (size_t)r, &seen, &apps);
	}

	/* $XDG_DATA_DIRS (default /usr/local/share:/usr/share), in order */
	if (!data_dirs || !*data_dirs)
		data_dirs = "/usr/local/share:/usr/share";
	for (p = data_dirs; *p; p += n + (p[n] != '\0')) {
		n = strcspn(p, ":");
		if (n)
			scan_data_dir(p, n, &seen, &apps);
	}

	for (i = 0; i < seen.count; i++)
		free(seen.names[i]);
	free(seen.names);

	if (!apps.v)
		apps.v = ecalloc(1, sizeof *apps.v);
	apps.v[apps.count] = NULL;
	qsort(apps.v, apps.count, sizeof *apps.v, cmp_apps);
	return apps.v;
}

void
desktop_apps_free(DesktopApp **apps)
{
	size_t i;

	if (!apps)
		return;
	for (i = 0; apps[i]; i++) {
		free(apps[i]->name);
		free(apps[i]->exec);
		desktop_argv_free(apps[i]->argv);
		free(apps[i]);
	}
	free(apps);
}

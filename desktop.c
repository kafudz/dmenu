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
	if (!(b->s = realloc(b->s, n)))
		die("realloc:");
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

/* ---- Exec field-code expansion (%f %F %u %U %i %c %k %%) ---------- */

/*
 * The result is meant for `sh -c`. The Exec= value is passed through as is
 * (shell quoting is close enough to the spec's), but text we substitute in
 * is quoted so names containing spaces or quotes stay one argument.
 */
static char *
expand_exec(const char *exec, const char *name, const char *icon, const char *path)
{
	struct sbuf b = {0};

	for (const char *p = exec; *p; p++) {
		if (*p != '%') {
			sbuf_addc(&b, *p);
			continue;
		}
		if (!*++p)
			break;
		switch (*p) {
		case '%':
			sbuf_addc(&b, '%');
			break;
		case 'i':
			if (icon && *icon) {
				sbuf_add(&b, "--icon ");
				sbuf_addquoted(&b, icon);
			}
			break;
		case 'c':
			if (name)
				sbuf_addquoted(&b, name);
			break;
		case 'k':
			if (path)
				sbuf_addquoted(&b, path);
			break;
		default:
			/* %f %F %u %U (no file or URL is supplied) and the
			 * deprecated codes expand to nothing */
			break;
		}
	}
	return sbuf_take(&b);
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

static DesktopApp *
entry_to_app(const struct entry *e, const char *desktop_path)
{
	DesktopApp *app = ecalloc(1, sizeof *app);
	char *cmd = expand_exec(e->exec, e->name, e->icon, desktop_path);
	char *c;

	app->name = estrdup(e->name);
	/* a menu item is one line: neutralise newlines and other controls */
	for (c = app->name; *c; c++)
		if ((unsigned char)*c < 0x20 || *c == 0x7f)
			*c = ' ';

	if (e->terminal) {
		/* Wrap in a terminal: $TERMINAL, falling back to xterm. */
		const char *term = getenv("TERMINAL");
		struct sbuf b = {0};

		sbuf_add(&b, term && *term ? term : "xterm");
		sbuf_add(&b, " -e ");
		sbuf_add(&b, cmd);
		free(cmd);
		app->exec = sbuf_take(&b);
	} else {
		app->exec = cmd;
	}
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
		size_t n = l->cap ? l->cap * 2 : 64;
		DesktopApp **tmp = realloc(l->v, n * sizeof *tmp);

		if (!tmp)
			die("realloc:");
		l->v = tmp;
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
		size_t n = s->cap ? s->cap * 2 : 64;
		char **tmp = realloc(s->names, n * sizeof *tmp);

		if (!tmp)
			die("realloc:");
		s->names = tmp;
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

		if (parse_desktop_file(path, &e) == 0 && app_is_visible(&e))
			app_list_push(apps, entry_to_app(&e, path));
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
		free(apps[i]);
	}
	free(apps);
}

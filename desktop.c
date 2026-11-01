/* See LICENSE file for copyright and license details. */
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>

#include "desktop.h"
#include "util.h"

#define DESKTOP_SUFFIX ".desktop"

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
	size_t want;

	if (need > SIZE_MAX - b->len - 1)
		die("string too long");
	want = b->len + need + 1;
	if (want > b->cap) {
		size_t n = b->cap ? b->cap : 64;
		while (n < want)
			n *= 2;
		char *p = realloc(b->s, n);
		if (!p)
			die("realloc:");
		b->s = p;
		b->cap = n;
	}
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
	sbuf_add(&b, "");
	for (const char *p = in; *p; ) {
		if (*p == '\\' && p[1]) {
			p++;
			switch (*p) {
			case 'n':  sbuf_addc(&b, '\n'); break;
			case 't':  sbuf_addc(&b, '\t'); break;
			case 'r':  sbuf_addc(&b, '\r'); break;
			case 's':  sbuf_addc(&b, ' ');  break;
			case '\\': sbuf_addc(&b, '\\'); break;
			case ';':  sbuf_addc(&b, ';');  break;
			default:   sbuf_addc(&b, *p);   break;
			}
			p++;
		} else {
			sbuf_addc(&b, *p++);
		}
	}
	return b.s;
}

static int
parse_bool(const char *v)
{
	return v && (!strcmp(v, "true") || !strcmp(v, "1"));
}

/* Parse one file. Return 0 on success and fill e; -1 otherwise. */
static int
parse_desktop_file(const char *path, struct entry *e)
{
	FILE *f = fopen(path, "r");
	char *line = NULL;
	size_t linesiz = 0;
	ssize_t n;
	int in_section = 0, type_ok = 0, got_exec = 0;

	if (!f)
		return -1;

	while ((n = getline(&line, &linesiz, f)) != -1) {
		char *s, *eq, *key, *val;

		while (n > 0 && (line[n-1] == '\n' || line[n-1] == '\r'))
			line[--n] = '\0';
		s = trim(line);
		if (!*s || *s == '#')
			continue;
		if (*s == '[') {
			in_section = !strcmp(s, "[Desktop Entry]");
			continue;
		}
		if (!in_section)
			continue;
		if (!(eq = strchr(s, '=')))
			continue;
		*eq = '\0';
		key = trim(s);
		val = trim(eq + 1);

		if (!strcmp(key, "Type")) {
			type_ok = !strcmp(val, "Application");
		} else if (!strcmp(key, "Name")) {
			free(e->name);
			e->name = unescape(val);
		} else if (!strcmp(key, "Exec")) {
			free(e->exec);
			e->exec = unescape(val);
			got_exec = 1;
		} else if (!strcmp(key, "Icon")) {
			free(e->icon);
			e->icon = unescape(val);
		} else if (!strcmp(key, "TryExec")) {
			free(e->try_exec);
			e->try_exec = unescape(val);
		} else if (!strcmp(key, "OnlyShowIn")) {
			free(e->only_show_in);
			e->only_show_in = unescape(val);
		} else if (!strcmp(key, "NotShowIn")) {
			free(e->not_show_in);
			e->not_show_in = unescape(val);
		} else if (!strcmp(key, "NoDisplay")) {
			e->no_display = parse_bool(val);
		} else if (!strcmp(key, "Hidden")) {
			e->hidden = parse_bool(val);
		} else if (!strcmp(key, "Terminal")) {
			e->terminal = parse_bool(val);
		}
	}
	free(line);
	if (ferror(f)) {
			fclose(f);
			return -1;
	}
	fclose(f);

	if (!type_ok || !got_exec || !e->name || !*e->name)
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

static char *
expand_exec(const char *exec, const char *name, const char *icon, const char *path)
{
	struct sbuf b = {0};
	sbuf_add(&b, "");

	for (const char *p = exec; *p; ) {
		if (!*p) {
			/* trailing '%' is dropped per spec */
			break;
		switch (*p) {
		case '%':  sbuf_addc(&b, '%'); break;
		case 'f':  case 'F':  case 'u':  case 'U':
			/* no file / url supplied */
			break;
		case 'i':
			if (icon && *icon) {
				sbuf_add(&b, "--icon ");
				sbuf_add(&b, icon);
			}
			break;
		case 'c':
			if (name)
				sbuf_add(&b, name);
			break;
		case 'k':
			if (path)
				sbuf_add(&b, path);
			break;
		default:
			break;
		}
		p++;
	}
	return b.s;
}

/* ---- XDG filtering ------------------------------------------------- */

static int
in_list(const char *val, const char *desktop)
{
	size_t dlen = strlen(desktop);
	for (const char *p = val; *p; ) {
		const char *end = strchr(p, ';');
		size_t len = end ? (size_t)(end - p) : strlen(p);
		if (len == dlen && !strncmp(p, desktop, dlen))
			return 1;
		if (!end)
			break;
		p = end + 1;
	}
	return 0;
}

static int
matches_current_desktop(struct entry *e, const char *cur)
{
	if (!cur || !*cur)
		return 1;
	if (e->only_show_in)
		return in_list(e->only_show_in, cur);
	if (e->not_show_in)
		return !in_list(e->not_show_in, cur);
	return 1;
}

static int
binary_exists(const char *te)
{
	if (strchr(te, '/'))
		return access(te, X_OK) == 0;

	const char *path = getenv("PATH");
	if (!path)
		return access(te, X_OK) == 0;

	char *dup = strdup(path);
	if (!dup)
		return 0;
	int found = 0;
	char *saveptr;
	for (char *dir = strtok_r(dup, ":", &saveptr); dir;
	     dir = strtok_r(NULL, ":", &saveptr)) {
		char buf[PATH_MAX];
		if ((size_t)snprintf(buf, sizeof buf, "%s/%s", dir, te) < sizeof buf
		    && access(buf, X_OK) == 0) {
			found = 1;
			break;
		}
	}
	free(dup);
	return found;
}

static int
app_is_visible(struct entry *e)
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
entry_to_app(struct entry *e, const char *desktop_path)
{
	DesktopApp *app = ecalloc(1, sizeof *app);
	app->name = strdup(e->name);
	if (!app->name)
		die("strdup:");

	char *cmd = expand_exec(e->exec, e->name, e->icon, desktop_path);
	if (e->terminal) {
		/* Wrap in a terminal: $TERMINAL, then xterm, then st. */
		const char *term = getenv("TERMINAL");
		if (!term || !*term)
			term = "xterm";
		struct sbuf b = {0};
		sbuf_add(&b, term);
		sbuf_add(&b, " -e ");
		sbuf_add(&b, cmd);
		free(cmd);
		app->exec = b.s;
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
	return strcasecmp(aa->name, bb->name);
}

/* ---- directory scanning ------------------------------------------- */

/* Dedup: keep the first .desktop file seen for a given basename. */
struct seen_list { char **names; size_t count, cap; };

static int
seen_first(struct seen_list *s, const char *base)
{
	for (size_t i = 0; i < s->count; i++)
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
	s->names[s->count++] = strdup(base);
	return 1;
}

static void
load_dir(const char *dir, struct seen_list *seen,
         DesktopApp ***apps, size_t *count, size_t *cap)
{
	DIR *d = opendir(dir);
	if (!d)
		return;

	struct dirent *de;
	while ((de = readdir(d))) {
		size_t nl = strlen(de->d_name);
		if (nl < sizeof DESKTOP_SUFFIX - 1 ||
		    strcmp(de->d_name + nl - (sizeof DESKTOP_SUFFIX - 1),
		           DESKTOP_SUFFIX))
			continue;
		if (!seen_first(seen, de->d_name))
			continue;

		char path[PATH_MAX];
		if ((size_t)snprintf(path, sizeof path, "%s/%s", dir, de->d_name)
		    >= sizeof path)
			continue;

		struct entry e = {0};
		if (parse_desktop_file(path, &e) != 0) {
			entry_free(&e);
			continue;
		}
		if (!app_is_visible(&e)) {
			entry_free(&e);
			continue;
		}

		DesktopApp *app = entry_to_app(&e, path);
		entry_free(&e);

		if (*count + 2 > *cap) {
			size_t n = *cap ? *cap * 2 : 64;
			DesktopApp **tmp = realloc(*apps, n * sizeof *tmp);
			if (!tmp)
				die("realloc:");
			*apps = tmp;
			*cap = n;
		}
		(*apps)[(*count)++] = app;
	}
	closedir(d);
}

/* ---- public API ---------------------------------------------------- */

DesktopApp **
desktop_apps_load(void)
{
	DesktopApp **apps = NULL;
	size_t count = 0, cap = 0;
	struct seen_list seen = {0};

	/* XDG_DATA_HOME/applications (default ~/.local/share/applications) */
	const char *data_home = getenv("XDG_DATA_HOME");
	char *home_buf = NULL;
	if (!data_home || !*data_home) {
		const char *home = getenv("HOME");
		if (home) {
			size_t l = strlen(home) + 32;
			home_buf = ecalloc(l, 1);
			snprintf(home_buf, l, "%s/.local/share", home);
			data_home = home_buf;
		}
	}
	if (data_home && *data_home) {
		char path[PATH_MAX];
		snprintf(path, sizeof path, "%s/applications", data_home);
		load_dir(path, &seen, &apps, &count, &cap);
	}
	free(home_buf);

	/* XDG_DATA_DIRS/applications (default /usr/local/share:/usr/share) */
	const char *data_dirs = getenv("XDG_DATA_DIRS");
	if (!data_dirs || !*data_dirs)
		data_dirs = "/usr/local/share:/usr/share";
	char *dup = strdup(data_dirs);
	if (dup) {
		char *saveptr;
		for (char *dir = strtok_r(dup, ":", &saveptr); dir;
		     dir = strtok_r(NULL, ":", &saveptr)) {
			char path[PATH_MAX];
			snprintf(path, sizeof path, "%s/applications", dir);
			load_dir(path, &seen, &apps, &count, &cap);
		}
		free(dup);
	}

	for (size_t i = 0; i < seen.count; i++)
		free(seen.names[i]);
	free(seen.names);

	if (!apps)
		return NULL;

	if (count + 1 > cap) {
		DesktopApp **tmp = realloc(apps, (count + 1) * sizeof *tmp);
		if (tmp)
			apps = tmp;
	}
	apps[count] = NULL;
	qsort(apps, count, sizeof *apps, cmp_apps);
	return apps;
}

void
desktop_apps_free(DesktopApp **apps)
{
	if (!apps)
		return;
	for (size_t i = 0; apps[i]; i++) {
		free(apps[i]->name);
		free(apps[i]->exec);
		free(apps[i]);
	}
	free(apps);
}
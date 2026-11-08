/* See LICENSE file for copyright and license details. */
#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <locale.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#include <X11/Xlib.h>
#include <X11/Xatom.h>
#include <X11/Xutil.h>
#ifdef XINERAMA
#include <X11/extensions/Xinerama.h>
#endif
#include <X11/Xft/Xft.h>

#include "drw.h"
#include "desktop.h"
#include "util.h"

/* macros */
#define INTERSECT(x,y,w,h,r)  (MAX(0, MIN((x)+(w),(r).x_org+(r).width)  - MAX((x),(r).x_org)) \
                             * MAX(0, MIN((y)+(h),(r).y_org+(r).height) - MAX((y),(r).y_org)))
#define TEXTW(X)              ((int)drw_fontset_getwidth(drw, (X)) + lrpad) /* signed */

/* enums */
enum { SchemeNorm, SchemeSel, SchemeOut, SchemeLast }; /* color schemes */

struct item {
	char *text;
	char *cmd;    /* desktop mode: command line to display/print */
	char **argv;  /* desktop mode: what to run, with execvp() */
	struct item *left, *right;
	int out;
};

static char text[BUFSIZ] = "";
static char *embed;
static int bh, ih, mw, mh; /* ih: height of one item of a vertical list */
static int inputw = 0, promptw;
static int lrpad; /* sum of left and right padding */
static size_t cursor;
static struct item *items = NULL;
static struct item *matches, *matchend;
static struct item *prev, *curr, *next, *sel;
static int mon = -1, screen;
static int ptrmon = 0;                 /* -P: use the monitor under the pointer */
static int xoff = 0, yoff = 0;         /* -x, -y: offsets from the monitor edge */
static unsigned int menuw = 0;         /* -z: menu width, 0 means full width */
static unsigned int bw = 0;            /* -bw: border width in pixels */
static const int scrollstep = 1;       /* items per mouse wheel notch in a vertical list */
static unsigned int itempad = 0;       /* -ip: extra height per item of a vertical list */
static unsigned int promptpad = 0;     /* -pp: extra height of the prompt and input row */
static int clickaway = 0;              /* -C: a click outside the menu closes it */
static int seplines = 0;               /* -L: draw lines between the items of a vertical list */
static unsigned int opacity = 100;     /* -op: window opacity in percent (needs a compositor) */
/* -lc, -bc: color of the lines between items and of the border. These live
 * here rather than in config.h so that an existing config.h keeps working.
 * A NULL border color means the background color of the selected item. */
static const char *linecolors[2] = { "#444444", NULL };
static Clr *linescheme;

static Atom clip, utf8;
static Display *dpy;
static Window root, parentwin, win;
static XIC xic;

static Drw *drw;
static Clr *scheme[SchemeLast];

#include "config.h"

static int (*fstrncmp)(const char *, const char *, size_t) = strncmp;
static char *(*fstrstr)(const char *, const char *) = strstr;

/* parse a non-negative int command line argument, dying on bad input */
static unsigned int
parsenum(const char *s, const char *opt)
{
	char *end;
	unsigned long v;

	errno = 0;
	v = strtoul(s, &end, 10);
	if (errno || end == s || *end || v > INT_MAX)
		die("invalid argument for %s: '%s'", opt, s);
	return (unsigned int)v;
}

/* parse a padding in pixels; capped so that the heights cannot overflow an int */
static unsigned int
parsepad(const char *s, const char *opt)
{
	unsigned int v = parsenum(s, opt);

	return MIN(v, 1000u);
}

/* parse a window id (base 0 so 0x... works), dying on bad input */
static Window
parsewinid(const char *s)
{
	char *end;
	unsigned long v;

	errno = 0;
	v = strtoul(s, &end, 0);
	if (errno || end == s || *end || v == 0)
		die("invalid window id: '%s'", s);
	return (Window)v;
}

static unsigned int
textw_clamp(const char *str, unsigned int n)
{
	unsigned int w = drw_fontset_getwidth_clamp(drw, str, n) + lrpad;
	return MIN(w, n);
}

static void
appenditem(struct item *item, struct item **list, struct item **last)
{
	if (*last)
		(*last)->right = item;
	else
		*list = item;

	item->left = *last;
	item->right = NULL;
	*last = item;
}

static void
calcoffsets(void)
{
	int i, n;

	if (lines > 0)
		n = lines * ih;
	else
		n = mw - (promptw + inputw + TEXTW("<") + TEXTW(">"));
	/* calculate which items will begin the next page and previous page */
	for (i = 0, next = curr; next; next = next->right)
		if ((i += (lines > 0) ? ih : (int)textw_clamp(next->text, (unsigned int)n)) > n)
			break;
	for (i = 0, prev = curr; prev && prev->left; prev = prev->left)
		if ((i += (lines > 0) ? ih : (int)textw_clamp(prev->left->text, (unsigned int)n)) > n)
			break;
}

static void
cleanup(void)
{
	size_t i;

	XUngrabKeyboard(dpy, CurrentTime);
	for (i = 0; i < SchemeLast; i++)
		drw_scm_free(drw, scheme[i], 2);
	drw_scm_free(drw, linescheme, 2);
	for (i = 0; items && items[i].text; ++i) {
		free(items[i].text);
		free(items[i].cmd);
		desktop_argv_free(items[i].argv);
	}
	free(items);
	drw_free(drw);
	XSync(dpy, False);
	XCloseDisplay(dpy);
}

static char *
cistrstr(const char *h, const char *n)
{
	size_t i;

	if (!n[0])
		return (char *)h;

	for (; *h; ++h) {
		for (i = 0; n[i] && tolower((unsigned char)n[i]) ==
		            tolower((unsigned char)h[i]); ++i)
			;
		if (n[i] == '\0')
			return (char *)h;
	}
	return NULL;
}

static int
drawitem(struct item *item, int x, int y, int w)
{
	if (item == sel)
		drw_setscheme(drw, scheme[SchemeSel]);
	else if (item->out)
		drw_setscheme(drw, scheme[SchemeOut]);
	else
		drw_setscheme(drw, scheme[SchemeNorm]);

	return drw_text(drw, x, y, w, ih, lrpad / 2, item->text, 0);
}

static void
drawmenu(void)
{
	int curpos;
	struct item *item;
	int x = 0, y = 0, w;

	drw_setscheme(drw, scheme[SchemeNorm]);
	drw_rect(drw, 0, 0, mw, mh, 1, 1);

	if (prompt && *prompt) {
		drw_setscheme(drw, scheme[SchemeSel]);
		x = drw_text(drw, x, 0, promptw, bh, lrpad / 2, prompt, 0);
	}
	/* draw input field */
	w = (lines > 0 || !matches) ? mw - x : inputw;
	drw_setscheme(drw, scheme[SchemeNorm]);
	drw_text(drw, x, 0, w, bh, lrpad / 2, text, 0);

	/* TEXTW is signed, so this cannot wrap when the prompt is wider than the window */
	curpos = TEXTW(text) - TEXTW(&text[cursor]);
	if ((curpos += lrpad / 2 - 1) < w) {
		drw_setscheme(drw, scheme[SchemeNorm]);
		/* the cursor keeps the height of the text, centered in the row */
		drw_rect(drw, x + curpos, 2 + (int)promptpad / 2, 2, bh - 4 - (int)promptpad, 1, 0);
	}

	if (lines > 0) {
		/* draw vertical list */
		/* the items follow the input field, which is bh high */
		for (item = curr, y = bh; item != next; item = item->right, y += ih)
			drawitem(item, x, y, mw - x);
		if (seplines) {
			/* a line along the bottom edge of every item but the last one */
			drw_setscheme(drw, linescheme);
			for (item = curr, y = bh; item != next; item = item->right, y += ih)
				if (item->right != next)
					drw_rect(drw, x, y + ih - 1, mw - x, 1, 1, 0);
		}
	} else if (matches && curr) {
		/* draw horizontal list */
		x += inputw;
		w = TEXTW("<");
		if (curr->left) {
			drw_setscheme(drw, scheme[SchemeNorm]);
			drw_text(drw, x, 0, w, bh, lrpad / 2, "<", 0);
		}
		x += w;
		for (item = curr; item != next; item = item->right)
			x = drawitem(item, x, 0, textw_clamp(item->text, mw - x - TEXTW(">")));
		if (next) {
			w = TEXTW(">");
			drw_setscheme(drw, scheme[SchemeNorm]);
			drw_text(drw, mw - w, 0, w, bh, lrpad / 2, ">", 0);
		}
	}
	drw_map(drw, win, 0, 0, mw, mh);
}

static void
grabfocus(void)
{
	struct timespec ts = { .tv_sec = 0, .tv_nsec = 10000000  };
	Window focuswin;
	int i, revertwin;

	for (i = 0; i < 100; ++i) {
		XGetInputFocus(dpy, &focuswin, &revertwin);
		if (focuswin == win)
			return;
		XSetInputFocus(dpy, win, RevertToParent, CurrentTime);
		nanosleep(&ts, NULL);
	}
	die("cannot grab focus");
}

/* With -C, grabs the pointer so that a click anywhere outside the menu is
 * delivered to us (and closes the menu) instead of to the window under it.
 * owner_events is set, so clicks on our own window arrive there as usual. The
 * button that started us may still be held, so retry for a moment. Failing is
 * not fatal: the menu then just does not close on outside clicks. */
static void
grabpointer(void)
{
	struct timespec ts = { .tv_sec = 0, .tv_nsec = 1000000  };
	int i;

	for (i = 0; i < 1000; i++) {
		if (XGrabPointer(dpy, DefaultRootWindow(dpy), True, ButtonPressMask,
		                 GrabModeAsync, GrabModeAsync, None, None,
		                 CurrentTime) == GrabSuccess)
			return;
		nanosleep(&ts, NULL);
	}
	fputs("warning: cannot grab pointer\n", stderr);
}

static void
grabkeyboard(void)
{
	struct timespec ts = { .tv_sec = 0, .tv_nsec = 1000000  };
	int i;

	if (embed)
		return;
	/* try to grab keyboard, we may have to wait for another process to ungrab */
	for (i = 0; i < 1000; i++) {
		if (XGrabKeyboard(dpy, DefaultRootWindow(dpy), True, GrabModeAsync,
		                  GrabModeAsync, CurrentTime) == GrabSuccess) {
			if (clickaway)
				grabpointer();
			return;
		}
		nanosleep(&ts, NULL);
	}
	die("cannot grab keyboard");
}

static void
match(void)
{
	static char **tokv = NULL;
	static int tokn = 0;

	char buf[sizeof text], *s;
	int i, tokc = 0;
	size_t len, textsize;
	struct item *item, *lprefix, *lsubstr, *prefixend, *substrend;

	strcpy(buf, text);
	/* separate input text into tokens to be matched individually */
	for (s = strtok(buf, " "); s; tokv[tokc - 1] = s, s = strtok(NULL, " "))
		if (++tokc > tokn)
			tokv = ereallocarray(tokv, (size_t)++tokn, sizeof *tokv);
	len = tokc ? strlen(tokv[0]) : 0;

	matches = lprefix = lsubstr = matchend = prefixend = substrend = NULL;
	textsize = strlen(text) + 1;
	for (item = items; item && item->text; item++) {
		for (i = 0; i < tokc; i++)
			if (!fstrstr(item->text, tokv[i]))
				break;
		if (i != tokc) /* not all tokens match */
			continue;
		/* exact matches go first, then prefixes, then substrings */
		if (!tokc || !fstrncmp(text, item->text, textsize))
			appenditem(item, &matches, &matchend);
		else if (!fstrncmp(tokv[0], item->text, len))
			appenditem(item, &lprefix, &prefixend);
		else
			appenditem(item, &lsubstr, &substrend);
	}
	if (lprefix) {
		if (matches) {
			matchend->right = lprefix;
			lprefix->left = matchend;
		} else
			matches = lprefix;
		matchend = prefixend;
	}
	if (lsubstr) {
		if (matches) {
			matchend->right = lsubstr;
			lsubstr->left = matchend;
		} else
			matches = lsubstr;
		matchend = substrend;
	}
	curr = sel = matches;
	calcoffsets();
}

/*
 * Insert n bytes of str at the cursor, or delete -n bytes before the cursor
 * when n is negative (str is then ignored).
 */
static void
insert(const char *str, ssize_t n)
{
	size_t len = strlen(text);

	/* only growth can overflow the buffer; deletion is always safe */
	if (n > 0 && len + (size_t)n > sizeof text - 1)
		return;

	if (n < 0) {
		size_t del = (size_t)(-n);
		if (del > cursor)
			del = cursor;
		memmove(&text[cursor - del], &text[cursor], len - cursor + 1);
		cursor -= del;
	} else if (n > 0) {
		memmove(&text[cursor + (size_t)n], &text[cursor], len - cursor + 1);
		memcpy(&text[cursor], str, (size_t)n);
		cursor += (size_t)n;
	}
	match();
}

/* delete everything between position `to` and the cursor (to <= cursor) */
static void
deleteback(size_t to)
{
	insert(NULL, -(ssize_t)(cursor - to));
}

static size_t
nextrune(int inc)
{
	ssize_t n;

	/* return location of next utf8 rune in the given direction (+1 or -1) */
	for (n = (ssize_t)cursor + inc; n + inc >= 0 && (text[n] & 0xc0) == 0x80; n += inc)
		;
	return (size_t)n;
}

static void
movewordedge(int dir)
{
	if (dir < 0) { /* move cursor to the start of the word*/
		while (cursor > 0 && strchr(worddelimiters, text[nextrune(-1)]))
			cursor = nextrune(-1);
		while (cursor > 0 && !strchr(worddelimiters, text[nextrune(-1)]))
			cursor = nextrune(-1);
	} else { /* move cursor to the end of the word */
		while (text[cursor] && strchr(worddelimiters, text[cursor]))
			cursor = nextrune(+1);
		while (text[cursor] && !strchr(worddelimiters, text[cursor]))
			cursor = nextrune(+1);
	}
}

/* Is s a leading NAME=value word, as in `FOO=1 app`? */
static int
isenvassign(const char *s)
{
	size_t n = strspn(s, "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789_");

	return n && s[n] == '=' && !isdigit((unsigned char)*s);
}

/*
 * Run argv in its own session so it outlives dmenu, which is about to exit.
 * No shell is involved: the arguments come from a .desktop file and must not
 * be interpreted.
 */
static void
launch(char **argv)
{
	pid_t pid = fork();

	if (pid < 0)
		die("fork:");
	if (pid == 0) {
		close(ConnectionNumber(dpy));
		setsid();
		/* leading NAME=value words set the environment, like a shell */
		for (; *argv && isenvassign(*argv); argv++)
			putenv(*argv);
		if (*argv) {
			execvp(*argv, argv);
			perror(*argv);
		}
		_exit(127);
	}
}

/* Confirms the current selection, like pressing Enter. state holds the
 * modifiers: in desktop mode Enter launches the selected app and Shift+Enter
 * prints its Exec line instead (useful for scripting); otherwise the item (or
 * the typed text) is printed. With Ctrl the menu stays open and the item is
 * marked, so several items can be chosen. */
static void
accept(unsigned int state)
{
	if (sel && sel->cmd && !(state & ControlMask)) {
		if (state & ShiftMask) {
			puts(sel->cmd);
			if (fflush(stdout) == EOF)
				die("write error:");
		} else {
			launch(sel->argv);
		}
		cleanup();
		exit(0);
	}
	puts((sel && !(state & ShiftMask)) ? sel->text : text);
	if (fflush(stdout) == EOF)
		die("write error:");
	if (!(state & ControlMask)) {
		cleanup();
		exit(0);
	}
	if (sel)
		sel->out = 1;
}

/* Returns the visible item at the given position in the window, or NULL. This
 * mirrors the layout used by drawmenu(). */
static struct item *
itemat(int px, int py)
{
	struct item *item;
	int x, w, row;

	if (lines > 0) {
		/* vertical list: row 0 is the input field, the items follow */
		if (py < bh || px < promptw)
			return NULL;
		for (item = curr, row = bh; item != next; item = item->right, row += ih)
			if (py < row + ih)
				return item;
		return NULL;
	}
	/* horizontal list: after the prompt, the input field and the "<" */
	x = promptw + inputw + TEXTW("<");
	if (py < 0 || py >= bh || !matches || !curr)
		return NULL;
	for (item = curr; item != next; item = item->right) {
		w = (int)textw_clamp(item->text, mw - x - TEXTW(">"));
		if (px >= x && px < x + w)
			return item;
		x += w;
	}
	return NULL;
}

/* Mouse click: the left button chooses the item under the pointer, exactly as
 * if it had been selected and Enter pressed (Shift and Ctrl work as they do
 * for Enter). Clicks on anything else are ignored. */
static void
click(XButtonEvent *ev)
{
	struct item *item = itemat(ev->x, ev->y);

	if (!item)
		return;
	sel = item;
	accept(ev->state & (ShiftMask | ControlMask));
	drawmenu();
}

/* Moves the selection one item up (dir < 0) or down (dir > 0), scrolling the
 * list if the selection leaves the visible part of it. */
static void
moveselection(int dir)
{
	if (dir < 0) {
		if (sel && sel->left && (sel = sel->left)->right == curr) {
			curr = prev;
			calcoffsets();
		}
	} else if (sel && sel->right && (sel = sel->right) == next) {
		curr = next;
		calcoffsets();
	}
}

/* Mouse wheel: scrolls the list. Buttons 4 and 5 are the vertical wheel, 6
 * and 7 horizontal scrolling. */
static void
scroll(XButtonEvent *ev)
{
	int dir, n;

	switch (ev->button) {
	case Button4:
	case 6:
		dir = -1;
		break;
	case Button5:
	case 7:
		dir = +1;
		break;
	default:
		return;
	}
	for (n = lines > 0 ? scrollstep : 1; n > 0; n--)
		moveselection(dir);
	drawmenu();
}

static void
keypress(XKeyEvent *ev)
{
	char buf[64];
	int len;
	KeySym ksym = NoSymbol;
	Status status;

	len = XmbLookupString(xic, ev, buf, sizeof buf, &ksym, &status);
	switch (status) {
	default: /* XLookupNone, XBufferOverflow */
		return;
	case XLookupChars: /* composed string from input method */
		goto insert;
	case XLookupKeySym:
	case XLookupBoth: /* a KeySym and a string are returned: use keysym */
		break;
	}

	if (ev->state & ControlMask) {
		switch(ksym) {
		case XK_a: ksym = XK_Home;      break;
		case XK_b: ksym = XK_Left;      break;
		case XK_c: ksym = XK_Escape;    break;
		case XK_d: ksym = XK_Delete;    break;
		case XK_e: ksym = XK_End;       break;
		case XK_f: ksym = XK_Right;     break;
		case XK_g: ksym = XK_Escape;    break;
		case XK_h: ksym = XK_BackSpace; break;
		case XK_i: ksym = XK_Tab;       break;
		case XK_j: /* fallthrough */
		case XK_J: /* fallthrough */
		case XK_m: /* fallthrough */
		case XK_M: ksym = XK_Return; ev->state &= ~ControlMask; break;
		case XK_n: ksym = XK_Down;      break;
		case XK_p: ksym = XK_Up;        break;

		case XK_k: /* delete right */
			text[cursor] = '\0';
			match();
			break;
		case XK_u: /* delete left */
			deleteback(0);
			break;
		case XK_w: /* delete word */
			while (cursor > 0 && strchr(worddelimiters, text[nextrune(-1)]))
				deleteback(nextrune(-1));
			while (cursor > 0 && !strchr(worddelimiters, text[nextrune(-1)]))
				deleteback(nextrune(-1));
			break;
		case XK_y: /* paste selection */
		case XK_Y:
			XConvertSelection(dpy, (ev->state & ShiftMask) ? clip : XA_PRIMARY,
			                  utf8, utf8, win, CurrentTime);
			return;
		case XK_Left:
		case XK_KP_Left:
			movewordedge(-1);
			goto draw;
		case XK_Right:
		case XK_KP_Right:
			movewordedge(+1);
			goto draw;
		case XK_Return:
		case XK_KP_Enter:
			break;
		case XK_bracketleft:
			cleanup();
			exit(1);
		default:
			return;
		}
	} else if (ev->state & Mod1Mask) {
		switch(ksym) {
		case XK_b:
			movewordedge(-1);
			goto draw;
		case XK_f:
			movewordedge(+1);
			goto draw;
		case XK_g: ksym = XK_Home;  break;
		case XK_G: ksym = XK_End;   break;
		case XK_h: ksym = XK_Up;    break;
		case XK_j: ksym = XK_Next;  break;
		case XK_k: ksym = XK_Prior; break;
		case XK_l: ksym = XK_Down;  break;
		default:
			return;
		}
	}

	switch(ksym) {
	default:
insert:
		if (len > 0 && !iscntrl((unsigned char)*buf))
			insert(buf, len);
		break;
	case XK_Delete:
	case XK_KP_Delete:
		if (text[cursor] == '\0')
			return;
		cursor = nextrune(+1);
		/* fallthrough */
	case XK_BackSpace:
		if (cursor == 0)
			return;
		deleteback(nextrune(-1));
		break;
	case XK_End:
	case XK_KP_End:
		if (text[cursor] != '\0') {
			cursor = strlen(text);
			break;
		}
		if (next) {
			/* jump to end of list and position items in reverse */
			curr = matchend;
			calcoffsets();
			curr = prev;
			calcoffsets();
			while (next && (curr = curr->right))
				calcoffsets();
		}
		sel = matchend;
		break;
	case XK_Escape:
		cleanup();
		exit(1);
	case XK_Home:
	case XK_KP_Home:
		if (sel == matches) {
			cursor = 0;
			break;
		}
		sel = curr = matches;
		calcoffsets();
		break;
	case XK_Left:
	case XK_KP_Left:
		if (cursor > 0 && (!sel || !sel->left || lines > 0)) {
			cursor = nextrune(-1);
			break;
		}
		if (lines > 0)
			return;
		/* fallthrough */
	case XK_Up:
	case XK_KP_Up:
		moveselection(-1);
		break;
	case XK_Next:
	case XK_KP_Next:
		if (!next)
			return;
		sel = curr = next;
		calcoffsets();
		break;
	case XK_Prior:
	case XK_KP_Prior:
		if (!prev)
			return;
		sel = curr = prev;
		calcoffsets();
		break;
	case XK_Return:
	case XK_KP_Enter:
		accept(ev->state);
		break;
	case XK_Right:
	case XK_KP_Right:
		if (text[cursor] != '\0') {
			cursor = nextrune(+1);
			break;
		}
		if (lines > 0)
			return;
		/* fallthrough */
	case XK_Down:
	case XK_KP_Down:
		moveselection(+1);
		break;
	case XK_Tab:
		if (!sel)
			return;
		cursor = strnlen(sel->text, sizeof text - 1);
		memcpy(text, sel->text, cursor);
		text[cursor] = '\0';
		match();
		break;
	}

draw:
	drawmenu();
}

static void
paste(void)
{
	unsigned char *p, *q;
	int di;
	unsigned long dl;
	Atom da;

	/* we have been given the current selection, now insert it into input */
	if (XGetWindowProperty(dpy, win, utf8, 0, (sizeof text / 4) + 1, False,
	                   utf8, &da, &di, &dl, &dl, &p)
	    == Success && p) {
		/* the selection owner is another client: only accept 8-bit
		 * data, anything else is not text. Only the first line is used. */
		if (di == 8)
			insert((char *)p, (q = (unsigned char *)strchr((char *)p, '\n'))
			                  ? (ssize_t)(q - p) : (ssize_t)strlen((char *)p));
		XFree(p);
	}
	drawmenu();
}

static void
readstdin(void)
{
	char *line = NULL;
	size_t i, itemsiz = 0, linesiz = 0;
	ssize_t len;

	/* read each line from stdin and add it to the item list */
	for (i = 0; (len = getline(&line, &linesiz, stdin)) != -1; i++) {
		if (i + 1 >= itemsiz) {
			/* check before adding so the size_t cannot wrap;
			 * ereallocarray() checks the multiplication */
			if (itemsiz > SIZE_MAX - 256)
				die("too many items");
			itemsiz += 256;
			items = ereallocarray(items, itemsiz, sizeof *items);
		}
		if (len > 0 && line[len - 1] == '\n')
			line[len - 1] = '\0';
		items[i].text = estrdup(line);
		items[i].out = 0;
		items[i].cmd = NULL;
		items[i].argv = NULL;
	}
	free(line);
	/* getline() also returns -1 on a read error, not just EOF */
	if (ferror(stdin))
		die("failed to read stdin:");
	if (items) {
		items[i].text = NULL;
		items[i].cmd  = NULL;
		items[i].argv = NULL;
	}
	lines = (unsigned int)MIN((size_t)lines, i);
}

static void
load_desktop(void)
{
	DesktopApp **apps = desktop_apps_load();
	size_t i, count = 0;

	while (apps[count])
		count++;

	/* ecalloc zeroes everything, including the terminating sentinel */
	items = ecalloc(count + 1, sizeof *items);
	for (i = 0; i < count; i++) {
		/* take over the strings instead of copying them;
		 * desktop_apps_free() skips the NULLs left behind */
		items[i].text = apps[i]->name;
		items[i].cmd  = apps[i]->exec;
		items[i].argv = apps[i]->argv;
		apps[i]->name = apps[i]->exec = NULL;
		apps[i]->argv = NULL;
	}
	lines = (unsigned int)MIN((size_t)lines, count);
	desktop_apps_free(apps);
}

static void
run(void)
{
	XEvent ev;

	while (!XNextEvent(dpy, &ev)) {
		if (XFilterEvent(&ev, win))
			continue;
		switch(ev.type) {
		case DestroyNotify:
			if (ev.xdestroywindow.window != win)
				break;
			cleanup();
			exit(1);
		case Expose:
			if (ev.xexpose.count == 0)
				drw_map(drw, win, 0, 0, mw, mh);
			break;
		case FocusIn:
			/* regrab focus from parent window */
			if (ev.xfocus.window != win)
				grabfocus();
			break;
		case KeyPress:
			keypress(&ev.xkey);
			break;
		case ButtonPress:
			if (ev.xbutton.window != win) {
				/* only seen with -C: a click outside the menu (the wheel
				 * doesn't count) */
				if (ev.xbutton.button < Button4 || ev.xbutton.button > 7) {
					cleanup();
					exit(1);
				}
				break;
			}
			if (ev.xbutton.button == Button1)
				click(&ev.xbutton);
			else
				scroll(&ev.xbutton);
			break;
		case SelectionNotify:
			if (ev.xselection.property == utf8)
				paste();
			break;
		case VisibilityNotify:
			if (ev.xvisibility.state != VisibilityUnobscured)
				XRaiseWindow(dpy, win);
			break;
		}
	}
}

static void
setup(void)
{
	int x, y, i, j;
	unsigned int du;
	XSetWindowAttributes swa;
	XIM xim;
	Window w, dw, *dws;
	XWindowAttributes wa;
	XClassHint ch = {"dmenu", "dmenu"};
#ifdef XINERAMA
	XineramaScreenInfo *info;
	Window pw;
	int a, di, n, area = 0;
#endif
	/* init appearance */
	for (j = 0; j < SchemeLast; j++)
		scheme[j] = drw_scm_create(drw, colors[j], 2);
	if (!linecolors[1])
		linecolors[1] = colors[SchemeSel][ColBg];
	linescheme = drw_scm_create(drw, linecolors, 2);

	clip = XInternAtom(dpy, "CLIPBOARD",   False);
	utf8 = XInternAtom(dpy, "UTF8_STRING", False);

	/* calculate menu geometry */
	bh = drw->fonts->h + 2;
	/* bh: the prompt and input row, which also holds the items of a
	 * horizontal list; ih: one item of a vertical list */
	bh += (int)promptpad;
	ih = lines > 0 ? bh - (int)promptpad + (int)itempad : bh;
	mh = bh + (int)lines * ih;
#ifdef XINERAMA
	i = 0;
	if (parentwin == root && (info = XineramaQueryScreens(dpy, &n))) {
		if (n < 1) {
			XFree(info);
			die("Xinerama reported no screens");
		}
		XGetInputFocus(dpy, &w, &di);
		if (mon >= 0 && mon < n)
			i = mon;
		else if (!ptrmon && w != root && w != PointerRoot && w != None) {
			/* find top-level window containing current input focus */
			do {
				if (XQueryTree(dpy, (pw = w), &dw, &w, &dws, &du) && dws)
					XFree(dws);
			} while (w != root && w != pw);
			/* find xinerama screen with which the window intersects most */
			if (XGetWindowAttributes(dpy, pw, &wa))
				for (j = 0; j < n; j++)
					if ((a = INTERSECT(wa.x, wa.y, wa.width, wa.height, info[j])) > area) {
						area = a;
						i = j;
					}
		}
		/* no focused window is on screen, so use pointer location instead */
		if (mon < 0 && !area && XQueryPointer(dpy, root, &dw, &dw, &x, &y, &di, &di, &du))
			for (i = 0; i < n; i++)
				if (INTERSECT(x, y, 1, 1, info[i]) != 0)
					break;
		/* fallback to the first screen if there is no intersection */
		if (i >= n)
			i = 0;

		x = info[i].x_org + MIN(xoff, MAX(info[i].width - 1, 0));
		y = info[i].y_org + (topbar ? yoff : info[i].height - mh - 2 * (int)bw - yoff);
		mw = info[i].width - (x - info[i].x_org);
		if (menuw > 0)
			mw = MIN(mw, (int)menuw);
		mw = MAX(mw - 2 * (int)bw, 1); /* the border is part of the width */
		XFree(info);
	} else
#endif
	{
		if (!XGetWindowAttributes(dpy, parentwin, &wa))
			die("could not get embedding window attributes: 0x%lx",
			    parentwin);
		x = MIN(xoff, MAX(wa.width - 1, 0));
		y = topbar ? yoff : wa.height - mh - 2 * (int)bw - yoff;
		mw = wa.width - x;
		if (menuw > 0)
			mw = MIN(mw, (int)menuw);
		mw = MAX(mw - 2 * (int)bw, 1); /* the border is part of the width */
	}
	promptw = (prompt && *prompt) ? TEXTW(prompt) - lrpad / 4 : 0;
	inputw = mw / 3; /* input width: ~33% of monitor width */
	match();

	/* create menu window */
	swa.override_redirect = True;
	swa.background_pixel = scheme[SchemeNorm][ColBg].pixel;
	swa.border_pixel = linescheme[ColBg].pixel;
	swa.event_mask = ExposureMask | KeyPressMask | ButtonPressMask | VisibilityChangeMask;
	win = XCreateWindow(dpy, root, x, y, mw, mh, bw,
	                    CopyFromParent, CopyFromParent, CopyFromParent,
	                    CWOverrideRedirect | CWBackPixel | CWBorderPixel | CWEventMask, &swa);
	XSetClassHint(dpy, win, &ch);

	/* Opacity is handled by the compositor, which reads this property
	 * (the same one used by e.g. picom and xcompmgr) when the window is
	 * mapped. Without a compositor it has no effect. */
	if (opacity < 100) {
		unsigned long value = (unsigned long)(0xffffffffUL / 100 * opacity);
		XChangeProperty(dpy, win, XInternAtom(dpy, "_NET_WM_WINDOW_OPACITY", False),
		                XA_CARDINAL, 32, PropModeReplace, (unsigned char *)&value, 1);
	}

	/* input methods */
	if ((xim = XOpenIM(dpy, NULL, NULL, NULL)) == NULL)
		die("XOpenIM failed: could not open input device");

	if ((xic = XCreateIC(xim, XNInputStyle, XIMPreeditNothing | XIMStatusNothing,
	                XNClientWindow, win, XNFocusWindow, win, NULL)) == NULL)
		die("XCreateIC failed: could not obtain input context");

	XMapRaised(dpy, win);
	if (embed) {
		XReparentWindow(dpy, win, parentwin, x, y);
		XSelectInput(dpy, parentwin, FocusChangeMask | SubstructureNotifyMask);
		if (XQueryTree(dpy, parentwin, &dw, &w, &dws, &du) && dws) {
			for (i = 0; i < (int)du && dws[i] != win; ++i)
				XSelectInput(dpy, dws[i], FocusChangeMask);
			XFree(dws);
		}
		grabfocus();
	}
	drw_resize(drw, mw, mh);
	drawmenu();
}

static void
usage(void)
{
	fprintf(stderr,
	    "usage: dmenu [-AbCifLPv] [-l lines] [-p prompt] [-fn font] [-m monitor]\n"
	    "             [-x xoffset] [-y yoffset] [-z width] [-bw borderwidth]\n"
	    "             [-bc color] [-lc color] [-op opacity] [-ip padding]\n"
	    "             [-pp padding]\n"
	    "             [-nb color] [-nf color] [-sb color] [-sf color]\n"
	    "             [-ob color] [-of color] [-w windowid]\n");
	exit(2);
}

int
main(int argc, char *argv[])
{
	XWindowAttributes wa;
	int i, fast = 0, desktop = 0;

	for (i = 1; i < argc; i++)
		/* these options take no arguments */
		if (!strcmp(argv[i], "-v")) {      /* prints version information */
			puts("dmenu-"VERSION);
			exit(0);
		} else if (!strcmp(argv[i], "-b")) /* appears at the bottom of the screen */
			topbar = 0;
		else if (!strcmp(argv[i], "-f"))   /* grabs keyboard before reading stdin */
			fast = 1;
		else if (!strcmp(argv[i], "-A"))   /* desktop application mode */
			desktop = 1;
		else if (!strcmp(argv[i], "-P"))   /* use the monitor under the pointer */
			ptrmon = 1;
		else if (!strcmp(argv[i], "-L"))   /* lines between items in a vertical list */
			seplines = 1;
		else if (!strcmp(argv[i], "-C"))   /* a click outside the menu closes it */
			clickaway = 1;
		else if (!strcmp(argv[i], "-i")) { /* case-insensitive item matching */
			fstrncmp = strncasecmp;
			fstrstr = cistrstr;
		} else if (i + 1 == argc)
			usage();
		/* these options take one argument */
		else if (!strcmp(argv[i], "-l"))   /* number of lines in vertical list */
			lines = parsenum(argv[++i], "-l");
		else if (!strcmp(argv[i], "-m"))   /* monitor index */
			mon = (int)parsenum(argv[++i], "-m");
		else if (!strcmp(argv[i], "-x"))   /* horizontal offset from the monitor edge */
			xoff = (int)parsenum(argv[++i], "-x");
		else if (!strcmp(argv[i], "-y"))   /* vertical offset from the top/bottom edge */
			yoff = (int)parsenum(argv[++i], "-y");
		else if (!strcmp(argv[i], "-z"))   /* menu width in pixels */
			menuw = parsenum(argv[++i], "-z");
		else if (!strcmp(argv[i], "-op")) { /* opacity in percent */
			/* parse once: MIN() would evaluate the ++i expression twice */
			opacity = parsenum(argv[++i], "-op");
			if (opacity > 100)
				opacity = 100;
		} else if (!strcmp(argv[i], "-ip")) { /* extra height per item in pixels */
			itempad = parsepad(argv[++i], "-ip");
		} else if (!strcmp(argv[i], "-pp")) { /* extra height of the prompt row in pixels */
			promptpad = parsepad(argv[++i], "-pp");
		} else if (!strcmp(argv[i], "-bw"))  /* border width in pixels */
			bw = parsenum(argv[++i], "-bw");
		else if (!strcmp(argv[i], "-bc"))  /* border color */
			linecolors[1] = argv[++i];
		else if (!strcmp(argv[i], "-lc"))  /* color of the lines between items */
			linecolors[0] = argv[++i];
		else if (!strcmp(argv[i], "-p"))   /* adds prompt to left of input field */
			prompt = argv[++i];
		else if (!strcmp(argv[i], "-fn"))  /* font or font set */
			fonts[0] = argv[++i];
		else if (!strcmp(argv[i], "-nb"))  /* normal background color */
			colors[SchemeNorm][ColBg] = argv[++i];
		else if (!strcmp(argv[i], "-nf"))  /* normal foreground color */
			colors[SchemeNorm][ColFg] = argv[++i];
		else if (!strcmp(argv[i], "-sb"))  /* selected background color */
			colors[SchemeSel][ColBg] = argv[++i];
		else if (!strcmp(argv[i], "-sf"))  /* selected foreground color */
			colors[SchemeSel][ColFg] = argv[++i];
		else if (!strcmp(argv[i], "-ob"))  /* outline background color */
			colors[SchemeOut][ColBg] = argv[++i];
		else if (!strcmp(argv[i], "-of"))  /* outline foreground color */
			colors[SchemeOut][ColFg] = argv[++i];
		else if (!strcmp(argv[i], "-w"))   /* embedding window id */
			embed = argv[++i];
		else
			usage();

	if (!setlocale(LC_CTYPE, "") || !XSupportsLocale())
		fputs("warning: no locale support\n", stderr);
	if (!(dpy = XOpenDisplay(NULL)))
		die("cannot open display");
	screen = DefaultScreen(dpy);
	root = RootWindow(dpy, screen);
	parentwin = embed ? parsewinid(embed) : root;
	if (!XGetWindowAttributes(dpy, parentwin, &wa))
		die("could not get embedding window attributes: 0x%lx",
		    parentwin);
	drw = drw_create(dpy, screen, root, wa.width, wa.height);
	if (!drw_fontset_create(drw, fonts, LENGTH(fonts)))
		die("no fonts could be loaded.");
	lrpad = drw->fonts->h;

#ifdef __OpenBSD__
	/* desktop mode also forks and execs the chosen application */
	if (pledge(desktop ? "stdio rpath proc exec" : "stdio rpath", NULL) == -1)
		die("pledge");
#endif

	if (desktop) {
		grabkeyboard();
		load_desktop();
	} else if (fast && !isatty(0)) {
		grabkeyboard();
		readstdin();
	} else {
		readstdin();
		grabkeyboard();
	}
	setup();
	run();

	return 1; /* unreachable */
}

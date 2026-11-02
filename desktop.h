/* See LICENSE file for copyright and license details. */
#ifndef DESKTOP_H
#define DESKTOP_H

typedef struct {
	char  *name;  /* friendly Name= to display (single line) */
	char **argv;  /* NULL-terminated command, field codes expanded; run it
	               * with execvp(), never through a shell */
	char  *exec;  /* argv joined into one shell-safe line, for display and
	               * for printing; it is not meant to be run with sh -c */
} DesktopApp;

/* Load all visible desktop applications from the XDG data dirs.
 * Returns a NULL-terminated array sorted by name (case-insensitively); it is
 * empty when nothing was found. Exits via die() on allocation failure. */
DesktopApp **desktop_apps_load(void);

/* Free an array returned by desktop_apps_load(), and everything it owns.
 * Entries whose strings were taken over by the caller must have those
 * pointers set to NULL first. */
void desktop_apps_free(DesktopApp **apps);

/* Free a NULL-terminated argument vector (DesktopApp.argv). */
void desktop_argv_free(char **argv);

#endif

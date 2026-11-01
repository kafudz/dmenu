/* See LICENSE file for copyright and license details. */
#ifndef DESKTOP_H
#define DESKTOP_H

typedef struct {
	char *name;  /* friendly Name= to display */
	char *exec;  /* Exec= line, field codes expanded */
} DesktopApp;

/* Load all visible desktop applications from XDG data dirs.
 * Returns a NULL-terminated, name-sorted array, or NULL on total failure. */
DesktopApp **desktop_apps_load(void);
void desktop_apps_free(DesktopApp **apps);

#endif
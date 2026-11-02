/* Test helper: print every app desktop_apps_load() finds as "name<TAB>exec". */
#include <stdio.h>

#include "../desktop.h"

int
main(void)
{
	DesktopApp **apps = desktop_apps_load();
	size_t i;

	for (i = 0; apps[i]; i++)
		printf("%s\t%s\n", apps[i]->name, apps[i]->exec);
	desktop_apps_free(apps);
	return 0;
}

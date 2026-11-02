/* Test helper: print every app desktop_apps_load() finds as
 * "name<TAB>exec<TAB>[arg0][arg1]...". */
#include <stdio.h>

#include "../desktop.h"

int
main(void)
{
	DesktopApp **apps = desktop_apps_load();
	size_t i, j;

	for (i = 0; apps[i]; i++) {
		printf("%s\t%s\t", apps[i]->name, apps[i]->exec);
		for (j = 0; apps[i]->argv[j]; j++)
			printf("[%s]", apps[i]->argv[j]);
		putchar('\n');
	}
	desktop_apps_free(apps);
	return 0;
}

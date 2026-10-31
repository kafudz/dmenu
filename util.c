/* See LICENSE file for copyright and license details. */
#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "util.h"

void
die(const char *fmt, ...)
{
	va_list ap;
	int saved_errno = errno; /* save first: stdio calls below may change it */

	va_start(ap, fmt);
	vfprintf(stderr, fmt, ap);
	va_end(ap);

	if (fmt[0] && fmt[strlen(fmt) - 1] == ':')
		fprintf(stderr, " %s", strerror(saved_errno));
	fputc('\n', stderr);
	exit(1);
}

void *
ecalloc(size_t nmemb, size_t size)
{
	void *p = calloc(nmemb, size);

	/* a NULL result is legitimate for a zero-sized request */
	if (!p && nmemb && size)
		die("calloc:");
	return p;
}

void *
erealloc(void *p, size_t size)
{
	void *q;

	/* realloc(p, 0) may free p and return NULL, so ask for one byte */
	if (!(q = realloc(p, size ? size : 1)))
		die("realloc:");
	return q;
}

char *
estrdup(const char *s)
{
	char *p;

	if (!(p = strdup(s)))
		die("strdup:");
	return p;
}

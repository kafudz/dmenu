/* See LICENSE file for copyright and license details. */
#ifndef UTIL_H
#define UTIL_H

#include <stddef.h>

#define MAX(A, B)               ((A) > (B) ? (A) : (B))
#define MIN(A, B)               ((A) < (B) ? (A) : (B))
#define BETWEEN(X, A, B)        ((A) <= (X) && (X) <= (B))
#define LENGTH(X)               (sizeof (X) / sizeof (X)[0])

#if defined(__GNUC__) || defined(__clang__)
#define DIE_ATTR __attribute__((noreturn, format(printf, 1, 2)))
#else
#define DIE_ATTR
#endif

/* Print a printf-style message to stderr and exit(1). A trailing ':' in fmt
 * appends strerror(errno), like perror(3). */
void die(const char *fmt, ...) DIE_ATTR;
void *ecalloc(size_t nmemb, size_t size);
/* realloc(ptr, nmemb * size) that dies if the multiplication would overflow
 * or the allocation fails; never returns NULL for a nonzero size. */
void *ereallocarray(void *ptr, size_t nmemb, size_t size);
char *estrdup(const char *s);

#endif

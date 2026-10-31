/* See LICENSE file for copyright and license details. */
#ifndef UTIL_H
#define UTIL_H

#include <stddef.h>

/* NB: MAX, MIN and BETWEEN evaluate some of their arguments twice. */
#define MAX(A, B)               ((A) > (B) ? (A) : (B))
#define MIN(A, B)               ((A) < (B) ? (A) : (B))
#define BETWEEN(X, A, B)        ((A) <= (X) && (X) <= (B))
#define LENGTH(X)               (sizeof (X) / sizeof (X)[0])

/*
 * Let GCC/clang know that die() never returns and check its printf-style
 * arguments at compile time. Other compilers just see a plain prototype.
 */
#if defined(__GNUC__)
#define UTIL_NORETURN           __attribute__((noreturn))
#define UTIL_PRINTF(a, b)       __attribute__((format(printf, a, b)))
#else
#define UTIL_NORETURN
#define UTIL_PRINTF(a, b)
#endif

/*
 * Print a printf-style message to stderr and exit(1). If fmt ends in ':'
 * the strerror() text for the errno value at call time is appended.
 */
void die(const char *fmt, ...) UTIL_NORETURN UTIL_PRINTF(1, 2);

/* Allocation wrappers: they die() instead of returning NULL on failure. */
void *ecalloc(size_t nmemb, size_t size);
void *erealloc(void *p, size_t size);
char *estrdup(const char *s);

#endif

/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _LINUX_XALLOC_H
#define _LINUX_XALLOC_H

/*
 * Allocation wrappers that abort on out-of-memory, for host tools that would
 * only have to die anyway. Modelled on the helpers several scripts grew
 * privately before this header existed.
 */

#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static inline void die(const char *fmt, ...)
	__attribute__((noreturn, format(printf, 1, 2)));

static inline void die(const char *fmt, ...)
{
	va_list args;

	va_start(args, fmt);
	vfprintf(stderr, fmt, args);
	va_end(args);
	fputc('\n', stderr);
	exit(1);
}

static inline void *xmalloc(size_t size)
{
	void *ptr = malloc(size);

	if (!ptr && size)
		die("out of memory (allocating %zu bytes)", size);

	return ptr;
}

static inline void *xcalloc(size_t nmemb, size_t size)
{
	void *ptr = calloc(nmemb, size);

	if (!ptr && nmemb && size)
		die("out of memory (allocating %zu bytes)", nmemb * size);

	return ptr;
}

static inline void *xrealloc(void *ptr, size_t size)
{
	void *new_ptr = realloc(ptr, size);

	if (!new_ptr && size)
		die("out of memory (reallocating %zu bytes)", size);

	return new_ptr;
}

static inline char *xstrdup(const char *str)
{
	char *copy = strdup(str);

	if (!copy)
		die("out of memory (duplicating %zu bytes)", strlen(str) + 1);

	return copy;
}

#endif /* _LINUX_XALLOC_H */

#ifndef PLATFORM_STDLIB_H
#define PLATFORM_STDLIB_H
#include <stddef.h>
void *malloc(size_t);
void *calloc(size_t, size_t);
void *realloc(void *, size_t);
void free(void *);
void abort(void) __attribute__((noreturn));
int abs(int);
double strtod(const char *, char **);
size_t malloc_usable_size(void *);
long strtol(const char *, char **, int);
unsigned long strtoul(const char *, char **, int);
long long strtoll(const char *, char **, int);
void qsort(void *, size_t, size_t, int (*)(const void *, const void *));
#define alloca(size) __builtin_alloca(size)
char *getenv(const char *);
#endif

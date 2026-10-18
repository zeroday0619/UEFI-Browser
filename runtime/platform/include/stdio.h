#ifndef PLATFORM_STDIO_H
#define PLATFORM_STDIO_H
#include <stdarg.h>
#include <stddef.h>
typedef struct PlatformFile FILE;
int snprintf(char *, size_t, const char *, ...);
int vsnprintf(char *, size_t, const char *, va_list);
int sprintf(char *, const char *, ...);
int sscanf(const char *, const char *, ...);
extern FILE *stdout, *stderr;
int printf(const char *, ...);
int fprintf(FILE *, const char *, ...);
int fputc(int, FILE *);
int fputs(const char *, FILE *);
int puts(const char *);
int putchar(int);
size_t fwrite(const void *, size_t, size_t, FILE *);
#endif

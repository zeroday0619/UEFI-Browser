#ifndef PLATFORM_ASSERT_H
#define PLATFORM_ASSERT_H
#include <stdlib.h>
void platform_assert(const char *, const char *, int) __attribute__((noreturn));
#define assert(condition) ((condition) ? (void)0 : platform_assert(#condition, __FILE__, __LINE__))
#endif

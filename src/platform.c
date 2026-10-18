#include "uefi.h"
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
static SystemTable *firmware;
void platform_initialize(SystemTable *table) { firmware = table; }
static void serial_byte(unsigned char value) {
  unsigned char state;
  do {
    __asm__ volatile("inb %1,%0" : "=a"(state) : "Nd"((unsigned short)0x3fd));
  } while (!(state & 0x20));
  __asm__ volatile("outb %0,%1" ::"a"(value), "Nd"((unsigned short)0x3f8));
}
void graphics_log(const char *text) {
  while (*text) {
    if (*text == '\n')
      serial_byte('\r');
    serial_byte((unsigned char)*text++);
  }
}
void __assert_fail(const char *expression, const char *file, unsigned line,
                   const char *function) {
  (void)line;
  (void)function;
  graphics_log("ASSERT: ");
  graphics_log(file);
  graphics_log(": ");
  graphics_log(expression);
  abort();
}
static struct {
  void (*function)(void *);
  void *argument;
  void *handle;
} destructors[1024];
static size_t destructor_count;
int __cxa_atexit(void (*function)(void *), void *argument, void *handle) {
  if (destructor_count == 1024)
    return -1;
  destructors[destructor_count].function = function;
  destructors[destructor_count].argument = argument;
  destructors[destructor_count++].handle = handle;
  return 0;
}
void __cxa_finalize(void *handle) {
  for (size_t index = destructor_count; index > 0; index--) {
    if (destructors[index - 1].function &&
        (!handle || destructors[index - 1].handle == handle)) {
      void (*function)(void *) = destructors[index - 1].function;
      destructors[index - 1].function = 0;
      function(destructors[index - 1].argument);
    }
  }
}
int __cxa_thread_atexit_impl(void (*function)(void *), void *argument,
                             void *handle) {
  return __cxa_atexit(function, argument, handle);
}

void *__dso_handle;
int *__errno_location(void) {
  static int error;
  return &error;
}

char __libc_single_threaded = 1;
int isdigit(int value) { return value >= '0' && value <= '9'; }
unsigned long strtoul(const char *, char **, int);
unsigned long long __isoc23_strtoull(const char *text, char **end, int base) {
  return strtoul(text, end, base);
}

SystemTable *platform_firmware(void) { return firmware; }

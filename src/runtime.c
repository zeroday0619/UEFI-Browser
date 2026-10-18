#include "platform/runtime.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static SystemTable *firmware;
static size_t allocated;
FILE *stdout, *stderr;
int _fltused;
#define HEAP_LIMIT (48u * 1024u * 1024u)
typedef struct Allocation {
  size_t size;
  void *base;
  struct Allocation *previous, *next;
} Allocation;
static Allocation *allocations;
void runtime_initialize(SystemTable *table) { firmware = table; }
size_t runtime_memory_used(void) { return allocated; }
void *aligned_alloc(size_t alignment, size_t size) {
  if (alignment < 16)
    alignment = 16;
  if ((alignment & (alignment - 1)) || alignment > HEAP_LIMIT)
    return 0;
  if (!size)
    size = 1;
  if (size > HEAP_LIMIT || allocated > HEAP_LIMIT - size)
    return 0;
  void *base = 0;
  Status status =
      ((Status(UEFI_API *)(uint32_t, size_t, void **))firmware->boot->function[5])(
          2, size + sizeof(Allocation) + alignment - 1, &base);
  if (status)
    return 0;
  Allocation *result =
      (Allocation *)(((uintptr_t)base + sizeof(Allocation) + alignment - 1) &
                     ~(uintptr_t)(alignment - 1)) -
      1;
  result->base = base;
  result->size = size;
  result->previous = 0;
  result->next = allocations;
  if (allocations)
    allocations->previous = result;
  allocations = result;
  allocated += size;
  return result + 1;
}
void *malloc(size_t size) { return aligned_alloc(16, size); }
int posix_memalign(void **pointer, size_t alignment, size_t size) {
  if (alignment < sizeof(void *) || (alignment & (alignment - 1)))
    return 22;
  void *result = aligned_alloc(alignment, size);
  if (!result)
    return 12;
  *pointer = result;
  return 0;
}
void free(void *pointer) {
  if (!pointer)
    return;
  Allocation *allocation = (Allocation *)pointer - 1;
  if (allocation->previous)
    allocation->previous->next = allocation->next;
  else
    allocations = allocation->next;
  if (allocation->next)
    allocation->next->previous = allocation->previous;
  allocated -= allocation->size;
  ((Status(UEFI_API *)(void *))firmware->boot->function[6])(allocation->base);
}
void *calloc(size_t count, size_t size) {
  if (size && count > HEAP_LIMIT / size)
    return 0;
  void *result = malloc(count * size);
  if (result)
    memset(result, 0, count * size);
  return result;
}
void *realloc(void *pointer, size_t size) {
  if (!pointer)
    return malloc(size);
  if (!size) {
    free(pointer);
    return 0;
  }
  Allocation *allocation = (Allocation *)pointer - 1;
  void *result = malloc(size);
  if (!result)
    return 0;
  memcpy(result, pointer, size < allocation->size ? size : allocation->size);
  free(pointer);
  return result;
}
void *memset(void *target, int value, size_t length) {
  unsigned char *destination = target;
  while (length--)
    *destination++ = (unsigned char)value;
  return target;
}
void *memcpy(void *target, const void *source, size_t length) {
  unsigned char *destination = target;
  const unsigned char *input = source;
  while (length--)
    *destination++ = *input++;
  return target;
}
void *memmove(void *target, const void *source, size_t length) {
  unsigned char *destination = target;
  const unsigned char *input = source;
  if (destination < input)
    return memcpy(target, source, length);
  while (length) {
    length--;
    destination[length] = input[length];
  }
  return target;
}
int memcmp(const void *left, const void *right, size_t length) {
  const unsigned char *a = left, *b = right;
  while (length--) {
    if (*a != *b)
      return *a - *b;
    a++;
    b++;
  }
  return 0;
}
void *memchr(const void *source, int value, size_t length) {
  const unsigned char *input = source;
  while (length--) {
    if (*input == (unsigned char)value)
      return (void *)input;
    input++;
  }
  return 0;
}
size_t strlen(const char *text) {
  size_t length = 0;
  while (text[length])
    length++;
  return length;
}
int strcmp(const char *left, const char *right) {
  while (*left && *left == *right) {
    left++;
    right++;
  }
  return (unsigned char)*left - (unsigned char)*right;
}
int strncmp(const char *left, const char *right, size_t length) {
  while (length--) {
    if (*left != *right)
      return (unsigned char)*left - (unsigned char)*right;
    if (!*left)
      return 0;
    left++;
    right++;
  }
  return 0;
}
char *strcpy(char *target, const char *source) {
  char *result = target;
  while ((*target++ = *source++))
    ;
  return result;
}
char *strncpy(char *target, const char *source, size_t length) {
  size_t index = 0;
  for (; index < length && source[index]; index++)
    target[index] = source[index];
  for (; index < length; index++)
    target[index] = 0;
  return target;
}
char *strchr(const char *text, int value) {
  do {
    if (*text == (char)value)
      return (char *)text;
  } while (*text++);
  return 0;
}
char *strrchr(const char *text, int value) {
  const char *result = 0;
  do {
    if (*text == (char)value)
      result = text;
  } while (*text++);
  return (char *)result;
}
int abs(int value) { return value < 0 ? -value : value; }
void abort(void) {
  extern unsigned char __ImageBase;
  fprintf(0, "Runtime abort at EFI offset 0x%llx\n",
          (unsigned long long)((uintptr_t)__builtin_return_address(0) -
                               (uintptr_t)&__ImageBase));
  for (;;)
    __asm__ volatile("hlt");
}
/* The formatter covers the integer and string formats used by Duktape errors.
 */
int vsnprintf(char *buffer, size_t capacity, const char *format,
              va_list arguments) {
  size_t count = 0;
#define PUT(character)                                                         \
  do {                                                                         \
    if (count + 1 < capacity)                                                  \
      buffer[count] = (character);                                             \
    count++;                                                                   \
  } while (0)
  while (*format) {
    if (*format != '%') {
      PUT(*format++);
      continue;
    }
    format++;
    int precision = -1, width = 0, long_count = 0;
    char fill = ' ';
    if (*format == '0') {
      fill = '0';
      format++;
    }
    while (*format >= '0' && *format <= '9')
      width = width * 10 + (*format++ - '0');
    if (*format == '.') {
      format++;
      precision = 0;
      if (*format == '*') {
        precision = va_arg(arguments, int);
        format++;
      } else
        while (*format >= '0' && *format <= '9')
          precision = precision * 10 + (*format++ - '0');
    }
    if (*format == 'z') {
      long_count = 2;
      format++;
    }
    while (*format == 'l') {
      long_count++;
      format++;
    }
    char kind = *format ? *format++ : 0;
    char digits[80];
    const char *text = digits;
    size_t length = 0;
    if (kind == 's') {
      text = va_arg(arguments, const char *);
      if (!text)
        text = "(null)";
      length = strlen(text);
      if (precision >= 0 && length > (size_t)precision)
        length = (size_t)precision;
    } else if (kind == 'c') {
      digits[0] = (char)va_arg(arguments, int);
      length = 1;
    } else if (kind == '%') {
      digits[0] = '%';
      length = 1;
    } else if (kind == 'd' || kind == 'i' || kind == 'u' || kind == 'x' ||
               kind == 'X' || kind == 'p') {
      uint64_t value;
      int negative = 0;
      if (kind == 'p')
        value = (uintptr_t)va_arg(arguments, void *);
      else if (kind == 'd' || kind == 'i') {
        int64_t signed_value = long_count >= 2 ? va_arg(arguments, long long)
                               : long_count    ? va_arg(arguments, long)
                                               : va_arg(arguments, int);
        negative = signed_value < 0;
        value = negative ? 0 - (uint64_t)signed_value : (uint64_t)signed_value;
      } else
        value = long_count >= 2 ? va_arg(arguments, unsigned long long)
                : long_count    ? va_arg(arguments, unsigned long)
                                : va_arg(arguments, unsigned);
      unsigned base = (kind == 'x' || kind == 'X' || kind == 'p') ? 16 : 10;
      char reverse[64];
      size_t size = 0;
      do {
        reverse[size++] = "0123456789abcdef"[value % base];
        value /= base;
      } while (value);
      if (negative)
        digits[length++] = '-';
      while (size)
        digits[length++] = reverse[--size];
    } else {
      digits[0] = '?';
      length = 1;
    }
    while (width > 0 && (size_t)width > length) {
      PUT(fill);
      width--;
    }
    for (size_t index = 0; index < length; index++)
      PUT(text[index]);
  }
  if (capacity)
    buffer[count < capacity ? count : capacity - 1] = 0;
  return (int)count;
#undef PUT
}
int snprintf(char *buffer, size_t capacity, const char *format, ...) {
  va_list arguments;
  va_start(arguments, format);
  int result = vsnprintf(buffer, capacity, format, arguments);
  va_end(arguments);
  return result;
}
int sprintf(char *buffer, const char *format, ...) {
  va_list arguments;
  va_start(arguments, format);
  int result = vsnprintf(buffer, (size_t)-1, format, arguments);
  va_end(arguments);
  return result;
}
/* Date.parse uses only decimal integer scans in this configured engine. */
int sscanf(const char *text, const char *format, ...) {
  va_list arguments;
  va_start(arguments, format);
  int count = 0;
  while (*format) {
    if (*format == '%') {
      format++;
      int width = 0;
      while (*format >= '0' && *format <= '9')
        width = width * 10 + (*format++ - '0');
      if (*format++ != 'd')
        break;
      if (!width)
        width = 32;
      int negative = *text == '-';
      if (negative)
        text++;
      int value = 0, digits = 0;
      while (width-- && *text >= '0' && *text <= '9') {
        value = value * 10 + (*text++ - '0');
        digits++;
      }
      if (!digits)
        break;
      *va_arg(arguments, int *) = negative ? -value : value;
      count++;
    } else if (*text++ != *format++)
      break;
  }
  va_end(arguments);
  return count;
}
double browser_date_now(void) {
  struct {
    uint16_t year;
    uint8_t month, day, hour, minute, second, pad;
    uint32_t nanosecond;
    int16_t timezone;
    uint8_t daylight, pad2;
  } value;
  void **runtime =
      (void **)((unsigned char *)firmware->runtime + sizeof(TableHeader));
  if (((Status(UEFI_API *)(void *, void *))runtime[0])(&value, 0))
    return NAN;
  int year = value.year, month = value.month;
  year -= month <= 2;
  int era = year / 400;
  unsigned year_of_era = (unsigned)(year - era * 400);
  unsigned day_of_year =
      (153 * (unsigned)(month + (month > 2 ? -3 : 9)) + 2) / 5 + value.day - 1;
  unsigned day_of_era =
      year_of_era * 365 + year_of_era / 4 - year_of_era / 100 + day_of_year;
  int64_t days = (int64_t)era * 146097 + day_of_era - 719468;
  return (double)(days * 86400 + value.hour * 3600 + value.minute * 60 +
                  value.second) *
             1000 +
         value.nanosecond / 1000000.0;
}

size_t malloc_usable_size(void *pointer) {
  return pointer ? ((Allocation *)pointer - 1)->size : 0;
}
#include <sys/time.h>
int gettimeofday(struct timeval *value, void *timezone) {
  (void)timezone;
  double milliseconds = browser_date_now();
  if (!value || !isfinite(milliseconds))
    return -1;
  value->tv_sec = (long long)(milliseconds / 1000);
  value->tv_usec = (long long)((milliseconds - value->tv_sec * 1000.0) * 1000);
  return 0;
}
int fprintf(FILE *stream, const char *format, ...) {
  (void)stream;
  char text[1024];
  va_list arguments;
  va_start(arguments, format);
  int result = vsnprintf(text, sizeof(text), format, arguments);
  va_end(arguments);
  extern void graphics_log(const char *);
  graphics_log(text);
  return result;
}
/* The COFF target needs a software helper for unsigned 128-bit division. */
unsigned __int128 __udivti3(unsigned __int128 numerator,
                            unsigned __int128 denominator) {
  if (!denominator)
    abort();
  unsigned __int128 remainder = 0, quotient = 0;
  for (int bit = 127; bit >= 0; bit--) {
    unsigned carry = (unsigned)(remainder >> 127);
    remainder = (remainder << 1) | ((numerator >> bit) & 1);
    if (carry || remainder >= denominator) {
      remainder -= denominator;
      quotient |= (unsigned __int128)1 << bit;
    }
  }
  return quotient;
}

void platform_assert(const char *condition, const char *file, int line) {
  fprintf(0, "Assertion failed: %s at %s:%d\n", condition, file, line);
  abort();
}
char *strstr(const char *text, const char *needle) {
  size_t length = strlen(needle);
  if (!length)
    return (char *)text;
  for (; *text; text++)
    if (!strncmp(text, needle, length))
      return (char *)text;
  return 0;
}

char *getenv(const char *name) {
  (void)name;
  return 0;
}

void runtime_release_all(void) {
  while (allocations)
    free(allocations + 1);
}

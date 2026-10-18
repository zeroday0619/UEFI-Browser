#define _GNU_SOURCE
#include "uefi.h"
#ifdef USE_MUSL
#include "virtual_memory.h"
#endif
#include <errno.h>
#include <limits.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>
extern SystemTable *platform_firmware(void);
extern void graphics_log(const char *);
struct Attributes;
typedef Status(UEFI_API *AttributeCall)(struct Attributes *, uint64_t, uint64_t,
                                   uint64_t);
struct Attributes {
  AttributeCall get, set, clear;
};
static struct Attributes *attributes;
int mprotect(void *address, size_t length, int protection) {
  if ((uintptr_t)address % 4096 || length > SIZE_MAX - 4095 ||
      (protection & ~(PROT_READ | PROT_WRITE | PROT_EXEC))) {
    errno = EINVAL;
    return -1;
  }
  if (!length)
    return 0;
  length = (length + 4095) & ~(size_t)4095;
  if (length > UINTPTR_MAX - (uintptr_t)address) {
    errno = EINVAL;
    return -1;
  }
#ifdef USE_MUSL
  if (virtual_memory_contains(address))
    return virtual_memory_protect(address, length, protection);
#endif
  const uint64_t read_protect = 0x2000, execute_protect = 0x4000,
                 read_only = 0x20000;
  uint64_t mask = read_protect | execute_protect | read_only;
  uint64_t desired = (!(protection & PROT_READ) ? read_protect : 0) |
                     (!(protection & PROT_WRITE) ? read_only : 0) |
                     (!(protection & PROT_EXEC) ? execute_protect : 0);
  if (!attributes) {
    Guid guid = {0xf4560cf6,
                 0x40ec,
                 0x4b4a,
                 {0xa1, 0x92, 0xbf, 0x1d, 0x57, 0xd0, 0xb1, 0x89}};
    ((Status(UEFI_API *)(Guid *, void *, void **))platform_firmware()
         ->boot->function[37])(&guid, 0, (void **)&attributes);
  }
  Status status;
  if (attributes) {
    status = mask & ~desired ? attributes->clear(attributes, (uintptr_t)address,
                                                 length, mask & ~desired)
                             : 0;
    if (!status && desired)
      status = attributes->set(attributes, (uintptr_t)address, length, desired);
  } else {
    // Older OVMF exposes the PI CPU protocol instead of Memory Attribute
    // Protocol.
    Guid guid = {0x26baccb1,
                 0x6f42,
                 0x11d4,
                 {0xbc, 0xe7, 0x00, 0x80, 0xc7, 0x3c, 0x88, 0x81}};
    void **cpu = 0;
    status = ((Status(UEFI_API *)(Guid *, void *, void **))platform_firmware()
                  ->boot->function[37])(&guid, 0, (void **)&cpu);
    if (!status)
      status = ((Status(UEFI_API *)(void *, uint64_t, uint64_t, uint64_t))cpu[7])(
          cpu, (uintptr_t)address, length, desired | 8);
  }
  if (status) {
    graphics_log("UEFI: memory attribute update failed\n");
    errno = ENOMEM;
    return -1;
  }
  return 0;
}
static struct {
  uintptr_t start, end;
} mappings[1024];
static int empty_mapping(void) {
  for (int index = 0; index < 1024; index++)
    if (!mappings[index].start)
      return index;
  return -1;
}
static int containing_mapping(void *address, size_t length) {
  uintptr_t start = (uintptr_t)address;
  if (length > UINTPTR_MAX - start)
    return -1;
  for (int index = 0; index < 1024; index++)
    if (start >= mappings[index].start && start + length <= mappings[index].end)
      return index;
  return -1;
}
static uint64_t page_permissions(void *address);
int platform_memory_is_read_only(const void *address) {
  uint64_t permissions = page_permissions((void *)address);
  return (permissions & 3) == 1;
}
void *mmap(void *hint, size_t length, int protection, int flags, int descriptor,
           off_t offset) {
#ifdef USE_MUSL
  return virtual_memory_map(hint, length, protection, flags, descriptor, offset);
#endif
  if (!length || length > 256u * 1024u * 1024u || !(flags & MAP_ANONYMOUS) ||
      descriptor != -1 || offset) {
    errno = ENOTSUP;
    return MAP_FAILED;
  }
  length = (length + 4095) & ~(size_t)4095;
  if (flags & MAP_FIXED) {
    if ((uintptr_t)hint % 4096 || containing_mapping(hint, length) < 0) {
      errno = ENOMEM;
      return MAP_FAILED;
    }
    if (mprotect(hint, length, PROT_READ | PROT_WRITE))
      return MAP_FAILED;
    memset(hint, 0, length);
    if (mprotect(hint, length, protection))
      return MAP_FAILED;
    return hint;
  }
  int slot = empty_mapping();
  if (slot < 0) {
    errno = ENOMEM;
    return MAP_FAILED;
  }
  size_t pages = (length + 4095) / 4096;
  uint64_t address = 0;
  Status status = ((Status(UEFI_API *)(uint32_t, uint32_t, size_t,
                                  uint64_t *))platform_firmware()
                       ->boot->function[2])(0, 1, pages, &address);
  if (status) {
    errno = ENOMEM;
    return MAP_FAILED;
  }
  memset((void *)address, 0, pages * 4096);
  if (mprotect((void *)address, pages * 4096, protection)) {
    ((Status(UEFI_API *)(uint64_t, size_t))platform_firmware()->boot->function[3])(
        address, pages);
    return MAP_FAILED;
  }
  mappings[slot].start = address;
  mappings[slot].end = address + pages * 4096;
  return (void *)address;
}
int munmap(void *address, size_t length) {
#ifdef USE_MUSL
  if (virtual_memory_contains(address))
    return virtual_memory_unmap(address, length);
#endif
  if ((uintptr_t)address % 4096 || !length) {
    errno = EINVAL;
    return -1;
  }
  length = (length + 4095) & ~(size_t)4095;
  int slot = containing_mapping(address, length);
  if (slot < 0) {
    errno = EINVAL;
    return -1;
  }
  uintptr_t start = (uintptr_t)address, end = start + length;
  int extra = -1;
  if (start > mappings[slot].start && end < mappings[slot].end) {
    extra = empty_mapping();
    if (extra < 0) {
      errno = ENOMEM;
      return -1;
    }
  }
  if (mprotect(address, (length + 4095) & ~(size_t)4095,
               PROT_READ | PROT_WRITE))
    return -1;
  Status status =
      ((Status(UEFI_API *)(uint64_t, size_t))platform_firmware()->boot->function[3])(
          (uintptr_t)address, (length + 4095) / 4096);
  if (status) {
    errno = EINVAL;
    return -1;
  }
  if (extra >= 0) {
    mappings[extra].start = end;
    mappings[extra].end = mappings[slot].end;
    mappings[slot].end = start;
  } else if (start == mappings[slot].start) {
    mappings[slot].start = end;
    if (end == mappings[slot].end)
      mappings[slot].start = mappings[slot].end = 0;
  } else
    mappings[slot].end = start;
  return 0;
}
int madvise(void *address, size_t length, int advice) {
#ifdef USE_MUSL
  if (virtual_memory_contains(address)) {
    if (advice != MADV_DONTNEED || length % 4096) { errno = EINVAL; return -1; }
    return virtual_memory_discard(address, length);
  }
#endif
  if (advice != MADV_DONTNEED || (uintptr_t)address % 4096 || length % 4096 ||
      containing_mapping(address, length) < 0) {
    errno = EINVAL;
    return -1;
  }
  uint64_t permissions = page_permissions(address);
  int uniform = 1;
  for (size_t offset = 4096; offset < length; offset += 4096)
    if (page_permissions((char *)address + offset) != permissions) {
      uniform = 0;
      break;
    }
  if (!uniform) {
    for (size_t offset = 0; offset < length; offset += 4096)
      if (madvise((char *)address + offset, 4096, advice))
        return -1;
    return 0;
  }
  int protection = permissions & 1 ? PROT_READ : PROT_NONE;
  if (permissions & 2)
    protection |= PROT_WRITE;
  if ((permissions & 1) && !(permissions & (1ULL << 63)))
    protection |= PROT_EXEC;
  if (mprotect(address, length, PROT_READ | PROT_WRITE))
    return -1;
  memset(address, 0, length);
  return mprotect(address, length, protection);
}
void platform_release_pages(void) {
#ifdef USE_MUSL
  virtual_memory_release();
#endif
  for (int index = 0; index < 1024; index++)
    if (mappings[index].start &&
        munmap((void *)mappings[index].start,
               mappings[index].end - mappings[index].start))
      abort();
}

#ifdef USE_MUSL
static long firmware_memory_pages(int available_only) {
  struct Descriptor {
    uint32_t type;
    uint64_t physical, virtual_address, pages, attributes;
  };
  typedef Status(UEFI_API *GetMemoryMap)(size_t *, void *, size_t *, size_t *, uint32_t *);
  GetMemoryMap get_map = (GetMemoryMap)platform_firmware()->boot->function[4];
  size_t length = 0, key = 0, stride = 0;
  uint32_t version = 0;
  Status status = get_map(&length, NULL, &key, &stride, &version);
  if (status != ERROR(5)) { errno = EIO; return -1; }
  for (unsigned attempt = 0; attempt < 4; ++attempt) {
    if (!length || length > 1024 * 1024 || stride > 4096) break;
    size_t capacity = length + 16 * (stride ? stride : sizeof(struct Descriptor));
    void *buffer = malloc(capacity);
    if (!buffer) { errno = ENOMEM; return -1; }
    length = capacity;
    status = get_map(&length, buffer, &key, &stride, &version);
    if (status == ERROR(5)) { free(buffer); continue; }
    if (status || stride < sizeof(struct Descriptor) || length > capacity || length % stride) {
      free(buffer); break;
    }
    uint64_t pages = 0;
    for (size_t offset = 0; offset < length; offset += stride) {
      struct Descriptor descriptor;
      memcpy(&descriptor, (char *)buffer + offset, sizeof(descriptor));
      int include = available_only ? descriptor.type == 7 :
          ((descriptor.type >= 1 && descriptor.type <= 7) ||
           descriptor.type == 9 || descriptor.type == 10 || descriptor.type == 13);
      if (include) {
        if (descriptor.pages > (uint64_t)LONG_MAX - pages) {
          free(buffer); errno = EOVERFLOW; return -1;
        }
        pages += descriptor.pages;
      }
    }
    free(buffer);
    return (long)pages;
  }
  errno = EIO;
  return -1;
}
#endif
long sysconf(int name) {
  switch (name) {
  case _SC_PAGESIZE:
    return 4096;
  case _SC_NPROCESSORS_ONLN:
  case _SC_NPROCESSORS_CONF:
    return 1;
  case _SC_THREAD_STACK_MIN:
    return 16384;
#ifdef USE_MUSL
  case _SC_PHYS_PAGES:
    return firmware_memory_pages(0);
  case _SC_AVPHYS_PAGES:
    return firmware_memory_pages(1);
#endif
  default:
    errno = EINVAL;
    return -1;
  }
}
#ifndef USE_MUSL
pid_t getpid(void) { return 1; }
pid_t gettid(void) { return 1; }
pthread_t pthread_self(void) { return 1; }
int pthread_equal(pthread_t left, pthread_t right) { return left == right; }
int pthread_mutex_init(pthread_mutex_t *mutex,
                       const pthread_mutexattr_t *attribute) {
  (void)attribute;
  memset(mutex, 0, sizeof(*mutex));
  return 0;
}
int pthread_mutex_destroy(pthread_mutex_t *mutex) {
  return *(int *)mutex ? EBUSY : 0;
}
int pthread_mutex_lock(pthread_mutex_t *mutex) {
  if (*(int *)mutex) {
    graphics_log("FAIL: recursive single-thread mutex lock\n");
    abort();
  }
  *(int *)mutex = 1;
  return 0;
}
int pthread_mutex_trylock(pthread_mutex_t *mutex) {
  if (*(int *)mutex)
    return EBUSY;
  *(int *)mutex = 1;
  return 0;
}
int pthread_mutex_unlock(pthread_mutex_t *mutex) {
  if (!*(int *)mutex)
    return EPERM;
  *(int *)mutex = 0;
  return 0;
}
int pthread_once(pthread_once_t *once, void (*callback)(void)) {
  if (*once == 2)
    return 0;
  if (*once == 1)
    abort();
  *once = 1;
  callback();
  *once = 2;
  return 0;
}
static struct {
  int used;
  void *value;
  void (*destructor)(void *);
} keys[256];
int pthread_key_create(pthread_key_t *key, void (*destructor)(void *)) {
  for (unsigned index = 0; index < 256; index++)
    if (!keys[index].used) {
      keys[index].used = 1;
      keys[index].destructor = destructor;
      *key = index;
      return 0;
    }
  return EAGAIN;
}
int pthread_key_delete(pthread_key_t key) {
  if (key >= 256 || !keys[key].used)
    return EINVAL;
  memset(keys + key, 0, sizeof(keys[key]));
  return 0;
}
int pthread_setspecific(pthread_key_t key, const void *value) {
  if (key >= 256 || !keys[key].used)
    return EINVAL;
  keys[key].value = (void *)value;
  return 0;
}
void *pthread_getspecific(pthread_key_t key) {
  return key < 256 && keys[key].used ? keys[key].value : 0;
}
int pthread_create(pthread_t *thread, const pthread_attr_t *attribute,
                   void *(*entry)(void *), void *argument) {
  (void)thread;
  (void)attribute;
  (void)entry;
  (void)argument;
  return ENOTSUP;
}
int pthread_join(pthread_t thread, void **value) {
  (void)thread;
  (void)value;
  return ENOTSUP;
}
int sched_yield(void) { return 0; }
static void *stack_base;
static size_t stack_size;
void platform_set_stack(void *base, size_t size) {
  stack_base = base;
  stack_size = size;
}
int pthread_attr_init(pthread_attr_t *attribute) {
  memset(attribute, 0, sizeof(*attribute));
  return 0;
}
int pthread_attr_destroy(pthread_attr_t *attribute) {
  (void)attribute;
  return 0;
}
int pthread_getattr_np(pthread_t thread, pthread_attr_t *attribute) {
  if (thread != 1)
    return ESRCH;
  return pthread_attr_init(attribute);
}
int pthread_attr_getstack(const pthread_attr_t *attribute, void **base,
                          size_t *size) {
  (void)attribute;
  *base = stack_base;
  *size = stack_size;
  return stack_base ? 0 : EINVAL;
}
int pthread_attr_getguardsize(const pthread_attr_t *attribute, size_t *size) {
  (void)attribute;
  *size = 0;
  return 0;
}
int pthread_attr_setstacksize(pthread_attr_t *attribute, size_t size) {
  (void)attribute;
  (void)size;
  return ENOTSUP;
}
#endif
static uint64_t ticks(void) {
  uint32_t low, high;
  __asm__ volatile("lfence;rdtsc" : "=a"(low), "=d"(high)::"memory");
  return ((uint64_t)high << 32) | low;
}
static uint64_t frequency, origin;
static uint64_t monotonic_nanoseconds(void) {
  if (!frequency) {
    origin = ticks();
    ((Status(UEFI_API *)(size_t))platform_firmware()->boot->function[28])(10000);
    frequency = (ticks() - origin) * 100;
    if (!frequency)
      abort();
  }
  uint64_t elapsed = ticks() - origin;
  return elapsed / frequency * 1000000000ULL +
         (elapsed % frequency) * 1000000000ULL / frequency;
}
struct FirmwareTime {
  uint16_t year;
  uint8_t month, day, hour, minute, second, pad;
  uint32_t nanosecond;
  int16_t timezone;
  uint8_t daylight, pad2;
};
static int64_t calendar_days(int year, unsigned month, unsigned day) {
  year -= month <= 2;
  int era = (year >= 0 ? year : year - 399) / 400;
  unsigned y = (unsigned)(year - era * 400);
  unsigned doy = (153 * (month + (month > 2 ? -3 : 9)) + 2) / 5 + day - 1;
  return (int64_t)era * 146097 + y * 365 + y / 4 - y / 100 + doy - 719468;
}
int clock_gettime(clockid_t clock, struct timespec *result) {
  if (clock == CLOCK_MONOTONIC || clock == CLOCK_MONOTONIC_RAW ||
      clock == CLOCK_MONOTONIC_COARSE || clock == CLOCK_BOOTTIME) {
    uint64_t value = monotonic_nanoseconds();
    result->tv_sec = value / 1000000000;
    result->tv_nsec = value % 1000000000;
    return 0;
  }
  if (clock == CLOCK_REALTIME) {
    struct FirmwareTime current;
    void **services = (void **)((unsigned char *)platform_firmware()->runtime +
                                sizeof(TableHeader));
    Status status = ((Status(UEFI_API *)(struct FirmwareTime *, void *))services[0])(
        &current, 0);
    if (status) {
      errno = EIO;
      return -1;
    }
    result->tv_sec =
        calendar_days(current.year, current.month, current.day) * 86400 +
        current.hour * 3600 + current.minute * 60 + current.second;
    if (current.timezone != 2047)
      result->tv_sec -= current.timezone * 60;
    result->tv_nsec = current.nanosecond;
    return 0;
  }
  fprintf(stderr, "UEFI: unsupported clock identifier %d\n", (int)clock);
  errno = EINVAL;
  return -1;
}
int nanosleep(const struct timespec *request, struct timespec *remaining) {
  if (request->tv_sec < 0 || request->tv_nsec < 0 ||
      request->tv_nsec >= 1000000000) {
    errno = EINVAL;
    return -1;
  }
  ((Status(UEFI_API *)(size_t))platform_firmware()->boot->function[28])(
      (size_t)request->tv_sec * 1000000 + (request->tv_nsec + 999) / 1000);
  if (remaining)
    memset(remaining, 0, sizeof(*remaining));
  return 0;
}
struct RandomProtocol {
  void *information;
  Status(UEFI_API *generate)(struct RandomProtocol *, Guid *, size_t,
                        unsigned char *);
};
_Bool firmware_entropy(unsigned char *buffer, size_t size) {
  Guid guid = {0x3152bca5,
               0xeade,
               0x433d,
               {0x86, 0x2e, 0xc0, 0x1c, 0xdc, 0x29, 0x1f, 0x44}};
  struct RandomProtocol *random = 0;
  Status status = ((Status(UEFI_API *)(Guid *, void *, void **))platform_firmware()
                       ->boot->function[37])(&guid, 0, (void **)&random);
  return !status && random && !random->generate(random, 0, size, buffer);
}
static uint64_t page_permissions(void *address) {
  uintptr_t root, control;
  __asm__ volatile("mov %%cr3,%0" : "=r"(root));
  __asm__ volatile("mov %%cr4,%0" : "=r"(control));
  unsigned shift = (control & (1u << 12)) ? 48 : 39;
  uint64_t entry = 0, writable = 2, executable = 0;
  for (;;) {
    const uint64_t *table = (const uint64_t *)(root & 0x000ffffffffff000ULL);
    entry = table[((uintptr_t)address >> shift) & 511];
    if (!(entry & 1))
      return 0;
    writable &= entry;
    executable |= entry;
    if (shift == 12 || (entry & 128))
      return 1 | writable | (executable & (1ULL << 63));
    root = entry;
    shift -= 9;
  }
}
int run_platform_probe(void) {
#ifdef USE_MUSL
  long total_pages = sysconf(_SC_PHYS_PAGES);
  long available_pages = sysconf(_SC_AVPHYS_PAGES);
  if (available_pages <= 0 || total_pages <= available_pages) return 16;
  size_t reservation_size = 8ULL * 1024 * 1024 * 1024;
  unsigned char *reservation = mmap(NULL, reservation_size, PROT_NONE,
      MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
  if (reservation == MAP_FAILED) return 10;
  errno = 0;
  if (mmap((void *)(UINTPTR_MAX - 4095), 8192, PROT_READ | PROT_WRITE,
      MAP_FIXED | MAP_PRIVATE | MAP_ANONYMOUS, -1, 0) != MAP_FAILED ||
      errno != ENOMEM) return 15;
  unsigned char *committed = reservation + reservation_size / 2;
  if (mprotect(committed, 4096, PROT_READ | PROT_WRITE)) return 11;
  committed[123] = 0x5a;
  if (mprotect(committed, 4096, PROT_NONE) ||
      page_permissions(committed) != 0 ||
      mprotect(committed, 4096, PROT_READ) || committed[123] != 0x5a)
    return 12;
  if (madvise(committed, 4096, MADV_DONTNEED) || committed[123] ||
      (page_permissions(committed) & 2)) return 13;
  if (munmap(reservation, reservation_size)) return 14;
  graphics_log("PASS: 8 GiB virtual reservation, sparse commit and protection\n");
#endif
  void *pages =
      mmap(0, 8192, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  if (pages == MAP_FAILED) {
    graphics_log("FAIL: UEFI page allocation or permissions\n");
    return 1;
  }
  ((uint64_t *)pages)[0] = 0x123456789abcdefULL;
  if (mprotect(pages, 8192, PROT_READ) ||
      ((uint64_t *)pages)[0] != 0x123456789abcdefULL ||
      (page_permissions(pages) & 2) ||
      !(page_permissions(pages) & (1ULL << 63)))
    return 2;
  if (mprotect(pages, 8192, PROT_NONE) || page_permissions(pages) != 0 ||
      munmap(pages, 8192))
    return 5;
  unsigned char *region = mmap(0, 16384, PROT_READ | PROT_WRITE,
                               MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  if (region == MAP_FAILED)
    return 7;
  memset(region, 0x5a, 16384);
  if (mmap(region + 4096, 8192, PROT_READ | PROT_WRITE,
           MAP_FIXED | MAP_PRIVATE | MAP_ANONYMOUS, -1, 0) != region + 4096 ||
      region[0] != 0x5a || region[4096] || region[12288] != 0x5a)
    return 8;
  if (munmap(region + 4096, 8192) || munmap(region, 4096) ||
      munmap(region + 12288, 4096))
    return 9;
  struct timespec before, after;
  if (clock_gettime(CLOCK_MONOTONIC, &before))
    return 3;
  struct timespec wait = {0, 1000000};
  nanosleep(&wait, 0);
  if (clock_gettime(CLOCK_MONOTONIC, &after) ||
      (after.tv_sec == before.tv_sec && after.tv_nsec <= before.tv_nsec))
    return 4;
  unsigned char entropy[32];
  if (!firmware_entropy(entropy, sizeof(entropy))) {
    graphics_log("FAIL: firmware entropy unavailable\n");
    return 6;
  }
  graphics_log("PASS: UEFI pages, read-only protection, clock and RNG\n");
  return 0;
}

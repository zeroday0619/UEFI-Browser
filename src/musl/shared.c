#define _GNU_SOURCE
#include "../virtual_memory.h"
#include "firmware_shared.h"
#include "uefi.h"
#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#define HANDLE_BASE 2048
#define HANDLE_COUNT 256
#define MAPPING_COUNT 512
#define MEMORY_LIMIT (64u * 1024u * 1024u)
#define REGION_LIMIT (8u * 1024u * 1024u)
extern SystemTable *platform_firmware(void);
typedef struct {
  uint64_t physical;
  size_t size, pages;
  unsigned references, seals;
} Memory;
typedef struct {
  Memory *memory;
  int flags;
} Descriptor;
typedef struct {
  Memory *memory;
  void *address;
  size_t length;
} Mapping;
static Descriptor descriptors[HANDLE_COUNT];
static Mapping mappings[MAPPING_COUNT];
static size_t allocated;
int firmware_shared_descriptor(long descriptor) {
  return descriptor >= HANDLE_BASE && descriptor < HANDLE_BASE + HANDLE_COUNT;
}
static Descriptor *lookup(long descriptor) {
  return firmware_shared_descriptor(descriptor) &&
                 descriptors[descriptor - HANDLE_BASE].memory
             ? &descriptors[descriptor - HANDLE_BASE]
             : NULL;
}
static void release(Memory *memory) {
  if (--memory->references)
    return;
  if (memory->physical) {
    if (((Status(UEFI_API *)(uint64_t, size_t))platform_firmware()
             ->boot->function[3])(memory->physical, memory->pages))
      abort();
    allocated -= memory->pages * 4096;
  }
  free(memory);
}
static long duplicate(Memory *memory, int flags, long minimum) {
  for (long index = 0; index < HANDLE_COUNT; ++index)
    if (index + HANDLE_BASE >= minimum && !descriptors[index].memory) {
      descriptors[index] = (Descriptor){memory, flags};
      ++memory->references;
      return index + HANDLE_BASE;
    }
  return -EMFILE;
}
long firmware_shared_reopen(const char *path, int flags) {
  static const char prefix[] = "/proc/self/fd/";
  if (!path || strncmp(path, prefix, sizeof(prefix) - 1) ||
      (flags & ~(O_CLOEXEC | O_LARGEFILE)) != O_RDONLY)
    return -ENOENT;
  const char *number = path + sizeof(prefix) - 1;
  long descriptor = 0;
  if (!*number)
    return -ENOENT;
  for (; *number; ++number) {
    if (*number < '0' || *number > '9' || descriptor > 100000)
      return -ENOENT;
    descriptor = descriptor * 10 + *number - '0';
  }
  Descriptor *entry = lookup(descriptor);
  if (!entry)
    return -EBADF;
  return duplicate(entry->memory, flags, 0);
}
long firmware_shared_syscall(long number, long first, long second, long third) {
  if (number == SYS_memfd_create) {
    if (second & ~(1L | 2L | 8L))
      return -EINVAL;
    Memory *memory = calloc(1, sizeof(*memory));
    if (!memory)
      return -ENOMEM;
    memory->seals = (second & 2) ? 0 : F_SEAL_SEAL;
    long result = duplicate(memory, O_RDWR | ((second & 1) ? O_CLOEXEC : 0), 0);
    if (result < 0)
      free(memory);
    return result;
  }
  Descriptor *entry = lookup(first);
  if (!entry)
    return -EBADF;
  Memory *memory = entry->memory;
  if (number == SYS_close) {
    entry->memory = NULL;
    release(memory);
    return 0;
  }
  if (number == SYS_dup)
    return duplicate(memory, entry->flags & ~O_CLOEXEC, 0);
  if (number == SYS_fcntl) {
    if (second == F_GETFL)
      return entry->flags & ~O_CLOEXEC;
    if (second == F_GETFD)
      return entry->flags & O_CLOEXEC ? FD_CLOEXEC : 0;
    if (second == F_SETFD) {
      entry->flags =
          (entry->flags & ~O_CLOEXEC) | ((third & FD_CLOEXEC) ? O_CLOEXEC : 0);
      return 0;
    }
    if (second == F_DUPFD || second == F_DUPFD_CLOEXEC)
      return third < 0
                 ? -EINVAL
                 : duplicate(memory,
                             (entry->flags & ~O_CLOEXEC) |
                                 (second == F_DUPFD_CLOEXEC ? O_CLOEXEC : 0),
                             third);
    if (second == F_GET_SEALS)
      return memory->seals;
    if (second == F_ADD_SEALS) {
      if ((entry->flags & O_ACCMODE) != O_RDWR || (memory->seals & F_SEAL_SEAL))
        return -EPERM;
      if (third & ~(F_SEAL_SHRINK | F_SEAL_GROW | F_SEAL_SEAL))
        return -EINVAL;
      memory->seals |= third;
      return 0;
    }
    return -EINVAL;
  }
  if (number == SYS_fstat) {
    struct stat *status = (void *)second;
    if (!status)
      return -EFAULT;
    memset(status, 0, sizeof(*status));
    status->st_mode = S_IFREG | 0600;
    status->st_size = memory->size;
    status->st_nlink = 1;
    status->st_blksize = 4096;
    status->st_blocks = memory->pages * 8;
    return 0;
  }
  if (number == SYS_ftruncate) {
    if ((entry->flags & O_ACCMODE) != O_RDWR)
      return -EBADF;
    if (second < 0 || (size_t)second > REGION_LIMIT)
      return -EINVAL;
    if (((size_t)second > memory->size && (memory->seals & F_SEAL_GROW)) ||
        ((size_t)second < memory->size && (memory->seals & F_SEAL_SHRINK)))
      return -EPERM;
    if ((size_t)second == memory->size)
      return 0;
    if (memory->physical)
      return -ENOTSUP;
    size_t pages = ((size_t)second + 4095) / 4096;
    if (pages * 4096 > MEMORY_LIMIT - allocated)
      return -ENOMEM;
    uint64_t physical = 0;
    if (pages && ((Status(UEFI_API *)(uint32_t, uint32_t, size_t,
                                      uint64_t *))platform_firmware()
                      ->boot->function[2])(0, 2, pages, &physical))
      return -ENOMEM;
    if (pages)
      memset((void *)physical, 0, pages * 4096);
    memory->physical = physical;
    memory->size = second;
    memory->pages = pages;
    allocated += pages * 4096;
    return 0;
  }
  return -ENOSYS;
}
void *firmware_shared_map(size_t length, int protection, int flags,
                          int descriptor, off_t offset) {
  Descriptor *entry = lookup(descriptor);
  if (!entry || !length || length > REGION_LIMIT || offset < 0 ||
      offset % 4096 || (size_t)offset > entry->memory->size ||
      length > entry->memory->size - (size_t)offset || flags != MAP_SHARED ||
      (protection & ~(PROT_READ | PROT_WRITE))) {
    errno = EINVAL;
    return MAP_FAILED;
  }
  if ((protection & PROT_WRITE) && (entry->flags & O_ACCMODE) != O_RDWR) {
    errno = EACCES;
    return MAP_FAILED;
  }
  unsigned index;
  for (index = 0; index < MAPPING_COUNT && mappings[index].memory; ++index) {
  }
  if (index == MAPPING_COUNT) {
    errno = ENOMEM;
    return MAP_FAILED;
  }
  length = (length + 4095) & ~(size_t)4095;
  void *address =
      virtual_memory_alias(entry->memory->physical + offset, length, protection,
                           (entry->flags & O_ACCMODE) == O_RDWR);
  if (address == MAP_FAILED)
    return address;
  mappings[index] = (Mapping){entry->memory, address, length};
  ++entry->memory->references;
  return address;
}
int firmware_shared_mapping(const void *address) {
  for (unsigned index = 0; index < MAPPING_COUNT; ++index)
    if (mappings[index].memory && address >= mappings[index].address &&
        (uintptr_t)address - (uintptr_t)mappings[index].address <
            mappings[index].length)
      return 1;
  return 0;
}
int firmware_shared_unmap(void *address, size_t length) {
  if (!length || length > REGION_LIMIT) {
    errno = EINVAL;
    return -1;
  }
  length = (length + 4095) & ~(size_t)4095;
  for (unsigned index = 0; index < MAPPING_COUNT; ++index) {
    Mapping *mapping = &mappings[index];
    if (mapping->memory && mapping->address == address &&
        mapping->length == length) {
      if (virtual_memory_unmap(address, length))
        return -1;
      release(mapping->memory);
      memset(mapping, 0, sizeof(*mapping));
      return 0;
    }
  }
  errno = EINVAL;
  return -1;
}
void firmware_shared_reset(void) {
  for (unsigned index = 0; index < MAPPING_COUNT; ++index)
    if (mappings[index].memory)
      if (firmware_shared_unmap(mappings[index].address,
                                mappings[index].length))
        abort();
  for (unsigned index = 0; index < HANDLE_COUNT; ++index)
    if (descriptors[index].memory)
      firmware_shared_syscall(SYS_close, index + HANDLE_BASE, 0, 0);
}

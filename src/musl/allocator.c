#include "platform/runtime.h"
#include "renderer_runtime.h"
#include <errno.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static SystemTable *firmware;
static size_t allocated, committed;
static size_t heap_limit = 128u * 1024u * 1024u;
int _fltused;
typedef struct Allocation {
  size_t size, raw_size;
  void *base;
  struct Allocation *previous, *next;
} Allocation;
static Allocation *allocations;
static unsigned failure_reports;

static void allocation_failure(size_t requested, size_t status) {
  /* Allocation failure reporting must not allocate memory or take stdio locks. */
  if (failure_reports >= 8) return;
  ++failure_reports;
  char message[] = "ALLOCATOR: failure requested=0000000000000000 used=0000000000000000 committed=0000000000000000 limit=0000000000000000 status=0000000000000000 child=0\n";
  const size_t values[] = {requested, allocated, committed, heap_limit, status};
  char *position = message;
  static const char digits[] = "0123456789abcdef";
  for (size_t index = 0; index < 5; ++index) {
    while (*position != '=') ++position;
    ++position;
    for (unsigned digit = 0; digit < 16; ++digit)
      *position++ = digits[(values[index] >> ((15 - digit) * 4)) & 15];
  }
  message[sizeof(message) - 3] = renderer_runtime_is_child() ? '1' : '0';
  (void)write(2, message, sizeof(message) - 1);
}

size_t runtime_memory_used(void) { return allocated; }
size_t runtime_memory_limit(void) { return heap_limit; }
size_t runtime_memory_committed(void) { return committed; }
void runtime_initialize(SystemTable *table) {
  firmware = table;
  /* Reserve most available RAM for virtual commitments, firmware and other processes. */
  long pages = sysconf(_SC_AVPHYS_PAGES);
  if (pages > 0) {
    uint64_t budget = pages > 1024 * 1024 ? 1024u * 1024u * 1024u : (uint64_t)pages * 1024;
    heap_limit = (size_t)budget;
  }
}

typedef struct Slab Slab;
typedef struct FreeBlock {
  Slab *owner;
  struct FreeBlock *next;
} FreeBlock;
struct Slab {
  Slab *previous, *next;
  FreeBlock *available;
  size_t live;
};
#define SLAB_BYTES 65536u
static Slab *available_slabs[7];
static unsigned block_class(size_t size) {
  unsigned index = 0;
  if (size > 4096 - sizeof(FreeBlock)) return 7;
  while (index < 7 && size + sizeof(FreeBlock) > ((size_t)64 << index)) ++index;
  return index;
}
static void remove_slab(Slab *slab, unsigned index) {
  if (slab->previous) slab->previous->next = slab->next;
  else available_slabs[index] = slab->next;
  if (slab->next) slab->next->previous = slab->previous;
  slab->previous = slab->next = NULL;
}
static void insert_slab(Slab *slab, unsigned index) {
  slab->previous = NULL;
  slab->next = available_slabs[index];
  if (slab->next) slab->next->previous = slab;
  available_slabs[index] = slab;
}
static void *firmware_allocate(size_t size) {
  void *base = NULL;
  Status status = ((Status(UEFI_API *)(uint32_t,size_t,void**))firmware->boot->function[5])(2,size,&base);
  if (status) {
    allocation_failure(size, status);
    return NULL;
  }
  committed += size;
  return base;
}
static void firmware_release(void *base, size_t size) {
  if (((Status(UEFI_API *)(void*))firmware->boot->function[6])(base)) abort();
  committed -= size;
}
static void *raw_allocate(size_t size) {
  unsigned index = renderer_runtime_is_child() ? block_class(size) : 7;
  if (index == 7) return firmware_allocate(size);
  Slab *slab = available_slabs[index];
  if (!slab) {
    slab = firmware_allocate(SLAB_BYTES + sizeof(*slab));
    if (!slab) return NULL;
    memset(slab, 0, sizeof(*slab));
    size_t stride = (size_t)64 << index;
    for (size_t offset = 0; offset < SLAB_BYTES; offset += stride) {
      FreeBlock *block = (void *)((unsigned char *)(slab + 1) + offset);
      block->owner = slab;
      block->next = slab->available;
      slab->available = block;
    }
    insert_slab(slab, index);
  }
  FreeBlock *block = slab->available;
  slab->available = block->next;
  ++slab->live;
  if (!slab->available) remove_slab(slab, index);
  return block + 1;
}
static void raw_release(void *base, size_t size) {
  unsigned index = renderer_runtime_is_child() ? block_class(size) : 7;
  if (index == 7) { firmware_release(base, size); return; }
  FreeBlock *block = (FreeBlock *)base - 1;
  Slab *slab = block->owner;
  if (!slab->available) insert_slab(slab, index);
  block->next = slab->available;
  slab->available = block;
  /* Empty slabs must not retain firmware pages after temporary allocation bursts. */
  if (!--slab->live) {
    remove_slab(slab, index);
    firmware_release(slab, SLAB_BYTES + sizeof(*slab));
  }
}
void *aligned_alloc(size_t alignment, size_t size) {
  if (alignment < 16) alignment = 16;
  if (alignment & (alignment - 1)) { errno = EINVAL; return NULL; }
  if (!size) size = 1;
  if (size > heap_limit || allocated > heap_limit - size ||
      alignment - 1 > SIZE_MAX - sizeof(Allocation) ||
      size > SIZE_MAX - sizeof(Allocation) - (alignment - 1)) {
    allocation_failure(size, 0);
    errno = ENOMEM;
    return NULL;
  }
  size_t raw_size = size + sizeof(Allocation) + alignment - 1;
  void *base = raw_allocate(raw_size);
  if (!base) { errno = ENOMEM; return NULL; }
  Allocation *result = (Allocation *)(((uintptr_t)base + sizeof(Allocation) + alignment - 1) &
                                      ~(uintptr_t)(alignment - 1)) - 1;
  result->base = base;
  result->raw_size = raw_size;
  result->size = size;
  result->previous = NULL;
  result->next = allocations;
  if (allocations) allocations->previous = result;
  allocations = result;
  allocated += size;
  return result + 1;
}
void *malloc(size_t size) { return aligned_alloc(16, size); }
int posix_memalign(void **pointer, size_t alignment, size_t size) {
  if (alignment < sizeof(void *) || (alignment & (alignment - 1))) return EINVAL;
  int saved_errno = errno;
  void *result = aligned_alloc(alignment, size);
  errno = saved_errno;
  if (!result) return ENOMEM;
  *pointer = result;
  return 0;
}
void free(void *pointer) {
  if (!pointer) return;
  int saved_errno = errno;
  Allocation *allocation = (Allocation *)pointer - 1;
  if (allocation->previous) allocation->previous->next = allocation->next;
  else allocations = allocation->next;
  if (allocation->next) allocation->next->previous = allocation->previous;
  allocated -= allocation->size;
  raw_release(allocation->base, allocation->raw_size);
  errno = saved_errno;
}
void *calloc(size_t count, size_t size) {
  if (size && count > SIZE_MAX / size) { errno = ENOMEM; return NULL; }
  void *result = malloc(count * size);
  if (result) memset(result, 0, count * size);
  return result;
}
void *realloc(void *pointer, size_t size) {
  if (!pointer) return malloc(size);
  if (!size) { free(pointer); return NULL; }
  Allocation *allocation = (Allocation *)pointer - 1;
  size_t capacity = allocation->raw_size - ((uintptr_t)pointer - (uintptr_t)allocation->base);
  if (size <= capacity) {
    if (size > allocation->size && (size > heap_limit || allocated - allocation->size > heap_limit - size)) {
      allocation_failure(size, 0);
      errno = ENOMEM; return NULL;
    }
    allocated = allocated - allocation->size + size;
    allocation->size = size;
    return pointer;
  }
  void *result = malloc(size);
  if (!result) return NULL;
  memcpy(result, pointer, allocation->size);
  free(pointer);
  return result;
}
void runtime_release_all(void) {
  while (allocations) free(allocations + 1);
}
size_t malloc_usable_size(void *pointer) {
  return pointer ? ((Allocation *)pointer - 1)->size : 0;
}
/* Musl internal allocations share the tracked firmware allocator. */
void *__libc_malloc(size_t size) { return malloc(size); }
void *__libc_calloc(size_t count, size_t size) { return calloc(count, size); }
void *__libc_realloc(void *pointer, size_t size) { return realloc(pointer, size); }
void __libc_free(void *pointer) { free(pointer); }

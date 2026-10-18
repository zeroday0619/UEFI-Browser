#include "platform/runtime.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static SystemTable *firmware;
static size_t allocated;
int _fltused;
#define HEAP_LIMIT (128u * 1024u * 1024u)
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

void runtime_release_all(void) {
  while (allocations)
    free(allocations + 1);
}
size_t malloc_usable_size(void *pointer) {
  return pointer ? ((Allocation *)pointer - 1)->size : 0;
}
/* musl internal allocations share the tracked firmware allocator. */
void *__libc_malloc(size_t size) { return malloc(size); }
void *__libc_calloc(size_t count, size_t size) { return calloc(count, size); }
void *__libc_realloc(void *pointer, size_t size) {
  return realloc(pointer, size);
}
void __libc_free(void *pointer) { free(pointer); }

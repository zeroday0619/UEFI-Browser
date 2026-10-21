#include "uefi.h"
#include "virtual_memory.h"
#include "x86_page_cache.h"
#include "renderer_runtime.h"
#include "renderer_sandbox.h"
#include "firmware_shared.h"
#include <errno.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <time.h>
#include <sys/mman.h>

/* An unused lower-half PML4 entry isolates reservations from firmware mappings. */
#define PAGE_SIZE 4096ULL
#define ARENA_SIZE (1ULL << 39)
#define ADDRESS_MASK 0x000ffffffffff000ULL
#define SHARED_PAGE (1ULL << 9)
#define SHARED_WRITABLE (1ULL << 10)
#define EXECUTE_DISABLE (1ULL << 63)
#include "virtual_regions.h"
extern SystemTable *platform_firmware(void);
static uint64_t *firmware_root;
static uintptr_t original_root;
static unsigned arena_index;
static uintptr_t arena_start;

extern const int firmware_task_diagnostics;
static _Thread_local uint64_t jit_scope_calls;
static _Thread_local uint64_t jit_scope_nanoseconds;

void firmware_jit_diagnostics(uint64_t *calls, uint64_t *nanoseconds) {
  *calls = jit_scope_calls;
  *nanoseconds = jit_scope_nanoseconds;
}

int firmware_jit_write_scope(int enter) {
  if (!renderer_runtime_sandboxed() || !renderer_runtime_is_child()) return 0;
  if (!firmware_task_diagnostics)
    return (int)renderer_system_call(RENDERER_SYSTEM_JIT_SCOPE,enter,0,0,0,0);
  struct timespec started, finished;
  int clock_valid = clock_gettime(CLOCK_MONOTONIC, &started) == 0;
  int result = (int)renderer_system_call(RENDERER_SYSTEM_JIT_SCOPE,enter,0,0,0,0);
  ++jit_scope_calls;
  if (clock_valid && clock_gettime(CLOCK_MONOTONIC, &finished) == 0) {
    int64_t elapsed = (int64_t)(finished.tv_sec-started.tv_sec)*1000000000 +
                      finished.tv_nsec-started.tv_nsec;
    if (elapsed >= 0) jit_scope_nanoseconds += (uint64_t)elapsed;
  }
  return result;
}

size_t virtual_memory_reservation_limit(void) { return ARENA_SIZE; }

#include "renderer_pages.h"
size_t virtual_memory_renderer_backing_size(void) {
  size_t size = 0;
  for (PageBatch *batch = page_batches; batch; batch = batch->next)
    size += 256 * PAGE_SIZE;
  return size;
}
static void *allocate_page(void) {
  if (renderer_runtime_is_child()) {
    void *page=renderer_page();
    if (!page) errno=ENOMEM;
    return page;
  }
  uint64_t address = 0;
  Status result = ((Status(UEFI_API *)(uint32_t, uint32_t, size_t, uint64_t *))
      platform_firmware()->boot->function[2])(0, 2, 1, &address);
  if (result) { errno = ENOMEM; return NULL; }
  memset((void *)address, 0, PAGE_SIZE);
  return (void *)address;
}
static void free_page(uint64_t address) {
  if (renderer_runtime_is_child()) { renderer_page_release(address & ADDRESS_MASK); return; }
  if (((Status(UEFI_API *)(uint64_t, size_t))
       platform_firmware()->boot->function[3])(address & ADDRESS_MASK, 1))
    abort();
}
static int initialize(void) {
  if (arena_start) return 0;
  if (renderer_runtime_sandboxed() && renderer_runtime_is_child()) {
    firmware_root=allocate_page();
    if (!firmware_root) return -1;
    arena_index=128; arena_start=(uintptr_t)arena_index<<39;
    return 0;
  }
  uintptr_t root, control;
  __asm__ volatile("mov %%cr3,%0" : "=r"(root));
  __asm__ volatile("mov %%cr4,%0" : "=r"(control));
  if (control & (1ULL << 12)) { errno = ENOTSUP; return -1; }
  uint64_t *current_root = (uint64_t *)(root & ADDRESS_MASK);
  for (arena_index = 128; arena_index < 256; ++arena_index) {
    if (current_root[arena_index] != 0) continue;
    firmware_root = allocate_page();
    if (!firmware_root) return -1;
    memcpy(firmware_root, current_root, PAGE_SIZE);
    void *directory = allocate_page();
    if (!directory) { free_page((uintptr_t)firmware_root); return -1; }
    firmware_root[arena_index] = (uintptr_t)directory | 3;
    original_root = root;
    uintptr_t replacement = (uintptr_t)firmware_root | (root & 0xfff);
    __asm__ volatile("mov %0,%%cr3" :: "r"(replacement) : "memory");
    arena_start = (uintptr_t)arena_index << 39;
    return 0;
  }
  errno = ENOMEM;
  return -1;
}
int virtual_memory_contains(const void *address) {
  uintptr_t start = (uintptr_t)address;
  return arena_start && start >= arena_start && start - arena_start < ARENA_SIZE;
}
static uint64_t *leaf(uintptr_t address, int create) {
  uint64_t *table = firmware_root;
  for (unsigned shift = 39; shift > 12; shift -= 9) {
    uint64_t *entry = &table[(address >> shift) & 511];
    if (!*entry) {
      if (!create) return NULL;
      void *page = allocate_page();
      if (!page) return NULL;
      *entry = (uintptr_t)page | 3;
    }
    table = (uint64_t *)(*entry & ADDRESS_MASK);
  }
  return &table[(address >> 12) & 511];
}
static int length_valid(uintptr_t start, size_t *length) {
  if (start % PAGE_SIZE || !*length || *length > ARENA_SIZE) return 0;
  *length = (*length + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);
  return start <= UINTPTR_MAX - *length;
}
static int containing(uintptr_t start, size_t length) {
  if (!virtual_memory_contains((void *)start) ||
      length > arena_start + ARENA_SIZE - start) return -1;
  return find_region(start, length);
}
static void invalidate(uintptr_t address) {
  if (renderer_runtime_sandboxed() && renderer_runtime_is_child()) {
    uint64_t *entry=leaf(address,0);
    uint64_t value=entry ? *entry : 0;
    int result = (int)renderer_system_call(RENDERER_SYSTEM_MAP,address,value&ADDRESS_MASK,value&(EXECUTE_DISABLE|3),0,0);
    if (result) {
      fprintf(stderr,"VM: map rejected status=%d address=%llx physical=%llx flags=%llx\n",
             result,(unsigned long long)address,(unsigned long long)(value&ADDRESS_MASK),
             (unsigned long long)(value&(EXECUTE_DISABLE|3)));
      abort();
    }
    return;
  }
  __asm__ volatile("invlpg (%0)" :: "r"(address) : "memory");
}
int virtual_memory_protect(void *address, size_t length, int protection) {
  uintptr_t start = (uintptr_t)address;
  if (!length_valid(start, &length) || containing(start, length) < 0 ||
      (protection & ~(PROT_READ | PROT_WRITE | PROT_EXEC))) {
    errno = EINVAL; return -1;
  }
  for (uintptr_t position = start; position < start + length; position += PAGE_SIZE) {
    uint64_t *entry = leaf(position, protection != PROT_NONE);
    if (!entry) {
      if (protection == PROT_NONE) continue;
      return -1;
    }
    if (!(*entry & ADDRESS_MASK) && protection != PROT_NONE) {
      void *page = allocate_page();
      if (!page) return -1;
      *entry = (uintptr_t)page;
    }
    if ((*entry & SHARED_PAGE) && ((protection & PROT_EXEC) || ((protection & PROT_WRITE) && !(*entry & SHARED_WRITABLE)))) {errno=EACCES;return -1;}
    *entry = (*entry & (ADDRESS_MASK | SHARED_PAGE | SHARED_WRITABLE)) |
        (protection == PROT_NONE ? 0 : 1) |
        ((protection & PROT_WRITE) ? 2 : 0) |
        ((protection & PROT_EXEC) ? 0 : EXECUTE_DISABLE);
    invalidate(position);
  }
  return 0;
}
static void discard_pages(uintptr_t start, size_t length) {
  for (uintptr_t position = start; position < start + length; position += PAGE_SIZE) {
    uint64_t *entry = leaf(position, 0);
    if (!entry || !(*entry & ADDRESS_MASK)) continue;
    uint64_t physical = *entry & ADDRESS_MASK;
    int shared = !!(*entry & SHARED_PAGE);
    *entry = 0;
    invalidate(position);
    if (!shared) free_page(physical);
  }
}
int virtual_memory_discard(void *address, size_t length) {
  uintptr_t start = (uintptr_t)address;
  if (!length_valid(start, &length) || containing(start, length) < 0) {
    errno = EINVAL; return -1;
  }
  for(uintptr_t position=start;position<start+length;position+=PAGE_SIZE) {
    uint64_t *entry=leaf(position,0);
    if(entry && (*entry&SHARED_PAGE)) {errno=ENOTSUP;return -1;}
  }
  /* Without a page-fault handler, readable mappings retain zeroed backing. */
  for (uintptr_t position = start; position < start + length; position += PAGE_SIZE) {
    uint64_t *entry = leaf(position, 0);
    if (!entry || !(*entry & ADDRESS_MASK)) continue;
    if (*entry & 1) {
      uint64_t physical=*entry&ADDRESS_MASK;
      if (renderer_runtime_sandboxed() && renderer_runtime_is_child() &&
          !(renderer_system_call(RENDERER_SYSTEM_PERMISSIONS,physical,0,0,0,0)&2)) {
        /* JIT backing is immutable outside a write scope, including its identity alias. */
        void *replacement=allocate_page();
        if (!replacement) return -1;
        *entry=(*entry&~ADDRESS_MASK)|(uintptr_t)replacement;
        invalidate(position);
        free_page(physical);
      } else {
        memset((void *)physical,0,PAGE_SIZE);
      }
    } else discard_pages(position, PAGE_SIZE);
  }
  return 0;
}
int virtual_memory_unmap(void *address, size_t length) {
  uintptr_t start = (uintptr_t)address;
  if (!length_valid(start, &length)) { errno = EINVAL; return -1; }
  int index = containing(start, length);
  if (index < 0) { errno = EINVAL; return -1; }
  uintptr_t end = start + length;
  int extra = -1;
  if (start > regions[index].start && end < regions[index].end) {
    extra = empty_region();
    if (extra < 0) return -1;
  }
  discard_pages(start, length);
  trim_region(index, start, end, extra);
  return 0;
}
void *virtual_memory_map(void *hint, size_t length, int protection, int flags,
                         int descriptor, off_t offset) {
  if (!length_valid(0, &length) || descriptor != -1 || offset ||
      !(flags & MAP_ANONYMOUS) || !(flags & MAP_PRIVATE) ||
      (flags & ~(MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED | MAP_NORESERVE)) ||
      (protection & ~(PROT_READ | PROT_WRITE | PROT_EXEC))) {
    errno = ENOTSUP; return MAP_FAILED;
  }
  if (initialize()) return MAP_FAILED;
  if (flags & MAP_FIXED) {
    if ((uintptr_t)hint % PAGE_SIZE || containing((uintptr_t)hint, length) < 0) {
      errno = ENOMEM; return MAP_FAILED;
    }
    discard_pages((uintptr_t)hint, length);
    return virtual_memory_protect(hint, length, protection) ? MAP_FAILED : hint;
  }
  int index = empty_region();
  if (index < 0) return MAP_FAILED;
  uintptr_t start = arena_start;
  if (virtual_memory_contains(hint) && (uintptr_t)hint % PAGE_SIZE == 0)
    start = (uintptr_t)hint;
  if (!region_gap(start, arena_start + ARENA_SIZE, length, &start)) {
    errno = ENOMEM; return MAP_FAILED;
  }
  insert_region(index, start, start + length);
  if (protection != PROT_NONE && virtual_memory_protect((void *)start, length, protection)) {
    int saved = errno;
    virtual_memory_unmap((void *)start, length);
    errno = saved;
    return MAP_FAILED;
  }
  return (void *)start;
}
static void release_tables(uint64_t *table, unsigned level) {
  if (level > 1)
    for (unsigned index = 0; index < 512; ++index)
      if (table[index] & ADDRESS_MASK)
        release_tables((uint64_t *)(table[index] & ADDRESS_MASK), level - 1);
  free_page((uintptr_t)table);
}
void *virtual_memory_alias(uintptr_t physical,size_t length,int protection,int writable) {
  void *address=virtual_memory_map(NULL,length,PROT_NONE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);
  if(address==MAP_FAILED) return address;
  for(size_t offset=0;offset<length;offset+=PAGE_SIZE) {
    uint64_t *entry=leaf((uintptr_t)address+offset,1);
    if(!entry) {virtual_memory_unmap(address,length);return MAP_FAILED;}
    *entry=(physical+offset)|SHARED_PAGE|(writable?SHARED_WRITABLE:0)|EXECUTE_DISABLE|
        (protection==PROT_NONE?0:1)|((protection&PROT_WRITE)?2:0);
    invalidate((uintptr_t)address+offset);
  }
  return address;
}
void *virtual_memory_device_alias(uintptr_t physical, size_t length, unsigned cache_type) {
  if (renderer_runtime_is_child() || !length || ((physical|length)&(PAGE_SIZE-1)) ||
      length>UINTPTR_MAX-physical) { errno=EINVAL; return MAP_FAILED; }
  uint32_t low,high;
  __asm__ volatile("rdmsr" : "=a"(low),"=d"(high) : "c"(0x277));
  uint64_t cache_flags;
  if (!x86_device_page_cache(((uint64_t)high<<32)|low,cache_type,&cache_flags)) {
    errno=ENOTSUP; return MAP_FAILED;
  }
  void *address=virtual_memory_map(NULL,length,PROT_NONE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);
  if (address==MAP_FAILED) return address;
  for (size_t offset=0;offset<length;offset+=PAGE_SIZE) {
    uint64_t *entry=leaf((uintptr_t)address+offset,1);
    if (!entry) { virtual_memory_unmap(address,length); return MAP_FAILED; }
    // Device aliases are supervisor-only and non-executable; PAT follows the host mapping.
    *entry=(physical+offset)|SHARED_PAGE|SHARED_WRITABLE|EXECUTE_DISABLE|cache_flags|3;
    invalidate((uintptr_t)address+offset);
  }
  return address;
}
void virtual_memory_release(void) {
  firmware_shared_reset();
  if (!arena_start) { release_regions(); renderer_pages_release(); return; }
  for (unsigned index = 0; index < region_capacity; ++index)
    if (regions[index].start && virtual_memory_unmap((void *)regions[index].start,
        regions[index].end - regions[index].start)) abort();
  release_regions();
  uint64_t *directory = (uint64_t *)(firmware_root[arena_index] & ADDRESS_MASK);
  if (!renderer_runtime_sandboxed() || !renderer_runtime_is_child())
    __asm__ volatile("mov %0,%%cr3" :: "r"(original_root) : "memory");
  if (directory) release_tables(directory, 3);
  free_page((uintptr_t)firmware_root);
  arena_start = 0;
  renderer_pages_release();
}

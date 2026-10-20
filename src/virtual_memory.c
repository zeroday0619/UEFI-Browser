#include "uefi.h"
#include "virtual_memory.h"
#include "renderer_runtime.h"
#include "renderer_sandbox.h"
#include "firmware_shared.h"
#include <errno.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>

/* An unused lower-half PML4 entry isolates reservations from firmware mappings. */
#define PAGE_SIZE 4096ULL
#define ARENA_SIZE (1ULL << 39)
#define ADDRESS_MASK 0x000ffffffffff000ULL
#define SHARED_PAGE (1ULL << 9)
#define SHARED_WRITABLE (1ULL << 10)
#define EXECUTE_DISABLE (1ULL << 63)
#define MAX_REGIONS 4096
extern SystemTable *platform_firmware(void);
static uint64_t *firmware_root;
static uintptr_t original_root;
static unsigned arena_index;
static uintptr_t arena_start;
static struct Region { uintptr_t start, end; } regions[MAX_REGIONS];

size_t virtual_memory_reservation_limit(void) { return ARENA_SIZE; }

/* Renderer commitments reuse 1 MiB physical batches to bound service round trips. */
typedef struct PageBatch {
  struct PageBatch *next;
  uint64_t address, used[4];
} PageBatch;
static PageBatch *page_batches;
static void *renderer_page(void) {
  PageBatch *batch=page_batches;
  for (; batch; batch=batch->next)
    for (unsigned word=0; word<4; ++word) if (batch->used[word]!=UINT64_MAX) goto found;
  batch=calloc(1,sizeof(*batch));
  if (!batch) return NULL;
  if (((Status(UEFI_API *)(uint32_t,uint32_t,size_t,uint64_t*))
       platform_firmware()->boot->function[2])(0,2,256,&batch->address)) { free(batch); return NULL; }
  batch->next=page_batches; page_batches=batch;
found:
  for (unsigned word=0; word<4; ++word) if (batch->used[word]!=UINT64_MAX) {
    unsigned bit=__builtin_ctzll(~batch->used[word]);
    batch->used[word]|=1ULL<<bit;
    void *page=(void*)(batch->address+(word*64+bit)*PAGE_SIZE);
    memset(page,0,PAGE_SIZE);
    return page;
  }
  return NULL;
}
static void renderer_page_release(uint64_t address) {
  for (PageBatch *batch=page_batches; batch; batch=batch->next) {
    if (address<batch->address || address-batch->address>=256*PAGE_SIZE) continue;
    size_t page=(address-batch->address)/PAGE_SIZE;
    uint64_t bit=1ULL<<(page%64);
    if (!(batch->used[page/64]&bit)) abort();
    batch->used[page/64]&=~bit;
    return;
  }
  abort();
}
static void renderer_pages_release(void) {
  while (page_batches) {
    PageBatch *batch=page_batches;
    for (unsigned word=0; word<4; ++word) if (batch->used[word]) abort();
    if (((Status(UEFI_API *)(uint64_t,size_t))platform_firmware()->boot->function[3])(batch->address,256)) abort();
    page_batches=batch->next;
    free(batch);
  }
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
  for (int index = 0; index < MAX_REGIONS; ++index)
    if (regions[index].start && start >= regions[index].start &&
        start + length <= regions[index].end) return index;
  return -1;
}
static int empty_region(void) {
  for (int index = 0; index < MAX_REGIONS; ++index)
    if (!regions[index].start) return index;
  errno = ENOMEM;
  return -1;
}
static void invalidate(uintptr_t address) {
  if (renderer_runtime_sandboxed() && renderer_runtime_is_child()) {
    uint64_t *entry=leaf(address,0);
    uint64_t value=entry ? *entry : 0;
    if (renderer_system_call(RENDERER_SYSTEM_MAP,address,value&ADDRESS_MASK,value&(EXECUTE_DISABLE|3),0,0)) abort();
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
    if (*entry & 1) memset((void *)(*entry & ADDRESS_MASK), 0, PAGE_SIZE);
    else discard_pages(position, PAGE_SIZE);
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
  if (extra >= 0) {
    regions[extra] = (struct Region){end, regions[index].end};
    regions[index].end = start;
  } else if (start == regions[index].start) {
    regions[index].start = end;
    if (end == regions[index].end) regions[index] = (struct Region){0, 0};
  } else regions[index].end = start;
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
  for (;;) {
    if (length > arena_start + ARENA_SIZE - start) { errno = ENOMEM; return MAP_FAILED; }
    uintptr_t next = start;
    for (int candidate = 0; candidate < MAX_REGIONS; ++candidate)
      if (regions[candidate].start && start < regions[candidate].end &&
          start + length > regions[candidate].start && regions[candidate].end > next)
        next = regions[candidate].end;
    if (next == start) break;
    start = next;
  }
  regions[index] = (struct Region){start, start + length};
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
void virtual_memory_release(void) {
  firmware_shared_reset();
  if (!arena_start) { renderer_pages_release(); return; }
  for (int index = 0; index < MAX_REGIONS; ++index)
    if (regions[index].start && virtual_memory_unmap((void *)regions[index].start,
        regions[index].end - regions[index].start)) abort();
  uint64_t *directory = (uint64_t *)(firmware_root[arena_index] & ADDRESS_MASK);
  if (!renderer_runtime_sandboxed() || !renderer_runtime_is_child())
    __asm__ volatile("mov %0,%%cr3" :: "r"(original_root) : "memory");
  if (directory) release_tables(directory, 3);
  free_page((uintptr_t)firmware_root);
  arena_start = 0;
  renderer_pages_release();
}

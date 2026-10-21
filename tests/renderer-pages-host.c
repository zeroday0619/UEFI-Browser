#include "uefi.h"
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define PAGE_SIZE 4096ULL
static BootServices boot;
static SystemTable table;
static size_t live_batches, peak_batches;
static int fail_allocation;
static SystemTable *platform_firmware(void) { return &table; }
static Status UEFI_API allocate_pages(uint32_t type, uint32_t memory,
                                      size_t pages, uint64_t *address) {
  assert(type == 0 && memory == 2 && pages == 256);
  if (fail_allocation) return ERROR(9);
  void *result = aligned_alloc(PAGE_SIZE, pages * PAGE_SIZE);
  if (!result) return ERROR(9);
  memset(result, 0xa5, pages * PAGE_SIZE);
  *address = (uintptr_t)result;
  ++live_batches;
  if (live_batches > peak_batches) peak_batches = live_batches;
  return 0;
}
static Status UEFI_API free_pages(uint64_t address, size_t pages) {
  assert(live_batches && pages == 256);
  --live_batches;
  free((void *)(uintptr_t)address);
  return 0;
}
#include "../src/renderer_pages.h"

int main(void) {
  table.boot = &boot;
  boot.function[2] = allocate_pages;
  boot.function[3] = free_pages;
  unsigned char *pages[768];
  for (unsigned cycle = 0; cycle < 8; ++cycle) {
    for (unsigned index = 0; index < 768; ++index) {
      pages[index] = renderer_page();
      assert(pages[index]);
      for (size_t offset = 0; offset < PAGE_SIZE; ++offset) assert(!pages[index][offset]);
      memset(pages[index], index % 251, PAGE_SIZE);
    }
    assert(live_batches == 3);
    fail_allocation = 1;
    assert(!renderer_page() && live_batches == 3);
    fail_allocation = 0;
    for (unsigned index = 256; index < 512; ++index) renderer_page_release((uintptr_t)pages[index]);
    assert(live_batches == 2);
    for (unsigned index = 0; index < 768; ++index) {
      if (index >= 256 && index < 512) continue;
      for (size_t offset = 0; offset < PAGE_SIZE; ++offset)
        assert(pages[index][offset] == index % 251);
      renderer_page_release((uintptr_t)pages[index]);
    }
    assert(!live_batches && !page_batches);
    renderer_pages_release();
  }
  assert(peak_batches == 3);
  puts("PASS: renderer page zeroing, partial batch lifetime, empty batch reclamation, allocation failure and repeated reuse");
}

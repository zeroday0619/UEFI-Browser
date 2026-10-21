#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <assert.h>

static int allocation_fails;
static unsigned live_allocations;
static void *test_calloc(size_t count, size_t size) {
  if (allocation_fails) return NULL;
  void *result = calloc(count, size);
  if (result) ++live_allocations;
  return result;
}
static void test_free(void *pointer) {
  if (pointer) { assert(live_allocations); --live_allocations; }
  free(pointer);
}
#define calloc test_calloc
#define free test_free
#include "../src/virtual_regions.h"
#undef calloc
#undef free

#define PAGE_BYTES 4096u
#define MODEL_PAGES 16384u
static const uintptr_t base = 0x400000000000ULL;
static unsigned owners[MODEL_PAGES];
static uint32_t random_state = 0x0619cafe;
static unsigned next_random(void) {
  random_state ^= random_state << 13;
  random_state ^= random_state >> 17;
  random_state ^= random_state << 5;
  return random_state;
}
static void validate(void) {
  unsigned occupied = 0;
  uintptr_t previous = base;
  for (unsigned position = 0; position < region_count; ++position) {
    int index = region_order[position];
    assert(regions[index].start >= previous);
    assert(regions[index].start < regions[index].end);
    previous = regions[index].end;
    for (uintptr_t address = regions[index].start; address < regions[index].end; address += PAGE_BYTES) {
      assert(owners[(address - base) / PAGE_BYTES] == (unsigned)index + 1);
      ++occupied;
    }
  }
  unsigned expected = 0, slots = 0;
  for (unsigned index = 0; index < MODEL_PAGES; ++index) expected += owners[index] != 0;
  for (unsigned index = 0; index < region_capacity; ++index) slots += regions[index].start != 0;
  assert(expected == occupied && slots == region_count);
}
static int model_gap(unsigned start, unsigned length) {
  for (unsigned position = start; position + length <= MODEL_PAGES; ++position) {
    unsigned count = 0;
    while (count < length && !owners[position + count]) ++count;
    if (count == length) return (int)position;
  }
  return -1;
}
static void erase_all(void) {
  while (region_count) erase_region(region_order[region_count - 1]);
  release_regions();
  assert(!region_capacity && !regions && !region_order && !live_allocations);
  memset(owners, 0, sizeof(owners));
}
static uint64_t microseconds(void) {
  struct timespec now;
  assert(!clock_gettime(CLOCK_MONOTONIC, &now));
  return (uint64_t)now.tv_sec * 1000000 + now.tv_nsec / 1000;
}
/* The original search is retained only as a performance comparison. */
static int baseline_gap(uintptr_t start, uintptr_t limit, size_t length, uintptr_t *result) {
  for (;;) {
    if (length > limit - start) return 0;
    uintptr_t next = start;
    for (unsigned index = 0; index < region_capacity; ++index)
      if (regions[index].start && start < regions[index].end &&
          start + length > regions[index].start && regions[index].end > next)
        next = regions[index].end;
    if (next == start) break;
    start = next;
  }
  *result = start;
  return 1;
}
int main(void) {
  uintptr_t result = 0;
  allocation_fails = 1;
  assert(empty_region() == -1 && errno == ENOMEM);
  assert(!region_count && !region_capacity && !regions && !region_order);
  allocation_fails = 0;
  assert(!region_gap(base, base + PAGE_BYTES, 0, &result));
  assert(!region_gap(UINTPTR_MAX - 1, UINTPTR_MAX, 2, &result));
  assert(region_gap(UINTPTR_MAX - 1, UINTPTR_MAX, 1, &result) && result == UINTPTR_MAX - 1);
  for (unsigned iteration = 0; iteration < 30000; ++iteration) {
    if (region_count && next_random() % 3 == 0) {
      int index = region_order[next_random() % region_count];
      unsigned first = (regions[index].start - base) / PAGE_BYTES;
      unsigned pages = (regions[index].end - regions[index].start) / PAGE_BYTES;
      unsigned start = first + next_random() % pages;
      unsigned end = start + 1 + next_random() % (first + pages - start);
      int extra = start > first && end < first + pages ? empty_region() : -1;
      if (start > first && end < first + pages && extra < 0) continue;
      trim_region(index, base + start * PAGE_BYTES, base + end * PAGE_BYTES, extra);
      for (unsigned page = start; page < end; ++page) owners[page] = 0;
      if (extra >= 0)
        for (unsigned page = end; page < first + pages; ++page) owners[page] = (unsigned)extra + 1;
    } else {
      unsigned start = next_random() % MODEL_PAGES, length = 1 + next_random() % 16;
      int expected = model_gap(start, length);
      int found = region_gap(base + start * PAGE_BYTES, base + MODEL_PAGES * PAGE_BYTES,
                             length * PAGE_BYTES, &result);
      assert(found == (expected >= 0));
      if (found) {
        assert(result == base + (unsigned)expected * PAGE_BYTES);
        int index = empty_region();
        if (index >= 0) {
          insert_region(index, result, result + length * PAGE_BYTES);
          for (unsigned page = expected; page < (unsigned)expected + length; ++page)
            owners[page] = (unsigned)index + 1;
        }
      }
    }
    unsigned page = next_random() % MODEL_PAGES, length = 1 + next_random() % 4;
    int expected = owners[page] ? (int)owners[page] - 1 : -1;
    for (unsigned offset = 0; offset < length; ++offset)
      if (page + offset >= MODEL_PAGES || owners[page + offset] != owners[page]) expected = -1;
    assert(find_region(base + page * PAGE_BYTES, length * PAGE_BYTES) == expected);
    if (!(iteration % 16)) validate();
  }
  validate();
  erase_all();
  /* Reverse insertion exercises index movement independently of slot order. */
  for (unsigned index = 0; index < INITIAL_REGIONS; ++index) {
    unsigned page = INITIAL_REGIONS - index - 1;
    insert_region(empty_region(), base + page * PAGE_BYTES, base + (page + 1) * PAGE_BYTES);
    owners[page] = index + 1;
  }
  validate();
  struct Region *original_regions = regions;
  uint32_t *original_order = region_order;
  allocation_fails = 1;
  assert(empty_region() == -1 && errno == ENOMEM);
  assert(regions == original_regions && region_order == original_order);
  assert(region_capacity == INITIAL_REGIONS && live_allocations == 1);
  validate();
  allocation_fails = 0;
  assert(!region_gap(base, base + INITIAL_REGIONS * PAGE_BYTES, PAGE_BYTES, &result));
  assert(region_gap(base, base + MODEL_PAGES * PAGE_BYTES, PAGE_BYTES, &result));
  assert(result == base + INITIAL_REGIONS * PAGE_BYTES);
  for (unsigned mode = 0; mode < 2; ++mode) {
    uint64_t started = microseconds();
    for (unsigned count = 0; count < 32; ++count) {
      __asm__ volatile("" ::: "memory");
      int found = mode ? region_gap(base, base + MODEL_PAGES * PAGE_BYTES, PAGE_BYTES, &result) :
                        baseline_gap(base, base + MODEL_PAGES * PAGE_BYTES, PAGE_BYTES, &result);
      assert(found && result == base + INITIAL_REGIONS * PAGE_BYTES);
    }
    printf("REGIONS: %s searches=32 adjacent=4096 elapsed_us=%llu\n",
           mode ? "ordered" : "baseline", (unsigned long long)(microseconds() - started));
  }
  for (unsigned page = INITIAL_REGIONS; page < 10000; ++page) {
    int index = empty_region();
    assert(index >= 0);
    insert_region(index, base + page * PAGE_BYTES, base + (page + 1) * PAGE_BYTES);
    owners[page] = (unsigned)index + 1;
  }
  assert(region_count == 10000 && region_capacity == 16384 && live_allocations == 1);
  validate();
  puts("REGIONS: dynamic growth passed mappings=10000 capacity=16384 allocation_failure_preserved=1");
  erase_region(0);
  owners[INITIAL_REGIONS - 1] = 0;
  assert(empty_region() == 0);
  assert(region_gap(base, base + MODEL_PAGES * PAGE_BYTES, PAGE_BYTES, &result));
  assert(result == base + (INITIAL_REGIONS - 1) * PAGE_BYTES);
  erase_all();
  assert(find_region(base, PAGE_BYTES) == -1);
  puts("PASS: ordered reservations, bitmap oracle, splits, trims, hints, dynamic growth, allocation failure, cleanup and overflow");
}

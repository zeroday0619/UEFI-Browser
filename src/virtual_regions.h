#ifndef UEFI_VIRTUAL_REGIONS_H
#define UEFI_VIRTUAL_REGIONS_H
#include <assert.h>
#include <errno.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define INITIAL_REGIONS 4096u
_Static_assert(sizeof(int) >= sizeof(int32_t), "Reservation indices require at least 32-bit int.");
static struct Region { uintptr_t start, end; } *regions;
/* Stable slots let protection and split operations retain their region index. */
static uint32_t *region_order;
static unsigned region_count, region_capacity;

static int grow_regions(void) {
  size_t maximum = SIZE_MAX / (sizeof(*regions) + sizeof(*region_order));
  if (maximum > INT32_MAX) maximum = INT32_MAX;
  if (region_capacity >= maximum) { errno = ENOMEM; return -1; }
  size_t capacity = region_capacity ? (size_t)region_capacity * 2 : INITIAL_REGIONS;
  if (capacity > maximum) capacity = maximum;
  struct Region *replacement = calloc(capacity, sizeof(*regions) + sizeof(*region_order));
  if (!replacement) { errno = ENOMEM; return -1; }
  uint32_t *order = (uint32_t *)(replacement + capacity);
  if (region_capacity) {
    memcpy(replacement, regions, region_capacity * sizeof(*regions));
    memcpy(order, region_order, region_count * sizeof(*region_order));
  }
  /* Publish only a complete replacement so allocation failure preserves every mapping. */
  struct Region *previous = regions;
  regions = replacement;
  region_order = order;
  region_capacity = (unsigned)capacity;
  free(previous);
  return 0;
}
static void release_regions(void) {
  assert(!region_count);
  struct Region *previous = regions;
  regions = NULL;
  region_order = NULL;
  region_capacity = 0;
  free(previous);
}

static int empty_region(void) {
  if (region_count == region_capacity && grow_regions()) return -1;
  for (unsigned index = 0; index < region_capacity; ++index)
    if (!regions[index].start) return (int)index;
  errno = ENOMEM;
  return -1;
}
static void insert_region(int index, uintptr_t start, uintptr_t end) {
  assert(index >= 0 && (unsigned)index < region_capacity && !regions[index].start);
  assert(start && start < end && region_count < region_capacity);
  unsigned position = 0;
  while (position < region_count && regions[region_order[position]].start < start)
    ++position;
  memmove(region_order + position + 1, region_order + position,
          (region_count - position) * sizeof(*region_order));
  region_order[position] = (uint32_t)index;
  regions[index] = (struct Region){start, end};
  ++region_count;
}
static void erase_region(int index) {
  unsigned position = 0;
  while (position < region_count && region_order[position] != index) ++position;
  assert(position < region_count);
  memmove(region_order + position, region_order + position + 1,
          (region_count - position - 1) * sizeof(*region_order));
  --region_count;
  regions[index] = (struct Region){0, 0};
}
static int find_region(uintptr_t start, size_t length) {
  unsigned lower = 0, upper = region_count;
  while (lower < upper) {
    unsigned middle = lower + (upper - lower) / 2;
    if (regions[region_order[middle]].start <= start) lower = middle + 1;
    else upper = middle;
  }
  if (!lower) return -1;
  int index = region_order[lower - 1];
  return start <= regions[index].end && length <= regions[index].end - start ? index : -1;
}
static void trim_region(int index, uintptr_t start, uintptr_t end, int extra) {
  if (extra >= 0) {
    uintptr_t previous_end = regions[index].end;
    regions[index].end = start;
    insert_region(extra, end, previous_end);
  } else if (start == regions[index].start) {
    if (end == regions[index].end) erase_region(index);
    else regions[index].start = end;
  } else regions[index].end = start;
}
static int region_gap(uintptr_t start, uintptr_t limit, size_t length, uintptr_t *result) {
  if (!length || start > limit || length > limit - start) return 0;
  unsigned lower = 0, upper = region_count;
  while (lower < upper) {
    unsigned middle = lower + (upper - lower) / 2;
    if (regions[region_order[middle]].end <= start) lower = middle + 1;
    else upper = middle;
  }
  /* Ordered intervals make advancing through overlapping reservations linear. */
  for (unsigned position = lower; position < region_count; ++position) {
    const struct Region *region = regions + region_order[position];
    if (start <= region->start && length <= region->start - start) break;
    start = region->end;
    if (start > limit || length > limit - start) return 0;
  }
  *result = start;
  return 1;
}
#endif

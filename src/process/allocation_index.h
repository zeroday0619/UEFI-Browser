#ifndef UEFI_BROKER_ALLOCATION_INDEX_H
#define UEFI_BROKER_ALLOCATION_INDEX_H
#include <assert.h>
#include <stdint.h>
#include <stddef.h>
#include <string.h>

#define BROKER_ALLOCATION_LIMIT 16384
typedef struct Allocation Allocation;
struct Allocation { uint64_t address; size_t backing_pages, pages, size; Allocation *next; };
/* Only the BSP mutates this index while dispatching one broker request at a time. */
static Allocation *allocation_order[BROKER_ALLOCATION_LIMIT];
static size_t allocation_count;

static size_t allocation_extent(const Allocation *entry) {
  return entry->backing_pages ? entry->backing_pages * 4096 : entry->size;
}
static size_t allocation_upper_bound(uint64_t address) {
  size_t lower=0, upper=allocation_count;
  while (lower<upper) {
    size_t middle=lower+(upper-lower)/2;
    if (allocation_order[middle]->address<=address) lower=middle+1;
    else upper=middle;
  }
  return lower;
}
static Allocation *owned_buffer(uint64_t address, size_t size) {
  if (!address || size>UINT64_MAX-address) return NULL;
  size_t position=allocation_upper_bound(address);
  if (!position) return NULL;
  Allocation *entry=allocation_order[position-1];
  uint64_t offset=address-entry->address;
  return offset<=entry->size && size<=entry->size-offset ? entry : NULL;
}
static int allocation_overlaps(uint64_t address, size_t size) {
  if (!address || size>UINT64_MAX-address) return 1;
  if (!size) return 0;
  size_t position=allocation_upper_bound(address);
  /* A gap can intersect only its preceding or following nonoverlapping extent. */
  if (position && address-allocation_order[position-1]->address<
      allocation_extent(allocation_order[position-1])) return 1;
  return position<allocation_count && allocation_order[position]->address<address+size;
}
static void index_allocation(Allocation *entry) {
  assert(allocation_count<BROKER_ALLOCATION_LIMIT);
  assert(entry->address && entry->size && entry->backing_pages<=SIZE_MAX/4096);
  size_t extent=allocation_extent(entry);
  assert(entry->size<=extent && extent<=UINT64_MAX-entry->address);
  size_t position=allocation_upper_bound(entry->address);
  assert(!position || allocation_order[position-1]->address+
         allocation_extent(allocation_order[position-1])<=entry->address);
  assert(position==allocation_count || entry->address+extent<=allocation_order[position]->address);
  memmove(allocation_order+position+1,allocation_order+position,
          (allocation_count-position)*sizeof(*allocation_order));
  allocation_order[position]=entry;
  ++allocation_count;
}
static void unindex_allocation(Allocation *entry) {
  size_t position=allocation_upper_bound(entry->address);
  assert(position && allocation_order[position-1]==entry);
  --position;
  memmove(allocation_order+position,allocation_order+position+1,
          (allocation_count-position-1)*sizeof(*allocation_order));
  allocation_order[--allocation_count]=NULL;
}
static void clear_allocation_index(void) {
  memset(allocation_order,0,sizeof(allocation_order));
  allocation_count=0;
}
#endif

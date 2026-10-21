#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include "../src/process/allocation_index.h"

static Allocation entries[BROKER_ALLOCATION_LIMIT];
static unsigned char active[BROKER_ALLOCATION_LIMIT];
static uint32_t random_state=0x0619cafe;
static unsigned next_random(void) {
  random_state^=random_state<<13;
  random_state^=random_state>>17;
  random_state^=random_state<<5;
  return random_state;
}
static Allocation *reference_owned(uint64_t address,size_t size) {
  if (!address || size>UINT64_MAX-address) return NULL;
  for (unsigned index=0;index<BROKER_ALLOCATION_LIMIT;++index) {
    Allocation *entry=entries+index;
    if (active[index] && address>=entry->address && address-entry->address<=entry->size &&
        size<=entry->size-(address-entry->address)) return entry;
  }
  return NULL;
}
static int reference_overlap(uint64_t address,size_t size) {
  if (!address || size>UINT64_MAX-address) return 1;
  if (!size) return 0;
  for (unsigned index=0;index<BROKER_ALLOCATION_LIMIT;++index) {
    Allocation *entry=entries+index;
    if (active[index] && address<entry->address+allocation_extent(entry) &&
        entry->address<address+size) return 1;
  }
  return 0;
}
static void check(uint64_t address,size_t size) {
  assert(owned_buffer(address,size)==reference_owned(address,size));
  assert(allocation_overlaps(address,size)==reference_overlap(address,size));
}
static uint64_t microseconds(void) {
  struct timespec value;
  assert(!clock_gettime(CLOCK_MONOTONIC,&value));
  return (uint64_t)value.tv_sec*1000000+value.tv_nsec/1000;
}
static unsigned classify_reference(uint64_t address,size_t size) {
  return reference_owned(address,size) ? 1 : reference_overlap(address,size) ? 2 : 0;
}
static unsigned classify_index(uint64_t address,size_t size) {
  return owned_buffer(address,size) ? 1 : allocation_overlaps(address,size) ? 2 : 0;
}
int main(void) {
  for (unsigned index=0;index<BROKER_ALLOCATION_LIMIT;++index) {
    /* Gaps and rounded backing distinguish byte ownership from physical extent. */
    entries[index]=(Allocation){.address=0x100000ULL+index*16384ULL,
      .size=index%2 ? 17 : 4096,.backing_pages=index%3 ? 1 : 0};
  }
  check(0,1); check(UINT64_MAX,2); check(UINT64_MAX-1,1);
  for (unsigned iteration=0;iteration<12000;++iteration) {
    unsigned index=next_random()%BROKER_ALLOCATION_LIMIT;
    if (active[index]) { unindex_allocation(entries+index); active[index]=0; }
    else { index_allocation(entries+index); active[index]=1; }
    uint64_t address=entries[next_random()%BROKER_ALLOCATION_LIMIT].address;
    check(address,8);
    check(address+16,2);
    check(address+4095,1);
    check(address+4096,1);
    check(address-1,2);
    check(address+8192,16384);
  }
  for (unsigned index=0;index<BROKER_ALLOCATION_LIMIT;++index)
    if (!active[index]) { index_allocation(entries+index); active[index]=1; }
  assert(allocation_count==BROKER_ALLOCATION_LIMIT);
  for (unsigned index=0;index<BROKER_ALLOCATION_LIMIT;++index) {
    assert(allocation_order[index]==entries+index);
    assert(owned_buffer(entries[index].address,entries[index].size)==entries+index);
  }
  const uint64_t outside=0x70000000;
  for (unsigned mode=0;mode<2;++mode) {
    uint64_t started=microseconds();
    unsigned classifications=0;
    for (unsigned iteration=0;iteration<4096;++iteration) {
      __asm__ volatile("" ::: "memory");
      classifications+=mode ? classify_index(outside,8) : classify_reference(outside,8);
    }
    assert(!classifications);
    printf("BROKER INDEX: %s allocations=16384 outside_queries=4096 elapsed_us=%llu\n",
           mode ? "indexed" : "linear",(unsigned long long)(microseconds()-started));
  }
  for (unsigned index=0;index<BROKER_ALLOCATION_LIMIT;++index) {
    unindex_allocation(entries+index); active[index]=0;
    assert(!owned_buffer(entries[index].address,1));
  }
  assert(!allocation_count);
  entries[0]=(Allocation){.address=UINT64_MAX-8192,.size=4096,.backing_pages=1};
  index_allocation(entries); active[0]=1;
  check(UINT64_MAX-8193,2); check(UINT64_MAX-4097,1);
  check(UINT64_MAX-4096,0); check(UINT64_MAX-4096,1);
  check(UINT64_MAX-8192,SIZE_MAX);
  clear_allocation_index(); active[0]=0;
  check(UINT64_MAX-8192,1);
  for (unsigned index=0;index<BROKER_ALLOCATION_LIMIT;++index) assert(!allocation_order[index]);
  entries[0]=(Allocation){.address=0x1000,.size=17,.backing_pages=1};
  entries[1]=(Allocation){.address=0x2000,.size=17,.backing_pages=1};
  index_allocation(entries+1); index_allocation(entries);
  active[0]=active[1]=1;
  check(0x1010,1); check(0x1010,2); check(0x1011,1);
  check(0x1fff,2); check(0x2000,1); check(0x0fff,2);
  assert(!owned_buffer(0x1000,4097) && allocation_overlaps(0x1000,4097));
  clear_allocation_index();
  puts("PASS: allocation index, byte bounds, padding, spanning ranges, deletion, reset and overflow");
}

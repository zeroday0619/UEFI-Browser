#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define RENDERER_ADDRESS_MASK 0x000ffffffffff000ULL
#define RENDERER_NX (1ULL<<63)
#define RENDERER_JIT_INDEX_CAPACITY 8
static struct { uint64_t root[512]; } renderer;
static uint64_t identities[4];
static size_t lookups;
static uint64_t *renderer_existing_leaf(uint64_t address) {
  assert(address && !(address&4095) && address<=sizeof(identities)/sizeof(*identities)*4096);
  ++lookups;
  return &identities[address/4096-1];
}
#include "../src/loader-jit.h"

static uint64_t *table(void) {
  uint64_t *value=aligned_alloc(4096,4096);
  assert(value);
  memset(value,0,4096);
  return value;
}
static void check(uint64_t *leaves,unsigned writable) {
  for (size_t index=0;index<512;++index) {
    uint64_t entry=leaves[index];
    if (!(entry&RENDERER_JIT_PAGE)) continue;
    assert(!!(entry&2)==(writable && !!(entry&RENDERER_JIT_WRITE)));
    assert(!!(entry&RENDERER_NX)==(writable || !(entry&RENDERER_JIT_EXECUTE)));
    uint64_t identity=identities[(entry&RENDERER_ADDRESS_MASK)/4096-1];
    assert(!!(identity&2)==writable);
    assert(identity&RENDERER_NX);
  }
}
static void map(uint64_t *leaves,size_t slot,uint64_t physical,uint64_t flags) {
  leaves[slot]=renderer_jit_tag(physical|flags);
  identities[physical/4096-1]=physical|5|RENDERER_NX|RENDERER_JIT_PAGE;
  if (renderer_jit_writable) identities[physical/4096-1]|=2;
  renderer_jit_mark_path((128ULL<<39)+(slot<<12));
}
int main(void) {
  uint64_t *directory=table(),*middle=table(),*leaves=table();
  renderer.root[128]=(uintptr_t)directory|7;
  directory[0]=(uintptr_t)middle|7;
  middle[0]=(uintptr_t)leaves|7;
  renderer_jit_index_reset();
  identities[0]=4096|5|RENDERER_NX|RENDERER_JIT_PAGE;
  leaves[0]=4096|7|RENDERER_NX;
  leaves[1]=4096|5|RENDERER_NX;
  renderer_jit_visit(directory,3,4096,1);
  map(leaves,2,4096,7);
  assert(renderer_jit_index_count==3 && !renderer_jit_index_fallback);
  for (unsigned iteration=0;iteration<1000;++iteration) {
    size_t before=lookups;
    assert(renderer_jit_select(1)==1); check(leaves,1);
    assert(renderer_jit_select(0)==1); check(leaves,0);
    assert(lookups==before);
  }
  /* A remapped virtual leaf must use its new physical identity. */
  map(leaves,2,8192,7);
  assert(renderer_jit_index_count==3);
  renderer_jit_select(1); check(leaves,1);
  renderer_jit_select(0); check(leaves,0);
  /* Revocation leaves allocated table slots, but ordinary reuse is not JIT. */
  leaves[0]=leaves[1]=0;
  identities[0]=4096|3|RENDERER_NX;
  leaves[0]=12288|7|RENDERER_NX;
  renderer_jit_select(1); check(leaves,1);
  renderer_jit_select(0); check(leaves,0);
  assert(leaves[0]==(12288|7|RENDERER_NX));
  assert(identities[0]==(4096|3|RENDERER_NX));
  /* Inconsistent cache metadata must fall back before returning to user mode. */
  renderer_jit_index[2].identity=&identities[0];
  renderer_jit_select(1); check(leaves,1);
  assert(renderer_jit_index_fallback);
  renderer_jit_select(0); check(leaves,0);
  /* A fresh address space must discard all pointers from the old index. */
  renderer_jit_index_reset();
  memset(leaves,0,4096);
  for (size_t slot=0;slot<RENDERER_JIT_INDEX_CAPACITY+1;++slot)
    map(leaves,slot,8192,slot&1 ? 5|RENDERER_NX : 7);
  assert(renderer_jit_index_count==RENDERER_JIT_INDEX_CAPACITY);
  assert(renderer_jit_index_fallback);
  size_t before=lookups;
  renderer_jit_select(1); check(leaves,1);
  renderer_jit_select(0); check(leaves,0);
  assert(lookups>before);
  renderer_jit_index_reset();
  renderer.root[128]=0;
  free(leaves); free(middle); free(directory);
  renderer_jit_select(1); renderer_jit_select(0);
  puts("PASS: indexed JIT aliases, remapping, revocation, ordinary reuse, mismatch fallback, capacity fallback and teardown reset");
}

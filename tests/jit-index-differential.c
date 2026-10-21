#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define RENDERER_ADDRESS_MASK 0x000ffffffffff000ULL
#define RENDERER_NX (1ULL<<63)
#define RENDERER_JIT_INDEX_CAPACITY 512
static struct { uint64_t root[512]; } renderer;
static uint64_t identities[64];
static uint64_t *renderer_existing_leaf(uint64_t address) {
  assert(address && !(address&4095) && address<=sizeof(identities)/sizeof(*identities)*4096);
  return &identities[address/4096-1];
}
#include "../src/loader-jit.h"

static uint64_t *table(void) {
  uint64_t *value=aligned_alloc(4096,4096);
  assert(value);
  memset(value,0,4096);
  return value;
}
static uint32_t random_state=0x924bac51;
static uint32_t next_random(void) {
  random_state^=random_state<<13;
  random_state^=random_state>>17;
  random_state^=random_state<<5;
  return random_state;
}
static void compare_transition(uint64_t *directory,uint64_t *leaves) {
  uint64_t previous_leaves[512],previous_identities[64];
  uint64_t indexed_leaves[512],indexed_identities[64];
  memcpy(previous_leaves,leaves,sizeof(previous_leaves));
  memcpy(previous_identities,identities,sizeof(previous_identities));
  assert(renderer_jit_select(!renderer_jit_writable));
  assert(!renderer_jit_index_fallback);
  memcpy(indexed_leaves,leaves,sizeof(indexed_leaves));
  memcpy(indexed_identities,identities,sizeof(indexed_identities));
  memcpy(leaves,previous_leaves,sizeof(previous_leaves));
  memcpy(identities,previous_identities,sizeof(previous_identities));
  /* The original marked-tree policy is the independent transition oracle. */
  renderer_jit_visit(directory,3,0,0);
  assert(!memcmp(leaves,indexed_leaves,sizeof(indexed_leaves)));
  assert(!memcmp(identities,indexed_identities,sizeof(indexed_identities)));
  for (size_t index=0;index<512;++index) {
    uint64_t entry=leaves[index];
    assert(!(entry&1) || !(entry&2) || (entry&RENDERER_NX));
  }
}
int main(void) {
  uint64_t *directory=table(),*middle=table(),*leaves=table();
  renderer.root[128]=(uintptr_t)directory|7;
  directory[0]=(uintptr_t)middle|7;
  middle[0]=(uintptr_t)leaves|7;
  for (size_t index=0;index<64;++index) identities[index]=((index+1)<<12)|7|RENDERER_NX;
  renderer_jit_index_reset();
  for (unsigned operation=0;operation<10000;++operation) {
    size_t slot=next_random()%512,backing=next_random()%64;
    uint64_t physical=(backing+1)<<12;
    unsigned action=next_random()%8;
    uint64_t previous=leaves[slot]&RENDERER_ADDRESS_MASK;
    if (!action) {
      leaves[slot]=0;
      if (previous) renderer_jit_release(previous);
    } else if (action==1) {
      /* Revoking a grant clears every alias before its backing is reused. */
      for (size_t index=0;index<512;++index)
        if ((leaves[index]&RENDERER_ADDRESS_MASK)==physical) leaves[index]=0;
      identities[backing]=physical|7|RENDERER_NX;
    } else {
      uint64_t flags=((next_random()&1) ? 5 : 4)|
                     ((next_random()&1) ? 2 : 0)|
                     ((next_random()&1) ? RENDERER_NX : 0);
      int executable=(flags&1) && !(flags&RENDERER_NX);
      if (executable || (identities[backing]&RENDERER_JIT_PAGE)) {
        if (!(identities[backing]&RENDERER_JIT_PAGE))
          renderer_jit_visit(directory,3,physical,1);
        flags=renderer_jit_tag(flags);
        identities[backing]=(identities[backing]&~2ULL)|RENDERER_NX|RENDERER_JIT_PAGE;
        if (renderer_jit_writable) identities[backing]|=2;
      }
      leaves[slot]=physical|flags;
      if (flags&RENDERER_JIT_PAGE)
        renderer_jit_mark_path((128ULL<<39)+(slot<<12));
      if (previous && previous!=physical) renderer_jit_release(previous);
    }
    compare_transition(directory,leaves);
  }
  renderer_jit_index_reset();
  renderer.root[128]=0;
  free(leaves);free(middle);free(directory);
  puts("PASS: 10000 deterministic mapping, revocation and transition operations match the original JIT permission walk");
}

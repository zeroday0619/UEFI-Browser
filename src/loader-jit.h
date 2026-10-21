/* JIT permissions and scope state belong to the supervisor, not the renderer. */
#define RENDERER_JIT_PAGE (1ULL<<9)
#define RENDERER_JIT_WRITE (1ULL<<10)
#define RENDERER_JIT_EXECUTE (1ULL<<11)
static struct { uint64_t thread; unsigned depth; } renderer_jit_threads[64];
static unsigned renderer_jit_writable;

#ifndef RENDERER_JIT_INDEX_CAPACITY
#define RENDERER_JIT_INDEX_CAPACITY 16384
#endif
_Static_assert(RENDERER_JIT_INDEX_CAPACITY &&
               !(RENDERER_JIT_INDEX_CAPACITY&(RENDERER_JIT_INDEX_CAPACITY-1)),
               "JIT index capacity must be a power of two");
static struct { uint64_t *leaf,*identity; } renderer_jit_index[RENDERER_JIT_INDEX_CAPACITY];
static uint32_t renderer_jit_index_slots[2*RENDERER_JIT_INDEX_CAPACITY];
static size_t renderer_jit_index_count;
static unsigned renderer_jit_index_fallback;

static void renderer_jit_index_reset(void) {
  renderer_jit_index_count=0;
  renderer_jit_index_fallback=0;
  for (size_t index=0;index<2*RENDERER_JIT_INDEX_CAPACITY;++index)
    renderer_jit_index_slots[index]=0;
}

static void renderer_jit_index_add(uint64_t *leaf) {
  if (renderer_jit_index_fallback) return;
  uint64_t *identity=renderer_existing_leaf(*leaf&RENDERER_ADDRESS_MASK);
  if (!identity) { renderer_jit_index_fallback=1; return; }
  uint64_t hash=(uintptr_t)leaf>>3;
  hash^=hash>>30; hash*=0xbf58476d1ce4e5b9ULL;
  hash^=hash>>27; hash*=0x94d049bb133111ebULL; hash^=hash>>31;
  size_t slot=hash&(2*RENDERER_JIT_INDEX_CAPACITY-1);
  while (renderer_jit_index_slots[slot]) {
    size_t index=renderer_jit_index_slots[slot]-1;
    if (renderer_jit_index[index].leaf==leaf) {
      /* A reused PTE can refer to different physical backing. */
      renderer_jit_index[index].identity=identity;
      return;
    }
    slot=(slot+1)&(2*RENDERER_JIT_INDEX_CAPACITY-1);
  }
  if (renderer_jit_index_count==RENDERER_JIT_INDEX_CAPACITY) {
    renderer_jit_index_fallback=1;
    return;
  }
  size_t index=renderer_jit_index_count++;
  renderer_jit_index[index].leaf=leaf;
  renderer_jit_index[index].identity=identity;
  renderer_jit_index_slots[slot]=(uint32_t)index+1;
}

static uint64_t renderer_jit_permissions(uint64_t entry) {
  entry &= ~(2ULL|RENDERER_NX);
  if (renderer_jit_writable) {
    entry |= RENDERER_NX;
    if (entry&RENDERER_JIT_WRITE) entry |= 2;
  } else if (!(entry&RENDERER_JIT_EXECUTE)) {
    entry |= RENDERER_NX;
  }
  return entry;
}

static uint64_t renderer_jit_tag(uint64_t entry) {
  uint64_t metadata=RENDERER_JIT_PAGE;
  if (entry&2) metadata|=RENDERER_JIT_WRITE;
  if (!(entry&RENDERER_NX)) metadata|=RENDERER_JIT_EXECUTE;
  return renderer_jit_permissions(entry|metadata);
}

/* Ancestor markers may remain after unmapping, but never omit a JIT leaf. */
static void renderer_jit_mark_path(uint64_t address) {
  uint64_t *table=renderer.root;
  for (unsigned shift=39;shift>12;shift-=9) {
    uint64_t *entry=table+((address>>shift)&511);
    *entry|=RENDERER_JIT_PAGE;
    table=(void*)(*entry&RENDERER_ADDRESS_MASK);
  }
  renderer_jit_index_add(table+((address>>12)&511));
}

static unsigned renderer_jit_visit(uint64_t *table,unsigned level,
                                   uint64_t physical,int register_page) {
  unsigned contains_jit=0;
  for (size_t index=0;index<512;++index) {
    uint64_t entry=table[index];
    if (!(entry&RENDERER_ADDRESS_MASK)) continue;
    if (level>1) {
      /* Registration discovers old aliases; transitions visit only JIT branches. */
      if (!register_page && !(entry&RENDERER_JIT_PAGE)) continue;
      if (renderer_jit_visit((void*)(entry&RENDERER_ADDRESS_MASK),level-1,
                             physical,register_page)) {
        table[index]|=RENDERER_JIT_PAGE;
        contains_jit=1;
      }
    } else if (register_page) {
      if ((entry&RENDERER_ADDRESS_MASK)==physical &&
          !(entry&RENDERER_JIT_PAGE))
        table[index]=renderer_jit_tag(entry);
      if ((entry&RENDERER_ADDRESS_MASK)==physical &&
          (table[index]&RENDERER_JIT_PAGE)) renderer_jit_index_add(table+index);
      contains_jit|=!!(table[index]&RENDERER_JIT_PAGE);
    } else if (entry&RENDERER_JIT_PAGE) {
      contains_jit=1;
      table[index]=renderer_jit_permissions(entry);
      uint64_t *identity=renderer_existing_leaf(entry&RENDERER_ADDRESS_MASK);
      if (identity) {
        *identity=(*identity&~2ULL)|RENDERER_NX|RENDERER_JIT_PAGE;
        if (renderer_jit_writable) *identity|=2;
      }
    }
  }
  return contains_jit;
}

static int renderer_jit_has_alias(uint64_t *table,unsigned level,uint64_t physical) {
  for (size_t index=0;index<512;++index) {
    uint64_t entry=table[index];
    if (!(entry&RENDERER_ADDRESS_MASK)) continue;
    if (level>1) {
      if (renderer_jit_has_alias((void*)(entry&RENDERER_ADDRESS_MASK),
                                 level-1,physical)) return 1;
    } else if ((entry&RENDERER_ADDRESS_MASK)==physical) return 1;
  }
  return 0;
}

static void renderer_jit_release(uint64_t physical) {
  uint64_t *identity=renderer_existing_leaf(physical);
  if (!identity || !(*identity&RENDERER_JIT_PAGE)) return;
  if ((renderer.root[128]&1) &&
      renderer_jit_has_alias((void*)(renderer.root[128]&RENDERER_ADDRESS_MASK),
                             3,physical)) return;
  *identity=physical|7|RENDERER_NX;
}

static int renderer_jit_select(unsigned writable) {
  if (renderer_jit_writable==writable) return 0;
  renderer_jit_writable=writable;
  /* Protected table pages remain allocated until address-space teardown.
     Revocation clears leaf metadata; remapping refreshes the indexed identity. */
  if (!renderer_jit_index_fallback) {
    for (size_t index=0;index<renderer_jit_index_count;++index) {
      uint64_t *leaf=renderer_jit_index[index].leaf;
      uint64_t entry=*leaf;
      if (!(entry&RENDERER_JIT_PAGE) || !(entry&RENDERER_ADDRESS_MASK)) continue;
      uint64_t *identity=renderer_jit_index[index].identity;
      if (!(*identity&RENDERER_JIT_PAGE) ||
          (*identity&RENDERER_ADDRESS_MASK)!=(entry&RENDERER_ADDRESS_MASK)) {
        renderer_jit_index_fallback=1;
        break;
      }
      *leaf=renderer_jit_permissions(entry);
      *identity=(*identity&~2ULL)|RENDERER_NX;
      if (renderer_jit_writable) *identity|=2;
    }
  }
  if (renderer_jit_index_fallback && (renderer.root[128]&1))
    renderer_jit_visit((void*)(renderer.root[128]&RENDERER_ADDRESS_MASK),3,0,0);
  return 1;
}

static unsigned renderer_jit_depth(uint64_t thread) {
  for (size_t index=0;index<64;++index)
    if (renderer_jit_threads[index].thread==thread)
      return renderer_jit_threads[index].depth;
  return 0;
}

static int renderer_jit_scope_for_thread(uint64_t thread,uint64_t enter) {
  if (enter>1) return -1;
  size_t index;
  for (index=0;index<64;++index)
    if (renderer_jit_threads[index].thread==thread) break;
  if (index==64) {
    if (!enter) return -1;
    for (index=0;index<64 && renderer_jit_threads[index].depth;++index) {}
    if (index==64) return -1;
    renderer_jit_threads[index].thread=thread;
  }
  unsigned depth=renderer_jit_threads[index].depth;
  if ((!enter && !depth) || (enter && depth==65535)) return -1;
  renderer_jit_threads[index].depth=enter ? depth+1 : depth-1;
  renderer_jit_select(renderer_jit_threads[index].depth!=0);
  return 0;
}

static int renderer_jit_scope(uint64_t enter) {
  uint32_t low,high;
  __asm__ volatile("rdmsr" : "=a"(low),"=d"(high) : "c"(0xc0000100u));
  return renderer_jit_scope_for_thread(((uint64_t)high<<32)|low,enter);
}

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>

#define RENDERER_ADDRESS_MASK 0x000ffffffffff000ULL
#define RENDERER_NX (1ULL<<63)
static struct { uint64_t root[512]; } renderer;
static uint64_t identity;
static const uint64_t physical=0x12345000;
static uint64_t *renderer_existing_leaf(uint64_t address) {
  assert(address==physical);
  return &identity;
}
#include "../src/loader-jit.h"

static uint64_t *table(void) {
  uint64_t *result=aligned_alloc(4096,4096);
  assert(result);
  memset(result,0,4096);
  return result;
}
static void check(uint64_t entry,int present,int writable,int executable) {
  assert(!!(entry&1)==present);
  assert(!!(entry&2)==writable);
  assert((!(entry&RENDERER_NX))==executable);
  assert(!((entry&2) && !(entry&RENDERER_NX)));
}
int main(void) {
  uint64_t *directory=table(),*middle=table(),*leaves=table();
  renderer.root[128]=(uintptr_t)directory|7;
  directory[0]=(uintptr_t)middle|7;
  middle[0]=(uintptr_t)leaves|7;
  identity=physical|7|RENDERER_NX|RENDERER_JIT_PAGE;
  leaves[0]=physical|7|RENDERER_NX;
  leaves[1]=physical|5|RENDERER_NX;
  leaves[2]=physical|5;
  leaves[3]=renderer_jit_tag(physical|7);
  renderer_jit_visit(directory,3,physical,1);
  assert(directory[0]&RENDERER_JIT_PAGE);
  assert(middle[0]&RENDERER_JIT_PAGE);
  uint64_t *ordinary=table();
  middle[1]=(uintptr_t)ordinary|7;
  /* A transition must not inspect a branch containing only ordinary data. */
  assert(mprotect(ordinary,4096,PROT_NONE)==0);
  check(leaves[0],1,0,0);
  check(leaves[1],1,0,0);
  check(leaves[2],1,0,1);
  check(leaves[3],1,0,1);
  assert(renderer_jit_scope_for_thread(100,0)==-1);
  assert(renderer_jit_scope_for_thread(100,2)==-1);
  assert(renderer_jit_select(0)==0);
  assert(renderer_jit_scope_for_thread(100,1)==0);
  assert(renderer_jit_select(1)==0);
  check(identity,1,1,0);
  check(leaves[0],1,1,0);
  check(leaves[1],1,0,0);
  check(leaves[2],1,0,0);
  check(leaves[3],1,1,0);
  assert(renderer_jit_scope_for_thread(100,1)==0);
  assert(renderer_jit_scope_for_thread(100,0)==0);
  assert(renderer_jit_depth(100)==1);
  check(leaves[3],1,1,0);
  assert(renderer_jit_select(renderer_jit_depth(200)!=0)==1);
  assert(renderer_jit_select(renderer_jit_depth(200)!=0)==0);
  check(leaves[3],1,0,1);
  check(identity,1,0,0);
  assert(renderer_jit_scope_for_thread(200,1)==0);
  assert(renderer_jit_scope_for_thread(200,0)==0);
  assert(renderer_jit_select(renderer_jit_depth(100)!=0)==1);
  assert(renderer_jit_select(renderer_jit_depth(100)!=0)==0);
  check(leaves[3],1,1,0);
  assert(renderer_jit_scope_for_thread(100,0)==0);
  check(leaves[3],1,0,1);
  assert(mprotect(ordinary,4096,PROT_READ|PROT_WRITE)==0);
  ordinary[0]=renderer_jit_tag(physical|7);
  renderer_jit_mark_path((128ULL<<39)+(1ULL<<21));
  assert(middle[1]&RENDERER_JIT_PAGE);
  assert(renderer_jit_scope_for_thread(100,1)==0);
  check(ordinary[0],1,1,0);
  assert(renderer_jit_scope_for_thread(100,0)==0);
  check(ordinary[0],1,0,1);
  ordinary[0]=0;
  renderer_jit_release(physical);
  assert(identity&RENDERER_JIT_PAGE);
  memset(leaves,0,4096);
  renderer_jit_release(physical);
  assert(!(identity&RENDERER_JIT_PAGE));
  check(identity,1,1,0);
  for (uint64_t thread=1;thread<200;++thread) {
    assert(renderer_jit_scope_for_thread(thread,1)==0);
    assert(renderer_jit_scope_for_thread(thread,0)==0);
  }
  for (uint64_t thread=1000;thread<1064;++thread)
    assert(renderer_jit_scope_for_thread(thread,1)==0);
  assert(renderer_jit_scope_for_thread(1064,1)==-1);
  for (uint64_t thread=1000;thread<1064;++thread)
    assert(renderer_jit_scope_for_thread(thread,0)==0);
  renderer_jit_threads[0].thread=2000;
  renderer_jit_threads[0].depth=65535;
  assert(renderer_jit_scope_for_thread(2000,1)==-1);
  assert(renderer_jit_depth(2000)==65535);
  free(ordinary);free(leaves);free(middle);free(directory);
  puts("PASS: supervisor JIT policy, alias permissions, nested scopes, thread modes and backing reuse");
}

#include "platform/runtime.h"
#include <assert.h>
#include <errno.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void *checked_malloc(size_t);
void *checked_calloc(size_t,size_t);
void *checked_realloc(void *,size_t);
void *checked_aligned_alloc(size_t,size_t);
int checked_posix_memalign(void **,size_t,size_t);
void checked_free(void *);
void runtime_release_all(void);
static int child, fail_allocation;
static long available_pages = 1024*1024;
static size_t backing_count;
int renderer_runtime_is_child(void) { return child; }
long checked_sysconf(int name) { (void)name; return available_pages; }
static Status UEFI_API allocate_pool(uint32_t type,size_t size,void **output) {
  assert(type==2);
  if (fail_allocation) return ERROR(9);
  *output=malloc(size);
  if (!*output) return ERROR(9);
  ++backing_count;
  return 0;
}
static Status UEFI_API release_pool(void *pointer) {
  assert(backing_count);
  --backing_count;
  free(pointer);
  errno=EIO;
  return 0;
}
static BootServices boot;
static SystemTable table;
static void allocation_checks(void) {
  unsigned char *memory=checked_malloc(1000);
  assert(memory && !((uintptr_t)memory&15));
  memset(memory,0x91,1000);
  size_t backing=runtime_memory_committed();
  fail_allocation=1;
  assert(checked_realloc(memory,100)==memory);
  assert(runtime_memory_used()==100 && runtime_memory_committed()==backing);
  assert(checked_realloc(memory,1000)==memory);
  errno=0;
  assert(!checked_realloc(memory,10000) && errno==ENOMEM);
  for (size_t index=0;index<1000;++index) assert(memory[index]==0x91);
  assert(runtime_memory_used()==1000);
  errno=EDOM;
  checked_free(memory);
  assert(errno==EDOM && !runtime_memory_used() && !runtime_memory_committed() && !backing_count);
  void *sentinel=(void *)0x1234;
  assert(checked_posix_memalign(&sentinel,4096,4096)==ENOMEM && sentinel==(void *)0x1234 && errno==EDOM);
  fail_allocation=0;
  for (size_t alignment=16;alignment<=65536;alignment*=2) {
    void *aligned=NULL;
    assert(!checked_posix_memalign(&aligned,alignment,77));
    assert(!((uintptr_t)aligned&(alignment-1)));
    checked_free(aligned);
  }
  assert(checked_posix_memalign(&sentinel,3,1)==EINVAL && sentinel==(void *)0x1234);
  errno=0;
  assert(!checked_calloc(SIZE_MAX,2) && errno==ENOMEM);
  errno=0;
  assert(!checked_malloc(SIZE_MAX) && errno==ENOMEM);
  assert(!checked_aligned_alloc((size_t)1<<63,SIZE_MAX));
  memory=checked_calloc(37,19);
  assert(memory);
  for (size_t index=0;index<37*19;++index) assert(!memory[index]);
  checked_free(memory);
  enum { count=1300 };
  void *blocks[count];
  for (unsigned size_class=0;size_class<7;++size_class) {
    for (unsigned index=0;index<count;++index) {
      blocks[index]=checked_malloc((size_t)32<<size_class);
      assert(blocks[index]);
      memset(blocks[index],index,(size_t)32<<size_class);
    }
    size_t full_backing=runtime_memory_committed();
    for (unsigned index=0;index<count;index+=2) checked_free(blocks[index]);
    for (unsigned index=0;index<count;index+=2) {
      blocks[index]=checked_malloc((size_t)32<<size_class);
      assert(blocks[index]);
      memset(blocks[index],index,(size_t)32<<size_class);
    }
    assert(runtime_memory_committed()==full_backing);
    for (unsigned parity=0;parity<2;++parity)
      for (unsigned index=parity;index<count;index+=2) {
        assert(*(unsigned char*)blocks[index]==(unsigned char)index);
        checked_free(blocks[index]);
      }
    assert(!backing_count && !runtime_memory_used() && !runtime_memory_committed());
  }
  for (unsigned index=0;index<count;++index) assert(checked_malloc(index+1));
  runtime_release_all();
  assert(!backing_count && !runtime_memory_used() && !runtime_memory_committed());
}
int main(void) {
  boot.function[5]=allocate_pool;
  boot.function[6]=release_pool;
  table.boot=&boot;
  for (child=0;child<=1;++child) {
    runtime_initialize(&table);
    assert(runtime_memory_limit()==1024u*1024u*1024u);
    allocation_checks();
    available_pages=16*1024;
    runtime_initialize(&table);
    assert(runtime_memory_limit()==16u*1024u*1024u);
    errno=0;
    assert(!checked_malloc(runtime_memory_limit()+1) && errno==ENOMEM);
    available_pages=LONG_MAX;
    runtime_initialize(&table);
    assert(runtime_memory_limit()==1024u*1024u*1024u);
    available_pages=1024*1024;
  }
  puts("PASS: firmware and renderer allocation, slab reclamation, alignment, overflow, errno, realloc failure and dynamic budgets");
}

#include "platform/runtime.h"
#include "renderer_runtime.h"
#include "renderer_sandbox.h"
#include "../virtual_memory.h"
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>

#define VERIFY_MEMORY(condition) do { \
  if (!(condition)) { printf("FAIL: dynamic memory line=%u\n", __LINE__); return 1; } \
} while (0)
int run_dynamic_memory_probe(void) {
  size_t before=runtime_memory_used(), backing=runtime_memory_committed();
  const size_t size=160u*1024u*1024u;
  VERIFY_MEMORY(runtime_memory_limit()>size+before);
  unsigned char *buffer=malloc(size);
  VERIFY_MEMORY(buffer && !((uintptr_t)buffer&15));
  for (size_t offset=0;offset<size;offset+=4096) buffer[offset]=(unsigned char)(offset/4096);
  buffer[size-1]=0x73;
  for (size_t offset=0;offset<size;offset+=4096)
    VERIFY_MEMORY(buffer[offset]==(unsigned char)(offset/4096));
  VERIFY_MEMORY(buffer[size-1]==0x73);
  VERIFY_MEMORY(runtime_memory_used()==before+size);
  VERIFY_MEMORY(realloc(buffer,size/2)==buffer);
  VERIFY_MEMORY(runtime_memory_used()==before+size/2);
  VERIFY_MEMORY(realloc(buffer,size)==buffer);
  errno=0;
  VERIFY_MEMORY(!realloc(buffer,SIZE_MAX) && errno==ENOMEM && buffer[size-1]==0x73);
  free(buffer);
  VERIFY_MEMORY(runtime_memory_used()==before && runtime_memory_committed()==backing);
  void *blocks[1300];
  for (unsigned index=0;index<1300;++index) {
    blocks[index]=malloc(128);
    VERIFY_MEMORY(blocks[index]);
    memset(blocks[index],index,128);
  }
  for (unsigned parity=0;parity<2;++parity)
    for (unsigned index=parity;index<1300;index+=2) {
      VERIFY_MEMORY(*(unsigned char*)blocks[index]==(unsigned char)index);
      free(blocks[index]);
    }
  VERIFY_MEMORY(runtime_memory_used()==before && runtime_memory_committed()==backing);
  if (renderer_runtime_is_child()) {
    uint64_t charged=renderer_runtime_memory_statistic(0);
    VERIFY_MEMORY(charged>0 && charged<renderer_runtime_memory_statistic(1));
    VERIFY_MEMORY(renderer_runtime_memory_statistic(2)>0);
    VERIFY_MEMORY(renderer_runtime_memory_statistic(3)==UINT64_MAX);
    VERIFY_MEMORY(renderer_runtime_request(RENDERER_MEMORY_STATISTICS,UINT64_MAX,0,0,0,0)==UINT64_MAX);
    for (unsigned index=1; index<5; ++index) {
      uint64_t arguments[5]={0};
      arguments[index]=UINT64_MAX;
      VERIFY_MEMORY(renderer_runtime_request(RENDERER_MEMORY_STATISTICS,arguments[0],arguments[1],
                    arguments[2],arguments[3],arguments[4])==UINT64_MAX);
    }
    if (renderer_runtime_sandboxed()) {
      uint64_t physical=0;
      VERIFY_MEMORY(!renderer_runtime_request(1,2,8192,(uintptr_t)&physical,0,0));
      unsigned char *aliases=mmap(NULL,8192,PROT_NONE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);
      VERIFY_MEMORY(aliases!=MAP_FAILED);
      VERIFY_MEMORY(!renderer_system_call(RENDERER_SYSTEM_MAP,(uintptr_t)aliases,
                                          physical,(1ULL<<63)|3,0,0));
      aliases[0]=0x5a;
      VERIFY_MEMORY(*(unsigned char*)(uintptr_t)physical==0x5a);
      /* Non-present mappings must also lose their physical aliases before backing is freed. */
      VERIFY_MEMORY(!renderer_system_call(RENDERER_SYSTEM_MAP,(uintptr_t)aliases+4096,
                                          physical+4096,(1ULL<<63)|2,0,0));
      VERIFY_MEMORY(!renderer_runtime_request(2,physical,0,0,0,0));
      VERIFY_MEMORY(!(renderer_system_call(RENDERER_SYSTEM_PERMISSIONS,(uintptr_t)aliases,0,0,0,0)&5));
      VERIFY_MEMORY(!(renderer_system_call(RENDERER_SYSTEM_PERMISSIONS,(uintptr_t)aliases+4096,0,0,0,0)&5));
      VERIFY_MEMORY(!(renderer_system_call(RENDERER_SYSTEM_PERMISSIONS,physical,0,0,0,0)&4));
      VERIFY_MEMORY(!munmap(aliases,8192));
      puts("MEMORY: grant revocation cleared present and non-present aliases");
      VERIFY_MEMORY(renderer_runtime_request(RENDERER_TABLE_RESERVE,0,0,0,0,0)==1);
      for (unsigned index=0; index<5; ++index) {
        uint64_t arguments[5]={0};
        arguments[index]=UINT64_MAX;
        VERIFY_MEMORY(renderer_runtime_request(RENDERER_TABLE_RESERVE,arguments[0],arguments[1],
                      arguments[2],arguments[3],arguments[4])==0);
      }
    }
    uint64_t broker_allocations[4096];
    for (unsigned index=0; index<4096; ++index) {
      broker_allocations[index]=0;
      VERIFY_MEMORY(!renderer_runtime_request(1,2,32,(uintptr_t)&broker_allocations[index],0,0));
      VERIFY_MEMORY(broker_allocations[index]);
    }
    struct timespec lookup_start,lookup_finish;
    uint64_t rejected=0x0619cafe;
    VERIFY_MEMORY(!clock_gettime(CLOCK_MONOTONIC,&lookup_start));
    for (unsigned index=0; index<2048; ++index)
      VERIFY_MEMORY(renderer_runtime_request(1,2,0,(uintptr_t)&rejected,0,0)==ERROR(2));
    VERIFY_MEMORY(!clock_gettime(CLOCK_MONOTONIC,&lookup_finish));
    VERIFY_MEMORY(rejected==0x0619cafe);
    printf("MEMORY: broker buffer lookup passed allocations=4096 requests=2048 elapsed_us=%lld\n",
           (lookup_finish.tv_sec-lookup_start.tv_sec)*1000000LL+
           (lookup_finish.tv_nsec-lookup_start.tv_nsec)/1000);
    VERIFY_MEMORY(renderer_runtime_request(9,broker_allocations[0]+31,2,0,0,0)==0);
    VERIFY_MEMORY(renderer_runtime_request(9,broker_allocations[0]+32,1,0,0,0)==0);
    VERIFY_MEMORY(renderer_runtime_request(9,broker_allocations[0]-1,2,0,0,0)==0);
    VERIFY_MEMORY(renderer_runtime_request(9,UINT64_MAX-1,4,0,0,0)==0);
    if (renderer_runtime_sandboxed()) {
      VERIFY_MEMORY(!renderer_system_call(RENDERER_SYSTEM_PROTECT,broker_allocations[0],4096,PROT_READ,0,0));
      VERIFY_MEMORY(renderer_runtime_request(9,broker_allocations[0],1,0,0,0)==0);
      VERIFY_MEMORY(!renderer_system_call(RENDERER_SYSTEM_PROTECT,broker_allocations[0],4096,PROT_NONE,0,0));
      VERIFY_MEMORY((int64_t)renderer_runtime_request(11,broker_allocations[0],1,0,0,0)==-EFAULT);
      VERIFY_MEMORY(!renderer_system_call(RENDERER_SYSTEM_PROTECT,broker_allocations[0],4096,PROT_READ|PROT_WRITE,0,0));
    }
    VERIFY_MEMORY(renderer_runtime_request(9,broker_allocations[0],32,0,0,0)==1);
    for (unsigned index=0; index<4096; ++index)
      VERIFY_MEMORY(!renderer_runtime_request(2,broker_allocations[index],0,0,0,0));
    VERIFY_MEMORY(renderer_runtime_request(9,broker_allocations[0],1,0,0,0)==0);
    puts("MEMORY: broker buffer bounds, padding, permissions and revocation passed");
    void *reservations[5000];
    struct timespec reservation_start,reservation_finish;
    VERIFY_MEMORY(!clock_gettime(CLOCK_MONOTONIC,&reservation_start));
    for (unsigned index=0; index<5000; ++index) {
      reservations[index]=mmap(NULL,4096,PROT_NONE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);
      VERIFY_MEMORY(reservations[index]!=MAP_FAILED);
      VERIFY_MEMORY(!index || (uintptr_t)reservations[index]>(uintptr_t)reservations[index-1]);
    }
    for (unsigned index=0; index<5000; index+=2) VERIFY_MEMORY(!munmap(reservations[index],4096));
    void *reused=mmap(reservations[512],4096,PROT_NONE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);
    VERIFY_MEMORY(reused==reservations[512]);
    VERIFY_MEMORY(!munmap(reused,4096));
    for (unsigned index=1; index<5000; index+=2) VERIFY_MEMORY(!munmap(reservations[index],4096));
    VERIFY_MEMORY(!clock_gettime(CLOCK_MONOTONIC,&reservation_finish));
    printf("MEMORY: reservation index passed mappings=5000 hint_reused=1 elapsed_us=%lld\n",
           (reservation_finish.tv_sec-reservation_start.tv_sec)*1000000LL+
           (reservation_finish.tv_nsec-reservation_start.tv_nsec)/1000);
    unsigned char *split=mmap(NULL,12288,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);
    VERIFY_MEMORY(split!=MAP_FAILED);
    split[0]=0x31; split[4096]=0x52; split[8192]=0x73;
    VERIFY_MEMORY(mmap(split+4096,4096,PROT_READ|PROT_WRITE,
                       MAP_PRIVATE|MAP_ANONYMOUS|MAP_FIXED,-1,0)==split+4096);
    VERIFY_MEMORY(split[0]==0x31 && split[4096]==0 && split[8192]==0x73);
    VERIFY_MEMORY(!munmap(split+4096,4096));
    errno=0;
    VERIFY_MEMORY(mprotect(split+4096,4096,PROT_READ)==-1 && errno==EINVAL);
    VERIFY_MEMORY(mmap(split+4096,4096,PROT_READ|PROT_WRITE,
                       MAP_PRIVATE|MAP_ANONYMOUS,-1,0)==split+4096);
    VERIFY_MEMORY(split[0]==0x31 && split[4096]==0 && split[8192]==0x73);
    for (unsigned index=0; index<3; ++index) VERIFY_MEMORY(!munmap(split+index*4096,4096));
    puts("MEMORY: reservation split and fixed replacement preserved neighbors");
    const size_t commitment = 64u * 1024u * 1024u;
    size_t retained = 0;
    for (unsigned cycle = 0; cycle < 4; ++cycle) {
      unsigned char *mapping = mmap(NULL, commitment, PROT_READ | PROT_WRITE,
                                   MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
      VERIFY_MEMORY(mapping != MAP_FAILED);
      VERIFY_MEMORY(virtual_memory_renderer_backing_size() >= commitment);
      for (size_t offset = 0; offset < commitment; offset += 4096) {
        VERIFY_MEMORY(mapping[offset] == 0);
        mapping[offset] = (unsigned char)(cycle + 1);
      }
      for (size_t offset = 0; offset < commitment; offset += 4096)
        VERIFY_MEMORY(mapping[offset] == cycle + 1);
      VERIFY_MEMORY(!munmap(mapping, commitment));
      size_t current = virtual_memory_renderer_backing_size();
      if (!cycle) retained = current;
      VERIFY_MEMORY(current == retained && current < commitment);
    }
    printf("MEMORY: renderer page batches reclaimed cycles=4 bytes=%zu retained=%zu\n",
           commitment, retained);
    /* Sparse commitments exhaust page-table capacity without approaching the data quota. */
    const size_t stride = 2u * 1024u * 1024u, mappings = 2304;
    unsigned char *sparse = mmap(NULL, stride * mappings, PROT_NONE,
                                MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    VERIFY_MEMORY(sparse != MAP_FAILED);
    puts("MEMORY: sparse page-table growth begin mappings=2304");
    fflush(stdout);
    for (size_t index = 0; index < mappings; ++index) {
      VERIFY_MEMORY(!mprotect(sparse + index * stride, 4096, PROT_READ | PROT_WRITE));
      sparse[index * stride] = (unsigned char)(index % 251);
    }
    for (size_t index = 0; index < mappings; ++index)
      VERIFY_MEMORY(sparse[index * stride] == index % 251);
    struct timespec started,finished;
    VERIFY_MEMORY(!clock_gettime(CLOCK_MONOTONIC,&started));
    for (unsigned cycle=0; cycle<1024; ++cycle) {
      volatile unsigned char *ordinary=malloc(8192);
      VERIFY_MEMORY(ordinary);
      ordinary[8191]=(unsigned char)cycle;
      VERIFY_MEMORY(ordinary[8191]==(unsigned char)cycle);
      free((void*)ordinary);
    }
    VERIFY_MEMORY(!clock_gettime(CLOCK_MONOTONIC,&finished));
    long long elapsed=(finished.tv_sec-started.tv_sec)*1000000LL+
        (finished.tv_nsec-started.tv_nsec)/1000;
    printf("MEMORY: identity heap cycles=1024 sparse_mappings=2304 elapsed_us=%lld\n",elapsed);
    VERIFY_MEMORY(!munmap(sparse, stride * mappings));
    puts("MEMORY: sparse page-table growth passed mappings=2304");
  }
  uint16_t selector;
  __asm__ volatile("mov %%cs,%0" : "=r"(selector));
  printf("MEMORY: dynamic allocation passed cpl=%u bytes=%zu limit=%zu reclaimed=1\n",selector&3,size,runtime_memory_limit());
  return 0;
}

#include "uefi.h"
#include "renderer_sandbox.h"
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <sys/mman.h>
#include <time.h>

extern SystemTable *platform_firmware(void);
#define JIT_NX (1ULL<<63)
static const uint64_t code_address=(128ULL<<39)+0x7e0000000ULL;
static const uint64_t alias_address=(128ULL<<39)+0x7e0001000ULL;
static uint64_t permission(uint64_t address) {
  return renderer_system_call(RENDERER_SYSTEM_PERMISSIONS,address,0,0,0,0);
}
static void *jit_other_thread(void *unused) {
  (void)unused;
  uint64_t value=permission(code_address);
  if ((value&7)!=5 || (value&JIT_NX)) return (void*)1;
  int (*function)(void)=(void*)code_address;
  return function()==73 ? NULL : (void*)2;
}
int run_jit_memory_probe(unsigned fault) {
  SystemTable *table=platform_firmware();
  uint64_t physical=0;
  if (((Status(UEFI_API *)(uint32_t,uint32_t,size_t,uint64_t*))
       table->boot->function[2])(0,2,1,&physical)) return 1;
  if (renderer_system_call(RENDERER_SYSTEM_MAP,alias_address,physical,JIT_NX|3,0,0) ||
      renderer_system_call(RENDERER_SYSTEM_MAP,code_address,physical,3,0,0)) return 2;
  if ((permission(code_address)&(JIT_NX|7))!=5 ||
      (permission(alias_address)&(JIT_NX|7))!=(JIT_NX|5) ||
      (permission(physical)&(JIT_NX|7))!=(JIT_NX|5)) return 3;
  if (renderer_system_call(RENDERER_SYSTEM_MAP,alias_address,physical,JIT_NX|3,0,0) ||
      renderer_system_call(RENDERER_SYSTEM_MAP,code_address,physical,3,0,0) ||
      (permission(alias_address)&2) || (permission(code_address)&2)) return 17;
  if (renderer_system_call(RENDERER_SYSTEM_JIT_SCOPE,0,0,0,0,0)!=(uint64_t)-1 ||
      renderer_system_call(RENDERER_SYSTEM_JIT_SCOPE,2,0,0,0,0)!=(uint64_t)-1 ||
      renderer_system_call(RENDERER_SYSTEM_PROTECT,physical,4096,3,0,0)!=(uint64_t)-1 ||
      renderer_system_call(RENDERER_SYSTEM_PROTECT,physical,4096,5,0,0)!=(uint64_t)-1)
    return 4;
  if (renderer_system_call(RENDERER_SYSTEM_JIT_SCOPE,1,0,0,0,0)) return 5;
  if ((permission(code_address)&(JIT_NX|7))!=(JIT_NX|7) ||
      (permission(alias_address)&(JIT_NX|7))!=(JIT_NX|7) ||
      (permission(physical)&(JIT_NX|7))!=(JIT_NX|7)) return 6;
  volatile unsigned char *code=(void*)alias_address;
  const unsigned char instructions[]={0xb8,73,0,0,0,0xc3};
  for (unsigned index=0;index<sizeof(instructions);++index) code[index]=instructions[index];
  if (renderer_system_call(RENDERER_SYSTEM_JIT_SCOPE,1,0,0,0,0) ||
      renderer_system_call(RENDERER_SYSTEM_JIT_SCOPE,0,0,0,0,0)) return 7;
  if ((permission(code_address)&(JIT_NX|7))!=(JIT_NX|7)) return 8;
  if (!fault) {
    for (unsigned sample=0;sample<3;++sample) {
      struct timespec started,finished;
      if (clock_gettime(CLOCK_MONOTONIC,&started)) return 26;
      for (unsigned iteration=0;iteration<100000;++iteration) {
        if (renderer_system_call(RENDERER_SYSTEM_JIT_SCOPE,1,0,0,0,0) ||
            renderer_system_call(RENDERER_SYSTEM_JIT_SCOPE,0,0,0,0,0)) return 27;
      }
      if (clock_gettime(CLOCK_MONOTONIC,&finished)) return 28;
      if ((permission(code_address)&(JIT_NX|7))!=(JIT_NX|7)) return 29;
      int64_t elapsed=(int64_t)(finished.tv_sec-started.tv_sec)*1000000000+
                      finished.tv_nsec-started.tv_nsec;
      printf("JIT PROBE: nested_pairs=100000 sample=%u elapsed_us=%lld\n",
             sample,(long long)(elapsed/1000));
    }
  }
  pthread_t thread;
  void *result;
  if (pthread_create(&thread,NULL,jit_other_thread,NULL) ||
      pthread_join(thread,&result) || result) return 9;
  if ((permission(code_address)&(JIT_NX|7))!=(JIT_NX|7)) return 10;
  int (*function)(void)=(void*)code_address;
  if (fault==2) {
    puts("JIT PROBE: execute writable page");
    fflush(stdout);
    function();
    return 11;
  }
  if (renderer_system_call(RENDERER_SYSTEM_JIT_SCOPE,0,0,0,0,0) ||
      function()!=73) return 12;
  if (fault==1 || fault==3) {
    puts(fault==1 ? "JIT PROBE: write executable page" : "JIT PROBE: write backing alias");
    fflush(stdout);
    *(volatile unsigned char*)(fault==1 ? code_address : physical)=0;
    return 13;
  }
  /* Sparse aliases exercise real mode transitions across distinct leaf tables. */
  const uint64_t sparse_start=code_address+0x40000000ULL;
  for (unsigned index=0;index<64;++index)
    if (renderer_system_call(RENDERER_SYSTEM_MAP,sparse_start+((uint64_t)index<<21),physical,3,0,0)) return 30;
  for (unsigned sample=0;sample<3;++sample) {
    struct timespec started,finished;
    if (clock_gettime(CLOCK_MONOTONIC,&started)) return 31;
    for (unsigned iteration=0;iteration<1000;++iteration) {
      if (renderer_system_call(RENDERER_SYSTEM_JIT_SCOPE,1,0,0,0,0)) return 32;
      code[1]=(unsigned char)(iteration&127);
      if (renderer_system_call(RENDERER_SYSTEM_JIT_SCOPE,0,0,0,0,0) ||
          function()!=(int)(iteration&127)) return 33;
    }
    if (clock_gettime(CLOCK_MONOTONIC,&finished)) return 34;
    int64_t elapsed=(int64_t)(finished.tv_sec-started.tv_sec)*1000000000+
                    finished.tv_nsec-started.tv_nsec;
    printf("JIT PROBE: transition_pairs=1000 aliases=66 sample=%u elapsed_us=%lld\n",
           sample,(long long)(elapsed/1000));
  }
  for (unsigned index=0;index<64;++index) {
    uint64_t address=sparse_start+((uint64_t)index<<21);
    if ((permission(address)&(JIT_NX|7))!=5 ||
        renderer_system_call(RENDERER_SYSTEM_MAP,address,0,0,0,0)) return 35;
  }
  if (renderer_system_call(RENDERER_SYSTEM_MAP,code_address,0,0,0,0) ||
      renderer_system_call(RENDERER_SYSTEM_MAP,alias_address,0,0,0,0)) return 14;
  if ((permission(physical)&(JIT_NX|7))!=(JIT_NX|7)) return 15;
  *(volatile unsigned char*)physical=0x5a;
  if (((Status(UEFI_API *)(uint64_t,size_t))table->boot->function[3])(physical,1))
    return 16;
  /* Discarding JIT storage must preserve RX permissions without writing its alias. */
  for (unsigned iteration=0;iteration<8;++iteration) {
    unsigned char *mapping=mmap(NULL,4096,PROT_READ|PROT_WRITE|PROT_EXEC,
                                MAP_PRIVATE|MAP_ANONYMOUS,-1,0);
    if (mapping==MAP_FAILED) return 18;
    if (renderer_system_call(RENDERER_SYSTEM_JIT_SCOPE,1,0,0,0,0)) return 19;
    for (unsigned index=0;index<sizeof(instructions);++index)
      mapping[index]=instructions[index];
    if (renderer_system_call(RENDERER_SYSTEM_JIT_SCOPE,0,0,0,0,0)) return 20;
    int (*mapped_function)(void)=(void*)mapping;
    if (mapped_function()!=73) return 21;
    puts("JIT PROBE: discard executable mapping");
    fflush(stdout);
    if (madvise(mapping,4096,MADV_DONTNEED)) return 22;
    if ((permission((uintptr_t)mapping)&(JIT_NX|7))!=5) return 23;
    for (unsigned index=0;index<4096;++index) if (mapping[index]) return 24;
    if (munmap(mapping,4096)) return 25;
  }
  puts("PASS: JIT discard preserves RX, clears contents and reuses backing");
  puts("PASS: JIT RX execution, RW/NX writes, alias exclusion, nested scopes, TLS switch and reuse");
  return 0;
}

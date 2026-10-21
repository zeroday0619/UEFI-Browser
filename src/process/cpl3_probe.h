/* The payload tests execute on the same entry, stack, TLS, and mappings as the renderer. */
#include "renderer_sandbox.h"
#ifdef ENABLE_BROKER_PROBE
#include "../network/resource.h"
#endif
static const RendererBootstrap *boundary_bootstrap;
#if defined(ENABLE_BLINK)
extern void renderer_scroll_wire_probe(void);
__attribute__((used,retain,naked)) void renderer_boundary_scroll_wire(void) {
  /* Fault injection redirects an arbitrary user instruction, so establish the call ABI locally. */
  __asm__ volatile("andq $-16,%rsp; subq $8,%rsp; jmp renderer_scroll_wire_probe");
}
#endif
__attribute__((used,retain,noinline)) void renderer_boundary_attack(void) {
  /* Corrupt the allowed channel header before testing forbidden supervisor access. */
  volatile uint32_t *header=boundary_bootstrap->application;
  if (header) for (unsigned index=0;index<6;++index) header[index]=UINT32_MAX;
  __asm__ volatile("mov (%0),%%rax" :: "r"(boundary_bootstrap->supervisor_address) : "rax","memory");
  __builtin_trap();
}
__attribute__((used,retain,noinline)) void renderer_boundary_attack_table(void) {
  *(volatile uint64_t*)boundary_bootstrap->root_address=0;
  __builtin_trap();
}
static _Noreturn void renderer_boundary_payload(const RendererBootstrap *bootstrap) {
  uint16_t selector;
  __asm__ volatile("mov %%cs,%0" : "=r"(selector));
  if ((selector&3)!=3 || renderer_system_call(RENDERER_SYSTEM_ABI,0,0,0,0,0)!=1 ||
      renderer_system_call(UINT64_MAX,0,0,0,0,0)!=(uint64_t)-38 ||
      renderer_system_call(RENDERER_SYSTEM_THREAD,bootstrap->supervisor_address,0,0,0,0)!=(uint64_t)-14 ||
      renderer_system_call(RENDERER_SYSTEM_THREAD,0xffff800000000000ULL,0,0,0,0)!=(uint64_t)-14)
    goto failure;
  if (renderer_system_call(RENDERER_SYSTEM_MAP,128ULL<<39,bootstrap->root_address,3,0,0)!=(uint64_t)-1 ||
      renderer_system_call(RENDERER_SYSTEM_MAP,(uintptr_t)renderer_boundary_payload,0,0,0,0)!=(uint64_t)-1 ||
      renderer_system_call(RENDERER_SYSTEM_PROTECT,bootstrap->root_address,4096,3,0,0)!=(uint64_t)-1 ||
      renderer_system_call(RENDERER_SYSTEM_BROKER,11,bootstrap->supervisor_address,0,0,0)!=(uint64_t)-14)
    goto failure;
  uint64_t thread=renderer_system_call(RENDERER_SYSTEM_THREAD,0,0,0,0,0);
  if (!thread || renderer_system_call(RENDERER_SYSTEM_THREAD,thread,0,0,0,0)!=thread) goto failure;
  uint64_t permissions=renderer_system_call(RENDERER_SYSTEM_PERMISSIONS,(uintptr_t)bootstrap,0,0,0,0);
  if ((permissions&7)!=5 || !(permissions&(1ULL<<63))) goto failure;
  if (renderer_system_call(RENDERER_SYSTEM_PERMISSIONS,bootstrap->supervisor_address,0,0,0,0)&4) goto failure;
  switch (bootstrap->boundary_test) {
    case 1: renderer_system_call(RENDERER_SYSTEM_EXIT,0,0,0,0,0); break;
    case 2: __asm__ volatile("mov (%0),%%rax" :: "r"(bootstrap->supervisor_address) : "rax","memory"); break;
    case 3: *(volatile uint64_t*)bootstrap->supervisor_address=0; break;
    case 4: *(volatile uint64_t*)bootstrap->root_address=0; break;
    case 5: *(volatile unsigned char*)(uintptr_t)renderer_boundary_payload=0; break;
    case 6: {
      volatile unsigned char code[16]={0xc3};
      __asm__ volatile("call *%0" :: "r"(code) : "memory");
      break;
    }
    case 7: __asm__ volatile("mov %%cr3,%%rax" ::: "rax"); break;
    case 8: __asm__ volatile("outb %%al,$0x80" :: "a"(0)); break;
    case 9: __asm__ volatile("mov $0x1000,%%rsp; ud2" ::: "memory"); break;
    case 10: __asm__ volatile("cli"); break;
  }
failure:
  renderer_system_call(RENDERER_SYSTEM_EXIT,1,0,0,0,0);
  for (;;) __asm__ volatile("pause");
}
static int renderer_boundary_supervisor(RendererBootstrap *bootstrap, SystemTable *table) {
  fflush(stdout);
#ifdef ENABLE_BROKER_PROBE
  if (resource_async_initialize(table)) { puts("FAIL: CPL3 broker network initialization"); return 1; }
#else
  (void)table;
#endif
  int failed=0;
  static const unsigned vectors[]={128,14,14,14,14,14,13,13,6,13,128,14,14,14};
  for (unsigned index=0;index<sizeof(vectors)/sizeof(*vectors);++index) {
    if (renderer_runtime_start(NULL)) { puts("FAIL: CPL3 renderer start"); failed=1; break; }
    Status result;
    do {
      renderer_runtime_service();
#ifdef ENABLE_BROKER_PROBE
      /* Startup probes run before the browser loop can advance network requests. */
      resource_async_process();
#endif
      result=renderer_runtime_join();
    } while (result==ERROR(6));
    const uint64_t *fault=bootstrap->fault;
    fflush(stdout);
    printf("CPL3: case=%u vector=%llu error=%llx cs=%llx address=%llx\n",index+1,
        (unsigned long long)fault[0],(unsigned long long)fault[1],
        (unsigned long long)fault[3],(unsigned long long)fault[4]);
    fflush(stdout);
    if (result!=(vectors[index]==128 ? 0 : ERROR(21)) || fault[0]!=vectors[index] || (fault[3]&3)!=3 ||
        ((index==0 || index==10) && fault[1]) || (index>=1 && index<=4 && !(fault[1]&4)) ||
        (index==5 && (fault[1]&20)!=20) ||
        ((index==11 || index==13) && fault[1]!=7) ||
        (index==12 && fault[1]!=21)) {
      puts("FAIL: CPL3 boundary result"); failed=1; break;
    }
  }
#ifdef ENABLE_BROKER_PROBE
  resource_async_shutdown(table);
#endif
  if (failed) return 1;
  fflush(stdout);
  puts("PASS: CPL3 payload entry, fixed syscall ABI, TLS validation, supervisor read/write, protected page tables, RX code, NX stack, CR3, port I/O, invalid stack and CLI containment");
  puts("PASS: CPL3 JIT executable writes, writable execution and backing-alias writes blocked");
  fflush(stdout);
  return 0;
}

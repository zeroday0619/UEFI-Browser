/* This loader-side supervisor stays outside both payload data mappings. */
#include "renderer_runtime.h"
typedef struct RendererMp RendererMp;
struct RendererMp {
  Status(UEFI_API *count)(RendererMp*, size_t*, size_t*);
  Status(UEFI_API *information)(RendererMp*, size_t, void*);
  void *start_all;
  Status(UEFI_API *start_one)(RendererMp*, void(UEFI_API *)(void*), size_t,
                            void*, size_t, void*, uint8_t*);
  void *switch_bsp, *enable;
  Status(UEFI_API *who)(RendererMp*, size_t*);
};
extern int UEFI_API browser_setjmp(void*) __attribute__((returns_twice));
extern void renderer_trap_nmi(void), renderer_trap_invalid(void), renderer_trap_page(void), renderer_trap_general(void), renderer_trap_divide(void);
uint64_t renderer_escape_context[32], renderer_fault[3];
uint64_t renderer_escape_stack;
uint32_t renderer_trap_armed, renderer_nmi_seen;
struct RendererDescriptor { uint16_t limit; uint64_t address; } __attribute__((packed));
struct RendererGate { uint16_t low, selector; uint8_t ist, flags; uint16_t middle; uint32_t high, reserved; } __attribute__((packed));
static struct {
  SystemTable *table;
  Handle image;
  const ElfHeader *header;
  const ElfProgram *programs, *tls;
  RendererMp *mp;
  RendererBootstrap bootstrap;
  void *event, *stack, *thread_allocation, *thread;
  uint64_t *root;
  uint64_t allocations[64];
  size_t allocation_pages[64], allocation_count, processor;
  uint32_t completed, released, termination, initialized;
  uint32_t apic_identifier;
  struct RendererGate *idt;
  void *escape_stack;
  Status result;
} renderer;
static void *renderer_pages(size_t count) {
  uint64_t address = 0;
  if (renderer.allocation_count == 64 ||
      ((Status(UEFI_API *)(uint32_t,uint32_t,size_t,uint64_t*))
       renderer.table->boot->function[2])(0,2,count,&address)) return 0;
  size_t index = renderer.allocation_count++;
  renderer.allocations[index] = address;
  renderer.allocation_pages[index] = count;
  for (size_t position = 0; position < count*4096; ++position)
    ((unsigned char*)address)[position] = 0;
  return (void*)address;
}
static int renderer_owned(uint64_t address) {
  for (size_t index=0; index<renderer.allocation_count; ++index)
    if (renderer.allocations[index] == address) return 1;
  return 0;
}
static int renderer_remap(uint64_t address, uint64_t physical) {
  uint64_t *table = renderer.root;
  for (unsigned shift=39; shift>12; shift-=9) {
    uint64_t *entry = table + ((address >> shift)&511);
    if (!(*entry&1)) return 0;
    if (*entry&128) {
      uint64_t *child = renderer_pages(1);
      if (!child) return 0;
      uint64_t base = *entry & 0x000ffffffffff000ULL & ~((1ULL<<shift)-1);
      uint64_t flags = *entry & ~0x000ffffffffff000ULL;
      if (shift == 21) flags &= ~128ULL;
      for (size_t index=0; index<512; ++index)
        child[index] = (base + (index << (shift-9))) | flags;
      *entry = (uintptr_t)child | 3;
    } else if (!renderer_owned(*entry & 0x000ffffffffff000ULL)) {
      uint64_t *child = renderer_pages(1);
      if (!child) return 0;
      copy(child,(void*)(*entry & 0x000ffffffffff000ULL),4096);
      *entry = (uintptr_t)child | 3;
    }
    table = (void*)(*entry & 0x000ffffffffff000ULL);
  }
  table[(address>>12)&511] = physical | 3;
  return 1;
}
static Status UEFI_API renderer_invoke(Handle image, SystemTable *table) {
  return engine(image, table, renderer.stack, &renderer.bootstrap) ? ERROR(21) : 0;
}
static void renderer_gate(unsigned vector, void (*entry)(void)) {
  uint16_t selector; __asm__ volatile("mov %%cs,%0" : "=r"(selector));
  uintptr_t address=(uintptr_t)entry;
  renderer.idt[vector]=(struct RendererGate){address,selector,0,0x8e,address>>16,address>>32,0};
}
static void UEFI_API renderer_execute(void *argument) {
  (void)argument;
  uint64_t previous_root, previous_flags;
  struct RendererDescriptor previous_idt;
  uint32_t low, high;
  __asm__ volatile("mov %%cr3,%0; pushfq; pop %1; cli; sidt %2" : "=r"(previous_root), "=r"(previous_flags), "=m"(previous_idt) :: "memory");
  copy(renderer.idt,(void*)previous_idt.address,previous_idt.limit+1);
  renderer_gate(0,renderer_trap_divide); renderer_gate(2,renderer_trap_nmi);
  renderer_gate(6,renderer_trap_invalid); renderer_gate(13,renderer_trap_general);
  renderer_gate(14,renderer_trap_page);
  struct RendererDescriptor descriptor={4095,(uintptr_t)renderer.idt};
  __asm__ volatile("rdmsr" : "=a"(low),"=d"(high) : "c"(0xc0000100u));
  uint64_t previous_thread = ((uint64_t)high<<32)|low;
  uint64_t thread = (uintptr_t)renderer.thread;
  renderer_escape_stack=((uintptr_t)renderer.escape_stack+65536-8)&~7ULL;
  __asm__ volatile("lidt %0" :: "m"(descriptor) : "memory");
  if (!browser_setjmp(renderer_escape_context)) {
    __atomic_store_n(&renderer_trap_armed,1,__ATOMIC_RELEASE);
    __atomic_store_n(&renderer.initialized,1,__ATOMIC_RELEASE);
    __asm__ volatile("mov %0,%%cr3" :: "r"((uintptr_t)renderer.root) : "memory");
    __asm__ volatile("wrmsr" :: "a"((uint32_t)thread),"d"((uint32_t)(thread>>32)),"c"(0xc0000100u) : "memory");
    renderer.result = browser_call_on_stack((unsigned char*)renderer.stack+4*1024*1024,
        renderer.image,renderer.table,renderer_invoke);
  } else renderer.result=ERROR(21);
  __atomic_store_n(&renderer_trap_armed,0,__ATOMIC_RELEASE);
  __asm__ volatile("wrmsr" :: "a"((uint32_t)previous_thread),"d"((uint32_t)(previous_thread>>32)),"c"(0xc0000100u) : "memory");
  __asm__ volatile("mov %0,%%cr3" :: "r"(previous_root) : "memory");
  __atomic_store_n(&renderer.completed,1,__ATOMIC_RELEASE);
  /* The BSP acknowledges any requested NMI before allowing the firmware IDT to return. */
  while (!__atomic_load_n(&renderer.released,__ATOMIC_ACQUIRE)) __asm__ volatile("pause");
  __asm__ volatile("lidt %0; push %1; popfq" :: "m"(previous_idt),"r"(previous_flags) : "memory");
}
static Status UEFI_API renderer_terminate(void) {
  if (!renderer.event) return ERROR(3);
  if (__atomic_load_n(&renderer.completed,__ATOMIC_ACQUIRE) || renderer.termination) return 0;
  if (!__atomic_load_n(&renderer.initialized,__ATOMIC_ACQUIRE)) return ERROR(6);
  renderer.termination=1;
  uint32_t low, high;
  __asm__ volatile("rdmsr" : "=a"(low),"=d"(high) : "c"(0x1bu));
  if (low & 1024) {
    __asm__ volatile("wrmsr" :: "c"(0x830u),"a"(0x4400u),"d"(renderer.apic_identifier) : "memory");
  } else {
    volatile uint32_t *apic=(void*)(uintptr_t)((((uint64_t)high<<32)|low)&0xfffff000ULL);
    while (apic[0x300/4]&4096) __asm__ volatile("pause");
    apic[0x310/4]=renderer.apic_identifier<<24;
    apic[0x300/4]=0x4400;
  }
  return 0;
}
static void renderer_cleanup(void) {
  while (renderer.allocation_count) {
    size_t index = --renderer.allocation_count;
    ((Status(UEFI_API *)(uint64_t,size_t))renderer.table->boot->function[3])(
        renderer.allocations[index],renderer.allocation_pages[index]);
  }
  if (renderer.event) {
    ((Status(UEFI_API *)(void*))renderer.table->boot->function[11])(renderer.event);
    renderer.event = 0;
  }
}
static Status UEFI_API renderer_start(void *channel) {
  Guid guid = {0x3fdda605,0xa76e,0x4f46,{0xad,0x29,0x12,0xf4,0x53,0x1b,0x3d,0x08}};
  if (((Status(UEFI_API *)(Guid*,void*,void**))renderer.table->boot->function[37])(
          &guid,0,(void**)&renderer.mp)) return ERROR(3);
  size_t total=0, enabled=0, current=0;
  if (renderer.mp->count(renderer.mp,&total,&enabled) || enabled<2 ||
      renderer.mp->who(renderer.mp,&current)) return ERROR(3);
  renderer.processor = (size_t)-1;
  for (size_t index=0; index<total; ++index) {
    struct { uint64_t identifier; uint32_t flags, package, core, thread; } information;
    if (index!=current && !renderer.mp->information(renderer.mp,index,&information) && (information.flags&2)) {
      renderer.processor=index; renderer.apic_identifier=information.identifier; break;
    }
  }
  if (renderer.processor==(size_t)-1) return ERROR(3);
  uint64_t current_root;
  __asm__ volatile("mov %%cr3,%0" : "=r"(current_root));
  renderer.root=renderer_pages(1);
  if (!renderer.root) goto failure;
  copy(renderer.root,(void*)(current_root&0x000ffffffffff000ULL),4096);
  for (unsigned index=0; index<renderer.header->program_count; ++index) {
    const ElfProgram *program=renderer.programs+index;
    if (program->type!=1 || !(program->flags&2)) continue;
    if (program->address&4095) goto failure;
    size_t pages=(program->memory_size+4095)/4096;
    void *data=renderer_pages(pages);
    if (!data) goto failure;
    copy(data,payload_start+program->offset,program->file_size);
    renderer.bootstrap.data_start=program->address;
    renderer.bootstrap.data_end=program->address+program->memory_size;
    renderer.bootstrap.data_physical=(uintptr_t)data;
    for (size_t page=0; page<pages; ++page)
      if (!renderer_remap(program->address+page*4096,(uintptr_t)data+page*4096)) goto failure;
  }
  renderer.completed=renderer.released=renderer.termination=renderer.initialized=0;
  renderer_trap_armed=renderer_nmi_seen=0;
  for (unsigned index=0; index<3; ++index) renderer_fault[index]=0;
  renderer.idt=renderer_pages(1); renderer.escape_stack=renderer_pages(16);
  if (!renderer.idt || !renderer.escape_stack) goto failure;
  renderer.stack=renderer_pages(1024);
  size_t tls_size=renderer.tls ? (renderer.tls->memory_size+4095)&~(size_t)4095 : 0;
  renderer.thread_allocation=renderer_pages(tls_size/4096+1);
  if (!renderer.stack || !renderer.thread_allocation) goto failure;
  renderer.thread=(unsigned char*)renderer.thread_allocation+tls_size;
  if (renderer.tls) {
    size_t alignment=renderer.tls->alignment ? renderer.tls->alignment : 1;
    size_t length=(renderer.tls->memory_size+alignment-1)&~(alignment-1);
    copy((unsigned char*)renderer.thread-length,payload_start+renderer.tls->offset,renderer.tls->file_size);
  }
  *(void**)renderer.thread=renderer.thread;
  renderer.bootstrap.renderer=1;
  renderer.bootstrap.channel=channel;
  if (((Status(UEFI_API *)(uint32_t,size_t,void*,void*,void**))renderer.table->boot->function[7])(
      0,0,0,0,&renderer.event)) goto failure;
  Status status=renderer.mp->start_one(renderer.mp,renderer_execute,renderer.processor,renderer.event,0,0,0);
  if (status) goto failure;
  return 0;
failure:
  renderer_cleanup();
  return ERROR(9);
}
static Status UEFI_API renderer_join(void) {
  if (!__atomic_load_n(&renderer.completed,__ATOMIC_ACQUIRE)) return ERROR(6);
  if (renderer.termination && !__atomic_load_n(&renderer_nmi_seen,__ATOMIC_ACQUIRE)) return ERROR(6);
  __atomic_store_n(&renderer.released,1,__ATOMIC_RELEASE);
  Status status=((Status(UEFI_API *)(void*))renderer.table->boot->function[12])(renderer.event);
  /* CheckEvent must observe MP dispatch completion before AP stacks are freed. */
  if (status) return status;
  Status result=renderer.result;
  renderer_cleanup();
  return result;
}

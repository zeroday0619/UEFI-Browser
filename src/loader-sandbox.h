/* Supervisor state is linked into the loader rather than either payload mapping. */
#include "renderer_sandbox.h"
#include "x86_page_cache.h"
#define RENDERER_ADDRESS_MASK 0x000ffffffffff000ULL
#define RENDERER_NX (1ULL<<63)
struct RendererTask {
  uint32_t reserved; uint64_t stack[3], reserved2, interrupt_stack[7], reserved3;
  uint16_t reserved4, bitmap;
} __attribute__((packed));
_Static_assert(sizeof(struct RendererTask)==104,"x86-64 TSS layout");
_Static_assert(sizeof(struct RendererGate)==16,"x86-64 interrupt gate layout");
extern void UEFI_API renderer_enter_user(uint64_t,uint64_t,uint64_t,uint64_t,void*,void*);
extern void renderer_system_interrupt(void);
extern void *renderer_exception_table[32];
static unsigned renderer_boundary_sequence;
static uint64_t renderer_supervisor_canary=0xc13a56b90d74ef82ULL;
extern void renderer_escape(void);
static uint64_t renderer_permissions(uint64_t address) {
  if (address >= (1ULL<<47)) return 0;
  const uint64_t *table=renderer.root;
  uint64_t writable=2, user=4, execute=0;
  for (unsigned shift=39;;shift-=9) {
    uint64_t entry=table[(address>>shift)&511];
    if (!(entry&1)) return 0;
    writable&=entry; user&=entry; execute|=entry;
    if (shift==12 || (entry&128)) return 1|writable|user|(execute&RENDERER_NX);
    table=(void*)(entry&RENDERER_ADDRESS_MASK);
  }
}
static int renderer_user_buffer(uint64_t address, uint64_t size, int writable) {
  if (!address || !size || address >= (1ULL<<47) || size>(1ULL<<47)-address) return 0;
  for (uint64_t page=address&~4095ULL;page<address+size;page+=4096) {
    uint64_t permissions=renderer_permissions(page);
    if ((permissions&5)!=5 || (writable && !(permissions&2))) return 0;
  }
  return 1;
}
static int renderer_user_map(uint64_t address,uint64_t physical,size_t pages,uint64_t flags) {
  if (((address|physical)&4095) || address >= (1ULL<<47) || pages>((1ULL<<47)-address)/4096) return 0;
  for (size_t page=0;page<pages;++page,address+=4096,physical+=4096) {
    uint64_t *table=renderer.root;
    for (unsigned shift=39;shift>12;shift-=9) {
      uint64_t *entry=table+((address>>shift)&511);
      if (!(*entry&1) || (*entry&128) || !renderer_owned(*entry&RENDERER_ADDRESS_MASK)) {
        uint64_t *child=renderer_new_table();
        if (!child) return 0;
        if (*entry&128) {
          uint64_t base=(*entry&RENDERER_ADDRESS_MASK)&~((1ULL<<shift)-1);
          uint64_t inherited=(*entry&~RENDERER_ADDRESS_MASK)&~4ULL;
          if (shift==21) inherited&=~128ULL;
          for (size_t index=0;index<512;++index) child[index]=(base+(index<<(shift-9)))|inherited;
        } else if (*entry&1) {
          const uint64_t *source=(void*)(*entry&RENDERER_ADDRESS_MASK);
          for (size_t index=0;index<512;++index) child[index]=source[index]&~4ULL;
        }
        *entry=(uintptr_t)child|7;
      } else *entry=(*entry|7)&~RENDERER_NX;
      table=(void*)(*entry&RENDERER_ADDRESS_MASK);
    }
    table[(address>>12)&511]=physical|flags|5;
  }
  return 1;
}
struct RendererGrant { uint64_t address,size; unsigned permissions,may_have_aliases; };
static struct RendererGrant renderer_grants[16384];
static size_t renderer_grant_count;
static struct RendererGrant *renderer_find_grant(uint64_t address,uint64_t size) {
  for (size_t index=0;index<renderer_grant_count;++index) {
    struct RendererGrant *grant=renderer_grants+index;
    if (address>=grant->address && address-grant->address<=grant->size &&
        size<=grant->size-(address-grant->address)) return grant;
  }
  return 0;
}
static int renderer_granted_buffer(uint64_t address,size_t size,unsigned writable) {
  struct RendererGrant *grant=renderer_find_grant(address,size);
  return grant && (!writable || (grant->permissions&2)) &&
      renderer_user_buffer(address, size, writable);
}
static uint64_t *renderer_existing_leaf(uint64_t address) {
  uint64_t *table=renderer.root;
  for (unsigned shift=39;shift>12;shift-=9) {
    uint64_t entry=table[(address>>shift)&511];
    if (!(entry&1) || (entry&128)) return 0;
    table=(void*)(entry&RENDERER_ADDRESS_MASK);
  }
  return table+((address>>12)&511);
}
#include "loader-jit.h"
static void renderer_revoke_aliases(uint64_t *table,unsigned level,uint64_t address,uint64_t size) {
  for (size_t index=0;index<512;++index) {
    uint64_t entry=table[index];
    if (!(entry&RENDERER_ADDRESS_MASK)) continue;
    if (level>1) renderer_revoke_aliases((void*)(entry&RENDERER_ADDRESS_MASK),level-1,address,size);
    else if ((entry&RENDERER_ADDRESS_MASK)>=address && (entry&RENDERER_ADDRESS_MASK)-address<size) table[index]=0;
  }
}
static int UEFI_API renderer_grant(uint64_t address,size_t size,unsigned permissions) {
  if (!permissions && !renderer.event) return 1;
  if (!renderer.root || !size || ((address|size)&4095) || size>UINT64_MAX-address || (permissions&~59u) || ((permissions&8) && (!(permissions&1) || !(permissions>>4))) || (!(permissions&8) && (permissions>>4))) return 0;
  if (!permissions) {
    struct RendererGrant *grant=renderer_find_grant(address,size);
    if (!grant || grant->address!=address || grant->size!=size) return 0;
    for (size_t offset=0;offset<size;offset+=4096) {
      uint64_t *entry=renderer_existing_leaf(address+offset);
      if (!entry) return 0;
      *entry=(address+offset)|(*entry&0x98ULL)|3|RENDERER_NX;
    }
    if (__atomic_load_n(&grant->may_have_aliases,__ATOMIC_ACQUIRE) && (renderer.root[128]&1))
      renderer_revoke_aliases((void*)(renderer.root[128]&RENDERER_ADDRESS_MASK),3,address,size);
    *grant=renderer_grants[--renderer_grant_count];
    return 1;
  }
  if (renderer_grant_count==16384 || renderer_find_grant(address,size)) return 0;
  /* An identity grant can cross each page-table level's boundary. */
  if (!renderer_reserve_tables(6+(size>>21)+(size>>30)+(size>>39))) return 0;
  uint64_t cache_flags=0;
  if (permissions&8) {
    uint32_t low,high;
    __asm__ volatile("rdmsr" : "=a"(low),"=d"(high) : "c"(0x277));
    if (!x86_device_page_cache(((uint64_t)high<<32)|low,permissions>>4,&cache_flags)) return 0;
  }
  if (!renderer_user_map(address,address,size/4096,RENDERER_NX|(permissions&2)|cache_flags)) {
    /* A failed grant cannot leave user aliases to pages returned to firmware. */
    for (size_t offset=0;offset<size;offset+=4096) {
      uint64_t *entry=renderer_existing_leaf(address+offset);
      if (entry && (*entry&RENDERER_ADDRESS_MASK)==address+offset) *entry=(address+offset)|(*entry&0x98ULL)|3|RENDERER_NX;
    }
    return 0;
  }
  renderer_grants[renderer_grant_count++]=(struct RendererGrant){address,size,permissions,0};
  return 1;
}
/* Executable arena pages use supervisor-managed, alias-wide JIT permissions. */
static int renderer_map_owned(uint64_t address,uint64_t physical,uint64_t flags) {
  if (address<(128ULL<<39) || address>=(129ULL<<39) || (address&4095)) return -1;
  uint64_t *entry=renderer_existing_leaf(address);
  uint64_t previous=entry ? *entry&RENDERER_ADDRESS_MASK : 0;
  if (!physical) {
    if (entry) *entry=0;
    if (previous) renderer_jit_release(previous);
    return 0;
  }
  struct RendererGrant *grant=renderer_find_grant(physical,4096);
  if (!grant || (grant->permissions&8) || (physical&4095) || (flags&~(RENDERER_NX|3ULL)) ||
      ((flags&2) && !(grant->permissions&2))) return -1;
  uint64_t *identity=renderer_existing_leaf(physical);
  if (!identity) return -1;
  int executable=(flags&1) && !(flags&RENDERER_NX);
  if (executable && !(grant->permissions&2)) return -1;
  /* A monotonic supervisor marker avoids scanning the arena for identity-only heaps.
     Set it before publication or a BSP reserve wait and retain it after unmap or failure. */
  __atomic_store_n(&grant->may_have_aliases,1,__ATOMIC_RELEASE);
  if (renderer.page_table_capacity-renderer.page_table_count<3) {
    /* The AP waits while the BSP replenishes supervisor-only table backing. */
    RendererRuntimeChannel *channel=renderer.bootstrap.channel;
    channel->operation=RENDERER_TABLE_RESERVE;
    for (size_t index=0; index<5; ++index) channel->arguments[index]=0;
    __atomic_store_n(&channel->pending,1,__ATOMIC_RELEASE);
    while (__atomic_load_n(&channel->pending,__ATOMIC_ACQUIRE)) __asm__ volatile("pause");
    if (!channel->result || renderer.page_table_capacity-renderer.page_table_count<3) return -12;
  }
  if (executable || (*identity&RENDERER_JIT_PAGE)) {
    if (!(*identity&RENDERER_JIT_PAGE) && (renderer.root[128]&1))
      renderer_jit_visit((void*)(renderer.root[128]&RENDERER_ADDRESS_MASK),3,physical,1);
    flags=renderer_jit_tag(flags);
    *identity=(*identity&~2ULL)|RENDERER_NX|RENDERER_JIT_PAGE;
    if (renderer_jit_writable) *identity|=2;
  }
  if (!renderer_user_map(address,physical,1,flags)) return -12;
  if (flags&RENDERER_JIT_PAGE) renderer_jit_mark_path(address);
  entry=renderer_existing_leaf(address);
  if (!(flags&1)) *entry&=~1ULL;
  if (previous && previous!=physical) renderer_jit_release(previous);
  return 0;
}
static int renderer_sandbox_prepare(void) {
  renderer_grant_count=0;
  renderer_jit_index_reset();
  renderer_jit_writable=0;
  for (size_t index=0;index<64;++index) {
    renderer_jit_threads[index].thread=0;
    renderer_jit_threads[index].depth=0;
  }
  /* The child owns its sparse arena independently of the BSP reservation tree. */
  renderer.root[128]=0;
  /* All inherited mappings lose user access before any explicit grants are installed. */
  for (size_t index=0;index<512;++index) renderer.root[index]&=~4ULL;
  for (unsigned index=0;index<renderer.header->program_count;++index) {
    const ElfProgram *program=renderer.programs+index;
    if (program->type!=1 || !program->memory_size) continue;
    if ((program->address&4095) || (program->flags&3)==3) return 0;
    uint64_t physical=(program->flags&2) ? renderer.bootstrap.data_physical : program->address;
    uint64_t flags=((program->flags&2)?2:0)|((program->flags&1)?0:RENDERER_NX);
    if (!renderer_user_map(program->address,physical,(program->memory_size+4095)/4096,flags)) return 0;
  }
  size_t tls_size=renderer.tls ? (renderer.tls->memory_size+4095)&~(size_t)4095 : 0;
  if (!renderer_user_map((uintptr_t)renderer.stack,(uintptr_t)renderer.stack,1024,2|RENDERER_NX) ||
      !renderer_user_map((uintptr_t)renderer.thread_allocation,(uintptr_t)renderer.thread_allocation,
          tls_size/4096+1,2|RENDERER_NX)) return 0;
  renderer.gdt=renderer_pages(16); renderer.task=renderer_pages(1);
  renderer.kernel_stack=renderer_pages(16); renderer.exception_stack=renderer_pages(16);
  renderer.user_bootstrap=renderer_pages(1);
  if (!renderer.gdt || !renderer.task || !renderer.kernel_stack || !renderer.exception_stack || !renderer.user_bootstrap) return 0;
  RendererBootstrap *user=renderer.user_bootstrap;
  *user=renderer.bootstrap;
  user->sandbox=1;
  user->channel=0;
  user->reserve_tables=0;
  user->media=0;
  const RendererRuntimeChannel *channel=renderer.bootstrap.channel;
  user->application=channel->application;
  if (channel->application && (!channel->application_size ||
      !renderer_grant((uintptr_t)channel->application,(channel->application_size+4095)&~4095ULL,3))) return 0;
  user->supervisor_address=(uintptr_t)&renderer_supervisor_canary;
  user->root_address=(uintptr_t)renderer.root;
#ifdef ENABLE_CPL3_PROBE
  user->boundary_test=++renderer_boundary_sequence;
#endif
  return renderer_user_map((uintptr_t)user,(uintptr_t)user,1,RENDERER_NX);
}
static int renderer_sandbox_descriptors(const struct RendererDescriptor *previous,
    uint16_t previous_task,uint16_t previous_ss) {
  size_t index=((size_t)previous->limit+8)/8;
  if (index>8187 || (previous_task&4)) return 0;
  uint64_t *gdt=renderer.gdt;
  copy(gdt,(void*)previous->address,previous->limit+1);
  gdt[index]=0x00affa000000ffffULL; gdt[index+1]=0x00cff2000000ffffULL;
  uintptr_t task_address=(uintptr_t)renderer.task;
  gdt[index+2]=103|((task_address&0xffffff)<<16)|(0x89ULL<<40)|((task_address&0xff000000)<<32);
  gdt[index+3]=task_address>>32;
  renderer.user_code=index*8+3; renderer.user_data=(index+1)*8+3;
  struct RendererTask *task=renderer.task;
  task->stack[0]=(uintptr_t)renderer.kernel_stack+16*4096;
  task->interrupt_stack[0]=(uintptr_t)renderer.exception_stack+8*4096;
  task->interrupt_stack[1]=(uintptr_t)renderer.exception_stack+16*4096;
  task->bitmap=sizeof(*task);
  uint16_t code; __asm__ volatile("mov %%cs,%0" : "=r"(code));
  renderer_supervisor_cs=code; renderer_supervisor_ss=previous_ss;
  /* Dedicated exception stacks remain usable after an invalid user RSP or an NMI in the gate. */
  for (unsigned vector=0;vector<32;++vector) {
    uintptr_t entry=(uintptr_t)renderer_exception_table[vector];
    renderer.idt[vector]=(struct RendererGate){entry,code,vector==2?1:2,vector==3?0xee:0x8e,entry>>16,entry>>32,0};
  }
  uintptr_t entry=(uintptr_t)renderer_system_interrupt;
  renderer.idt[128]=(struct RendererGate){entry,code,0,0xee,entry>>16,entry>>32,0};
  struct RendererDescriptor descriptor={(index+4)*8-1,(uintptr_t)gdt};
  uint16_t selector=(index+2)*8;
  __asm__ volatile("lgdt %0; ltr %1" :: "m"(descriptor),"r"(selector) : "memory");
  return 1;
}
/* This dispatcher has no TLS, allocator, function-pointer request, or payload callback. */
void UEFI_API renderer_system_dispatch(uint64_t *frame) {
  uint64_t operation=frame[14],first=frame[9];
  uint64_t result=(uint64_t)-38;
  if ((frame[16]&3)!=3) { frame[14]=(uint64_t)-1; return; }
  /* The fixed-memory decoder service has no firmware, allocation or JIT authority. */
  if (renderer.bootstrap.renderer == 2 && operation != RENDERER_SYSTEM_ABI &&
      operation != RENDERER_SYSTEM_EXIT && operation != RENDERER_SYSTEM_PERMISSIONS) {
    frame[14]=(uint64_t)-1;
    return;
  }
  switch (operation) {
    case RENDERER_SYSTEM_ABI: result=1; break;
    case RENDERER_SYSTEM_THREAD: {
      uint32_t low,high;
      __asm__ volatile("rdmsr" : "=a"(low),"=d"(high) : "c"(0xc0000100u));
      result=((uint64_t)high<<32)|low;
      if (first) {
        if (!renderer_user_buffer(first,4096,1)) { result=(uint64_t)-14; break; }
        if (renderer_jit_select(renderer_jit_depth(first)!=0))
          __asm__ volatile("mov %0,%%cr3" :: "r"((uintptr_t)renderer.root) : "memory");
        __asm__ volatile("wrmsr" :: "a"((uint32_t)first),"d"((uint32_t)(first>>32)),"c"(0xc0000100u) : "memory");
      }
      break;
    }
    case RENDERER_SYSTEM_PERMISSIONS:
      result=renderer_permissions(first);
      if (!(result&4)) result=0;
      break;
    case RENDERER_SYSTEM_JIT_SCOPE: {
      unsigned previous=renderer_jit_writable;
      result=renderer_jit_scope(first);
      /* Nested scopes change depth without modifying any page-table permission. */
      if (previous!=renderer_jit_writable)
        __asm__ volatile("mov %0,%%cr3" :: "r"((uintptr_t)renderer.root) : "memory");
      break;
    }
    case RENDERER_SYSTEM_MAP:
      result=renderer_map_owned(first,frame[10],frame[11]);
      __asm__ volatile("mov %0,%%cr3" :: "r"((uintptr_t)renderer.root) : "memory");
      break;
    case RENDERER_SYSTEM_PROTECT: {
      uint64_t size=frame[10],protection=frame[11];
      struct RendererGrant *grant=renderer_find_grant(first,size);
      int private_data=first>=renderer.bootstrap.data_start && first<renderer.bootstrap.data_end &&
          size<=((renderer.bootstrap.data_end+4095)&~4095ULL)-first;
      if (!size || ((first|size)&4095) || protection>7 || (protection&4) ||
          (!private_data && !grant) || ((protection&2) && grant && !(grant->permissions&2))) { result=(uint64_t)-1; break; }
      result=0;
      for (uint64_t offset=0;offset<size;offset+=4096) {
        uint64_t *entry=renderer_existing_leaf(first+offset);
        if (!entry || ((*entry&RENDERER_JIT_PAGE) && (protection&2) &&
                       !renderer_jit_writable)) { result=(uint64_t)-1; break; }
      }
      if (result) break;
      for (uint64_t offset=0;offset<size;offset+=4096) {
        uint64_t *entry=renderer_existing_leaf(first+offset);
        if (!entry) { result=(uint64_t)-1; break; }
        *entry=(*entry&(RENDERER_ADDRESS_MASK|RENDERER_JIT_PAGE|0x98ULL))|4|(protection?1:0)|((protection&2)?2:0)|RENDERER_NX;
      }
      __asm__ volatile("mov %0,%%cr3" :: "r"((uintptr_t)renderer.root) : "memory");
      break;
    }
    case RENDERER_SYSTEM_BROKER: {
      if (first>255 || !renderer_user_buffer(frame[10],5*sizeof(uint64_t),0)) { result=(uint64_t)-14; break; }
      RendererRuntimeChannel *channel=renderer.bootstrap.channel;
      channel->operation=(unsigned)first;
      copy(channel->arguments,(const void*)frame[10],sizeof(channel->arguments));
      if (!channel->private_verified) __atomic_store_n(&channel->private_verified,1,__ATOMIC_RELEASE);
      __atomic_store_n(&channel->pending,1,__ATOMIC_RELEASE);
      while (__atomic_load_n(&channel->pending,__ATOMIC_ACQUIRE)) __asm__ volatile("pause");
      result=channel->result;
      __asm__ volatile("mov %0,%%cr3" :: "r"((uintptr_t)renderer.root) : "memory");
      break;
    }
    case RENDERER_SYSTEM_EXIT:
      renderer_fault[0]=128; renderer_fault[1]=first;
      renderer_fault[2]=frame[15]; renderer_fault[3]=frame[16]; renderer_fault[4]=0;
      renderer_trap_armed=0;
      frame[15]=(uintptr_t)renderer_escape; frame[16]=renderer_supervisor_cs;
      frame[18]=renderer_escape_stack; frame[19]=renderer_supervisor_ss; frame[17]=2;
      break;
    default: break;
  }
  frame[14]=result;
  /* User flags cannot enable interrupts, I/O privileges, nested tasks, or alignment faults in the supervisor. */
  frame[17]=(frame[17]&0xcd5)|2;
}

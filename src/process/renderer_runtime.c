#include "renderer_runtime.h"
#include "renderer_sandbox.h"
#include "firmware_audio.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <errno.h>
#include <sys/mman.h>

typedef RendererRuntimeChannel RuntimeChannel;
static RendererBootstrap *bootstrap;
static RuntimeChannel *channel;
static SystemTable *firmware;
static SystemTable proxy;
static BootServices boot;
static struct { TableHeader header; void *function[14]; } runtime;
static uint64_t requests, operations[256];
typedef struct Allocation Allocation;
struct Allocation { uint64_t address; size_t backing_pages, pages, size; Allocation *next; };
static Allocation *allocations;
static size_t allocation_count, allocation_bytes;
static size_t application_size;
#define BROKER_MEMORY_LIMIT (2ULL * 1024 * 1024 * 1024)
#define BROKER_ALLOCATION_LIMIT 16384
static Allocation *owned_buffer(uint64_t address, size_t size) {
  if (!address || size > UINT64_MAX-address) return NULL;
  for (Allocation *entry=allocations; entry; entry=entry->next)
    if (address>=entry->address && address-entry->address<=entry->size &&
        size<=entry->size-(address-entry->address)) return entry;
  return NULL;
}
int renderer_runtime_buffer(uint64_t address, size_t size, unsigned writable) {
  if (!size) return 1;
  if (renderer_runtime_is_child() || !address || size>UINT64_MAX-address ||
      (!owned_buffer(address,size) &&
       (!bootstrap || !bootstrap->buffer || !bootstrap->buffer(address,size,writable)))) return 0;
  /* Ownership alone does not make a PROT_NONE mapping safe for the BSP to access. */
  uint64_t root;
  __asm__ volatile("mov %%cr3,%0" : "=r"(root));
  for (uint64_t position=address&~4095ULL; position<address+size; position+=4096) {
    const uint64_t *table=(void*)(root&0x000ffffffffff000ULL);
    for (unsigned shift=39; ; shift-=9) {
      uint64_t entry=table[(position>>shift)&511];
      if (!(entry&1) || (writable && !(entry&2))) return 0;
      if (shift==12 || (entry&128)) break;
      table=(void*)(entry&0x000ffffffffff000ULL);
    }
  }
  return 1;
}
int renderer_runtime_string(char *output, size_t capacity, uint64_t address) {
  if (!address || capacity>UINT64_MAX-address) return 0;
  for (size_t index=0; index<capacity;) {
    size_t count=4096-((address+index)&4095);
    if (count>capacity-index) count=capacity-index;
    while (count && !renderer_runtime_buffer(address+index,count,0)) count/=2;
    if (!count) return 0;
    memcpy(output+index,(const void*)(address+index),count);
    if (memchr(output+index,0,count)) return 1;
    index+=count;
  }
  return 0;
}
static void release_allocations(void) {
  size_t reclaimed=0;
  while (allocations) {
    Allocation *entry=allocations; allocations=entry->next;
    if (entry->backing_pages) {
      mprotect((void*)entry->address,entry->backing_pages*4096,PROT_READ|PROT_WRITE|PROT_EXEC);
      ((Status(UEFI_API *)(uint64_t,size_t))firmware->boot->function[3])(entry->address,entry->backing_pages);
    } else ((Status(UEFI_API *)(void*))firmware->boot->function[6])((void*)entry->address);
    free(entry); ++reclaimed;
  }
  printf("RENDERER: reclaimed broker allocations=%zu tracked=%zu after AP quiescence\n",reclaimed,allocation_count);
  allocation_count=allocation_bytes=0;
}

static volatile unsigned private_data_probe=17;
static _Thread_local unsigned private_tls_probe=19;
extern void graphics_log(const char *);
extern long firmware_serial_write(long, const char *, size_t);
extern _Bool firmware_entropy(unsigned char *, size_t);
extern int mprotect(void *, size_t, int);
enum { ALLOCATE_POOL=1, FREE_POOL, ALLOCATE_PAGES, FREE_PAGES, MEMORY_MAP,
       GET_TIME, GET_VARIABLE, SET_VARIABLE, ENTROPY, PROTECT, LOG, STALL, MEMORY_PAGES=RENDERER_MEMORY_PAGES };
void renderer_runtime_initialize(RendererBootstrap *value) {
  bootstrap=value;
  if (value && value->renderer) {
    channel=value->channel;
    if (private_data_probe!=17 || private_tls_probe!=19) __builtin_trap();
    private_data_probe=59; private_tls_probe=61;
    if (channel) __atomic_store_n(&channel->private_verified,1,__ATOMIC_RELEASE);
  }
}
int renderer_runtime_sandboxed(void) { return bootstrap && bootstrap->sandbox; }
int renderer_runtime_is_child(void) { return bootstrap && bootstrap->renderer; }
int renderer_runtime_available(void) { return channel && !renderer_runtime_is_child(); }
void *renderer_runtime_channel(void) { return renderer_runtime_is_child() && renderer_runtime_sandboxed() ? bootstrap->application : channel ? channel->application : NULL; }
/* Private image data uses different physical backing at the same virtual address. */
static uint64_t shared(const void *pointer) {
  uint64_t address=(uintptr_t)pointer;
  if (bootstrap && address>=bootstrap->data_start && address<bootstrap->data_end)
    address=bootstrap->data_physical+address-bootstrap->data_start;
  return address;
}
uint64_t renderer_runtime_request(unsigned operation, uint64_t first, uint64_t second,
                                  uint64_t third, uint64_t fourth, uint64_t fifth) {
  if (renderer_runtime_is_child() && renderer_runtime_sandboxed()) {
    uint64_t arguments[5]={first,second,third,fourth,fifth};
    return renderer_system_call(RENDERER_SYSTEM_BROKER,operation,(uintptr_t)arguments,0,0,0);
  }
  if (!renderer_runtime_is_child() || !channel) abort();
  channel->operation=operation;
  channel->arguments[0]=first; channel->arguments[1]=second; channel->arguments[2]=third;
  channel->arguments[3]=fourth; channel->arguments[4]=fifth;
  __atomic_store_n(&channel->pending,1,__ATOMIC_RELEASE);
  while (__atomic_load_n(&channel->pending,__ATOMIC_ACQUIRE)) __asm__ volatile("pause");
  return channel->result;
}
void renderer_runtime_application_size(size_t size) { application_size=size; }
void *renderer_runtime_shared_allocate(size_t size) {
  uint64_t address=0;
  if (!firmware || !size || size>128*1024*1024 ||
      ((Status(UEFI_API *)(uint32_t,uint32_t,size_t,uint64_t*))firmware->boot->function[2])(0,2,(size+4095)/4096,&address)) return NULL;
  memset((void*)address,0,(size+4095)&~4095ULL);
  return (void*)address;
}
void renderer_runtime_shared_free(void *address,size_t size) {
  if (address) ((Status(UEFI_API *)(uint64_t,size_t))firmware->boot->function[3])((uintptr_t)address,(size+4095)/4096);
}
void *renderer_runtime_export_allocate(size_t size) {
  void *address=renderer_runtime_shared_allocate(size);
  if (!address) return NULL;
  if (!bootstrap->grant || !bootstrap->grant((uintptr_t)address,(size+4095)&~4095ULL,1)) {
    renderer_runtime_shared_free(address,size); return NULL;
  }
  return address;
}
void renderer_runtime_export_free(void *address,size_t size) {
  if (!address) return;
  if (bootstrap->grant) bootstrap->grant((uintptr_t)address,(size+4095)&~4095ULL,0);
  renderer_runtime_shared_free(address,size);
}
Status renderer_runtime_start(void *application) {
  if (!bootstrap || !bootstrap->start || channel) return ERROR(3);
  channel=calloc(1,sizeof(*channel));
  if (!channel) return ERROR(9);
  channel->application=application; channel->application_size=application_size;
  private_data_probe=41; private_tls_probe=43;
  Status result=bootstrap->start(channel);
  if (result) { free(channel); channel=NULL; }
  return result;
}
Status renderer_runtime_terminate(void) {
  if (!channel || !bootstrap || !bootstrap->terminate) return ERROR(3);
  return bootstrap->terminate();
}
__attribute__((__weak__)) void renderer_network_reset(void) {}
Status renderer_runtime_join(void) {
  if (!channel || !bootstrap || !bootstrap->join) return ERROR(3);
  Status result=bootstrap->join();
  if (result!=ERROR(6)) {
    if (result) printf("RENDERER: stopped status=%llx vector=%llu error=%llx rip=%llx cs=%llx address=%llx\n",
        (unsigned long long)result,(unsigned long long)bootstrap->fault[0],
        (unsigned long long)bootstrap->fault[1],(unsigned long long)bootstrap->fault[2],
        (unsigned long long)bootstrap->fault[3],(unsigned long long)bootstrap->fault[4]);
    renderer_network_reset();
    firmware_audio_reset(1);
    release_allocations();
    printf("RENDERER: runtime service requests=%llu\n",(unsigned long long)requests);
    for (unsigned index=1; index<256; ++index) if (operations[index])
      printf("RENDERER: service operation=%u requests=%llu\n",index,(unsigned long long)operations[index]);
    free(channel); channel=NULL;
  }
  return result;
}
static Status UEFI_API allocate_pool(uint32_t type, size_t size, void **output) {
  return renderer_runtime_request(ALLOCATE_POOL,type,size,shared(output),0,0);
}
static Status UEFI_API free_pool(void *pointer) { return renderer_runtime_request(FREE_POOL,shared(pointer),0,0,0,0); }
static Status UEFI_API allocate_pages(uint32_t type, uint32_t memory, size_t count, uint64_t *output) {
  return renderer_runtime_request(ALLOCATE_PAGES,type,memory,count,shared(output),0);
}
static Status UEFI_API free_pages(uint64_t address, size_t count) { return renderer_runtime_request(FREE_PAGES,address,count,0,0,0); }
static Status UEFI_API memory_map(size_t *size, void *buffer, size_t *key, size_t *stride, uint32_t *version) {
  return renderer_runtime_request(MEMORY_MAP,shared(size),shared(buffer),shared(key),shared(stride),shared(version));
}
static Status UEFI_API get_time(void *time, void *capabilities) { return renderer_runtime_request(GET_TIME,shared(time),shared(capabilities),0,0,0); }
static Status UEFI_API get_variable(Character *name, Guid *guid, uint32_t *attributes, size_t *size, void *data) {
  return renderer_runtime_request(GET_VARIABLE,shared(name),shared(guid),shared(attributes),shared(size),shared(data));
}
static Status UEFI_API set_variable(Character *name, Guid *guid, uint32_t attributes, size_t size, void *data) {
  return renderer_runtime_request(SET_VARIABLE,shared(name),shared(guid),attributes,size,shared(data));
}
uint64_t renderer_runtime_frequency(void) {
  static uint64_t frequency;
  if (!frequency) frequency=renderer_runtime_request(STALL,0,0,0,0,0);
  return frequency;
}
static Status UEFI_API stall(size_t microseconds) {
  /* Sleeping in the service would block browser input for the renderer's delay. */
  uint64_t frequency=renderer_runtime_frequency();
  uint32_t low, high;
  __asm__ volatile("rdtsc" : "=a"(low),"=d"(high));
  uint64_t start=((uint64_t)high<<32)|low;
  uint64_t duration=frequency/1000000*microseconds;
  do {
    __asm__ volatile("pause; rdtsc" : "=a"(low),"=d"(high));
  } while ((((uint64_t)high<<32)|low)-start<duration);
  return 0;
}
static Status UEFI_API unsupported(void) { return ERROR(3); }
SystemTable *renderer_runtime_table(SystemTable *table) {
  if (!renderer_runtime_is_child()) { firmware=table; return table; }
  /* The compatibility table exposes no firmware configuration tables or protocol handles. */
  if (table) { proxy.header=table->header; proxy.revision=table->revision; boot.header=table->boot->header; }
  for (size_t index=0; index<44; ++index) boot.function[index]=unsupported;
  boot.function[2]=allocate_pages; boot.function[3]=free_pages;
  boot.function[4]=memory_map; boot.function[5]=allocate_pool; boot.function[6]=free_pool;
  boot.function[28]=stall;
  for (size_t index=0; index<14; ++index) runtime.function[index]=unsupported;
  runtime.function[0]=get_time; runtime.function[6]=get_variable; runtime.function[8]=set_variable;
  proxy.boot=&boot; proxy.runtime=&runtime;
  proxy.input=NULL; proxy.output=NULL; proxy.error=NULL;
  return &proxy;
}
int renderer_runtime_entropy(void *buffer, size_t size) {
  /* Firmware runs with the BSP page table and cannot write renderer-only virtual mappings. */
  unsigned char bytes[256];
  unsigned char *destination=buffer;
  while (size) {
    size_t count=size<sizeof(bytes) ? size : sizeof(bytes);
    if (!renderer_runtime_request(ENTROPY,shared(bytes),count,0,0,0)) return 0;
    memcpy(destination,bytes,count);
    destination+=count; size-=count;
  }
  return 1;
}
int renderer_runtime_protect(void *buffer, size_t size, int protection) {
  uintptr_t address=(uintptr_t)buffer;
  if (renderer_runtime_sandboxed()) {
    int result=(int)renderer_system_call(RENDERER_SYSTEM_PROTECT,address,size,protection,0,0);
    if (result) errno=EACCES;
    return result;
  }
  if (address>=bootstrap->data_start && address<bootstrap->data_end) {
    if (size>((bootstrap->data_end+4095)&~4095ULL)-address) { errno=EINVAL; return -1; }
    uintptr_t root; __asm__ volatile("mov %%cr3,%0" : "=r"(root));
    for (uintptr_t position=address; position<address+size; position+=4096) {
      uint64_t *table=(void*)(root&0x000ffffffffff000ULL);
      for (unsigned shift=39; shift>12; shift-=9)
        table=(void*)(table[(position>>shift)&511]&0x000ffffffffff000ULL);
      uint64_t *entry=&table[(position>>12)&511];
      *entry=(*entry&0x000ffffffffff000ULL) | (protection ? 1 : 0) |
          ((protection&PROT_WRITE) ? 2 : 0) | ((protection&PROT_EXEC) ? 0 : (1ULL<<63));
      __asm__ volatile("invlpg (%0)" :: "r"(position) : "memory");
    }
    return 0;
  }
  int result=(int)renderer_runtime_request(PROTECT,address,size,protection,0,0);
  for (uintptr_t position=address; position<address+size; position+=4096)
    __asm__ volatile("invlpg (%0)" :: "r"(position) : "memory");
  if (result) errno=ENOMEM;
  return result;
}
void renderer_runtime_write(const void *data, size_t length) { renderer_runtime_request(LOG,shared(data),length,0,0,0); }
void renderer_runtime_log(const char *text) { renderer_runtime_write(text,strlen(text)); }
__attribute__((__weak__)) uint64_t renderer_network_service(unsigned operation, uint64_t first,
    uint64_t second, uint64_t third, uint64_t fourth, uint64_t fifth) {
  (void)operation; (void)first; (void)second; (void)third; (void)fourth; (void)fifth;
  return ERROR(3);
}
#include "renderer_broker.h"
static uint64_t broker_dispatch(unsigned operation, const uint64_t arguments[5]) {
  uint64_t address=arguments[0];
  switch (operation) {
    case RENDERER_AUDIO_REQUEST: return broker_audio(arguments);
    case RENDERER_DNS_REQUEST: return broker_dns(arguments[0],arguments[1],arguments[2]);
    case RENDERER_FILE_REQUEST: return broker_file(arguments[0],arguments[1]);
    case RENDERER_SOCKET_REQUEST: return broker_socket(arguments[0],arguments[1]);
    case ALLOCATE_POOL: case ALLOCATE_PAGES: {
      unsigned pages=operation==ALLOCATE_PAGES;
      size_t count=arguments[pages ? 2 : 1];
      uint64_t output=arguments[pages ? 3 : 2];
      if (!renderer_runtime_buffer(output,sizeof(uint64_t),1)) return ERROR(15);
      if (!count || (pages && (arguments[0]!=0 || (arguments[1]!=1 && arguments[1]!=2))) ||
          (!pages && arguments[0]!=2)) return ERROR(2);
      if (allocation_count>=BROKER_ALLOCATION_LIMIT || count>BROKER_MEMORY_LIMIT/(pages ? 4096 : 1)) return ERROR(9);
      size_t size=count*(pages ? 4096 : 1);
      if (allocation_bytes>BROKER_MEMORY_LIMIT-size) return ERROR(9);
      Allocation *entry=calloc(1,sizeof(*entry));
      if (!entry) return ERROR(9);
      uint64_t allocated=0;
      size_t backing_pages=renderer_runtime_sandboxed() ? (size+4095)/4096 : (pages ? count : 0);
      Status result=backing_pages ?
          ((Status(UEFI_API *)(uint32_t,uint32_t,size_t,uint64_t*))firmware->boot->function[2])(
              0,2,backing_pages,&allocated) :
          ((Status(UEFI_API *)(uint32_t,size_t,void**))firmware->boot->function[5])(2,size,(void**)&allocated);
      if (result) { free(entry); return result; }
      if (renderer_runtime_sandboxed()) {
        memset((void*)allocated,0,backing_pages*4096);
        if (!bootstrap->grant || !bootstrap->grant(allocated,backing_pages*4096,3)) {
          ((Status(UEFI_API *)(uint64_t,size_t))firmware->boot->function[3])(allocated,backing_pages);
          free(entry); return ERROR(9);
        }
      }
      entry->backing_pages=backing_pages;
      entry->address=allocated; entry->pages=pages ? count : 0; entry->size=size;
      entry->next=allocations; allocations=entry; ++allocation_count; allocation_bytes+=size;
      memcpy((void*)output,&allocated,sizeof(allocated));
      return 0;
    }
    case FREE_POOL: case FREE_PAGES: {
      Allocation **position=&allocations;
      while (*position && (*position)->address!=address) position=&(*position)->next;
      Allocation *entry=*position;
      if (!entry || (operation==FREE_POOL ? entry->pages!=0 :
          (!entry->pages || entry->pages!=arguments[1]))) return ERROR(2);
      /* Firmware allocators may inspect or poison released pages. */
      if (entry->pages && !renderer_runtime_buffer(address,entry->size,1)) return ERROR(15);
      if (renderer_runtime_sandboxed() && !bootstrap->grant(address,entry->backing_pages*4096,0)) return ERROR(15);
      Status result=entry->backing_pages ?
          ((Status(UEFI_API *)(uint64_t,size_t))firmware->boot->function[3])(address,entry->backing_pages) :
          ((Status(UEFI_API *)(void*))firmware->boot->function[6])((void*)address);
      if (!result) { *position=entry->next; --allocation_count; allocation_bytes-=entry->size; free(entry); }
      return result;
    }
    case MEMORY_MAP: return ERROR(15);
    case MEMORY_PAGES: return sysconf(address ? _SC_AVPHYS_PAGES : _SC_PHYS_PAGES);
    case GET_VARIABLE: case SET_VARIABLE: return broker_variable(operation,arguments);
    case GET_TIME: {
      unsigned char time[16]={0}, capabilities[12]={0};
      if (!renderer_runtime_buffer(address,sizeof(time),1) ||
          (arguments[1] && !renderer_runtime_buffer(arguments[1],sizeof(capabilities),1))) return ERROR(15);
      void **services=(void**)((unsigned char*)firmware->runtime+sizeof(TableHeader));
      Status result=((Status(UEFI_API *)(void*,void*))services[0])(time,capabilities);
      if (!result) {
        memcpy((void*)address,time,sizeof(time));
        if (arguments[1]) memcpy((void*)arguments[1],capabilities,sizeof(capabilities));
      }
      return result;
    }
    case ENTROPY: {
      unsigned char bytes[256];
      if (arguments[1]>sizeof(bytes) || !renderer_runtime_buffer(address,arguments[1],1)) return 0;
      if (!firmware_entropy(bytes,arguments[1])) return 0;
      if (arguments[1]) memcpy((void*)address,bytes,arguments[1]);
      return 1;
    }
    case PROTECT: {
      if (renderer_runtime_sandboxed()) return (uint64_t)-1;
      Allocation *entry=owned_buffer(address,arguments[1]);
      if (!entry || !entry->pages || !arguments[1] || ((address|arguments[1])&4095) ||
          arguments[2]>7 || (arguments[2]&(PROT_WRITE|PROT_EXEC))==(PROT_WRITE|PROT_EXEC)) return (uint64_t)-1;
      return mprotect((void*)address,arguments[1],arguments[2]);
    }
    case LOG: {
      if (arguments[1]>65536 || !renderer_runtime_buffer(address,arguments[1],0)) return (uint64_t)-EFAULT;
      char bytes[512];
      for (size_t offset=0; offset<arguments[1];) {
        size_t count=arguments[1]-offset;
        if (count>sizeof(bytes)) count=sizeof(bytes);
        memcpy(bytes,(const void*)(address+offset),count);
        firmware_serial_write(1,bytes,count); offset+=count;
      }
      return arguments[1];
    }
    case STALL: {
      static uint64_t frequency;
      if (!frequency) {
        uint32_t low, high;
        __asm__ volatile("rdtsc" : "=a"(low),"=d"(high));
        uint64_t start=((uint64_t)high<<32)|low;
        ((Status(UEFI_API *)(size_t))firmware->boot->function[28])(10000);
        __asm__ volatile("rdtsc" : "=a"(low),"=d"(high));
        frequency=(((((uint64_t)high<<32)|low)-start))*100;
      }
      return frequency;
    }
    default: return renderer_network_service(operation,arguments[0],arguments[1],arguments[2],arguments[3],arguments[4]);
  }
}
void renderer_runtime_service(void) {
  if (!renderer_runtime_available() || !__atomic_load_n(&channel->pending,__ATOMIC_ACQUIRE)) return;
  if (private_data_probe!=41 || private_tls_probe!=43) abort();
  if (__atomic_load_n(&channel->private_verified,__ATOMIC_ACQUIRE)==1) {
    uintptr_t root, thread;
    __asm__ volatile("mov %%cr3,%0; mov %%fs:0,%1" : "=r"(root),"=r"(thread));
    printf("RENDERER: private data and TLS verified; BSP cr3=%llx tls=%llx\n",
        (unsigned long long)root,(unsigned long long)thread);
    __atomic_store_n(&channel->private_verified,2,__ATOMIC_RELEASE);
  }
  /* Dispatch never rereads a shared operation or pointer after validating it. */
  unsigned operation=channel->operation;
  uint64_t arguments[5];
  memcpy(arguments,channel->arguments,sizeof(arguments));
  channel->result=broker_dispatch(operation,arguments);
  ++requests;
  if (operation<256) ++operations[operation];
  __atomic_store_n(&channel->pending,0,__ATOMIC_RELEASE);
}

#include "renderer_runtime.h"
#include "firmware_audio.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <errno.h>
#include <sys/mman.h>

typedef struct {
  uint32_t pending;
  unsigned operation;
  uint64_t arguments[5], result;
  void *application;
  uint32_t private_verified;
} RuntimeChannel;
static RendererBootstrap *bootstrap;
static RuntimeChannel *channel;
static SystemTable *firmware;
static SystemTable proxy;
static BootServices boot;
static struct { TableHeader header; void *function[14]; } runtime;
static uint64_t requests, operations[256];
typedef struct Allocation Allocation;
struct Allocation { uint64_t address; size_t pages; Allocation *next; };
static Allocation *allocations;
static size_t allocation_count;
static void release_allocations(void) {
  size_t reclaimed=0;
  while (allocations) {
    Allocation *entry=allocations; allocations=entry->next;
    if (entry->pages) {
      mprotect((void*)entry->address,entry->pages*4096,PROT_READ|PROT_WRITE|PROT_EXEC);
      ((Status(UEFI_API *)(uint64_t,size_t))firmware->boot->function[3])(entry->address,entry->pages);
    } else ((Status(UEFI_API *)(void*))firmware->boot->function[6])((void*)entry->address);
    free(entry); ++reclaimed;
  }
  printf("RENDERER: reclaimed broker allocations=%zu tracked=%zu after AP quiescence\n",reclaimed,allocation_count);
  allocation_count=0;
}

static volatile unsigned private_data_probe=17;
static _Thread_local unsigned private_tls_probe=19;
extern void graphics_log(const char *);
extern long firmware_serial_write(long, const char *, size_t);
extern _Bool firmware_entropy(unsigned char *, size_t);
extern int mprotect(void *, size_t, int);
enum { ALLOCATE_POOL=1, FREE_POOL, ALLOCATE_PAGES, FREE_PAGES, MEMORY_MAP,
       GET_TIME, GET_VARIABLE, SET_VARIABLE, ENTROPY, PROTECT, LOG, STALL };
void renderer_runtime_initialize(RendererBootstrap *value) {
  bootstrap=value;
  if (value && value->renderer) {
    channel=value->channel;
    if (private_data_probe!=17 || private_tls_probe!=19) __builtin_trap();
    private_data_probe=59; private_tls_probe=61;
    __atomic_store_n(&channel->private_verified,1,__ATOMIC_RELEASE);
  }
}
int renderer_runtime_is_child(void) { return bootstrap && bootstrap->renderer; }
int renderer_runtime_available(void) { return channel && !renderer_runtime_is_child(); }
void *renderer_runtime_channel(void) { return channel ? channel->application : NULL; }
/* Private image data uses different physical backing at the same virtual address. */
static uint64_t shared(const void *pointer) {
  uint64_t address=(uintptr_t)pointer;
  if (bootstrap && address>=bootstrap->data_start && address<bootstrap->data_end)
    address=bootstrap->data_physical+address-bootstrap->data_start;
  return address;
}
uint64_t renderer_runtime_request(unsigned operation, uint64_t first, uint64_t second,
                                  uint64_t third, uint64_t fourth, uint64_t fifth) {
  if (!renderer_runtime_is_child() || !channel) abort();
  channel->operation=operation;
  channel->arguments[0]=first; channel->arguments[1]=second; channel->arguments[2]=third;
  channel->arguments[3]=fourth; channel->arguments[4]=fifth;
  __atomic_store_n(&channel->pending,1,__ATOMIC_RELEASE);
  while (__atomic_load_n(&channel->pending,__ATOMIC_ACQUIRE)) __asm__ volatile("pause");
  return channel->result;
}
Status renderer_runtime_start(void *application) {
  if (!bootstrap || !bootstrap->start || channel) return ERROR(3);
  channel=calloc(1,sizeof(*channel));
  if (!channel) return ERROR(9);
  channel->application=application;
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
    if (result) printf("RENDERER: stopped status=%llx vector=%llu error=%llx rip=%llx\n",
        (unsigned long long)result,(unsigned long long)bootstrap->fault[0],
        (unsigned long long)bootstrap->fault[1],(unsigned long long)bootstrap->fault[2]);
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
  firmware=table;
  if (!renderer_runtime_is_child()) return table;
  proxy=*table;
  boot=*table->boot;
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
#include "firmware_io.h"
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
  uint64_t *a=channel->arguments, result=ERROR(3);
  typedef uint64_t (UEFI_API *Call)(uint64_t,uint64_t,uint64_t,uint64_t,uint64_t);
  void *function=NULL;
  void **services=(void**)((unsigned char*)firmware->runtime+sizeof(TableHeader));
  switch (channel->operation) {
    case RENDERER_AUDIO_REQUEST: result=firmware_audio_service(1,a[0],a[1],a[2],a[3],a[4]); break;
    case RENDERER_DNS_REQUEST: result=firmware_dns_service(1,a[0],a[1],(void *)a[2]); break;
    case RENDERER_FILE_REQUEST: result=firmware_file_service(1,a[0],(const long *)a[1]); break;
    case RENDERER_SOCKET_REQUEST: result=firmware_socket_service(1,a[0],(const long *)a[1]); break;
    case ALLOCATE_POOL: function=firmware->boot->function[5]; break;
    case FREE_POOL: function=firmware->boot->function[6]; break;
    case ALLOCATE_PAGES: function=firmware->boot->function[2]; break;
    case FREE_PAGES: function=firmware->boot->function[3]; break;
    case MEMORY_MAP: function=firmware->boot->function[4]; break;
    case GET_TIME: function=services[0]; break;
    case GET_VARIABLE: function=services[6]; break;
    case SET_VARIABLE: function=services[8]; break;
    case ENTROPY: result=firmware_entropy((void*)a[0],a[1]); break;
    case PROTECT: result=mprotect((void*)a[0],a[1],a[2]); break;
    case LOG: result=firmware_serial_write(1,(const char*)a[0],a[1]); break;
    case STALL: {
      static uint64_t frequency;
      if (!frequency) {
        uint32_t low, high;
        __asm__ volatile("rdtsc" : "=a"(low),"=d"(high));
        uint64_t start=((uint64_t)high<<32)|low;
        ((Status(UEFI_API *)(size_t))firmware->boot->function[28])(10000);
        __asm__ volatile("rdtsc" : "=a"(low),"=d"(high));
        frequency=((((uint64_t)high<<32)|low)-start)*100;
      }
      result=frequency; break;
    }
    default: result=renderer_network_service(channel->operation,a[0],a[1],a[2],a[3],a[4]); break;
  }
  Allocation *entry=NULL, **position=NULL;
  if (channel->operation==ALLOCATE_POOL || channel->operation==ALLOCATE_PAGES) {
    entry=calloc(1,sizeof(*entry));
    if (!entry) { function=NULL; result=ERROR(9); }
  }
  if (channel->operation==FREE_POOL || channel->operation==FREE_PAGES) {
    position=&allocations;
    while (*position && (*position)->address!=a[0]) position=&(*position)->next;
    if (!*position || (channel->operation==FREE_PAGES && (*position)->pages!=a[1])) {
      function=NULL; result=ERROR(2);
    }
  }
  if (function) result=((Call)function)(a[0],a[1],a[2],a[3],a[4]);
  if (entry) {
    if (result) free(entry);
    else {
      entry->address=*(uint64_t*)(uintptr_t)a[channel->operation==ALLOCATE_POOL ? 2 : 3];
      entry->pages=channel->operation==ALLOCATE_PAGES ? a[2] : 0;
      entry->next=allocations; allocations=entry; ++allocation_count;
    }
  }
  if (position && !result) { entry=*position; *position=entry->next; free(entry); --allocation_count; }
  channel->result=result;
  ++requests;
  if (channel->operation<256) ++operations[channel->operation];
  __atomic_store_n(&channel->pending,0,__ATOMIC_RELEASE);
}

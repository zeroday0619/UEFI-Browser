#include "uefi.h"

/* This standalone probe owns and resets one Virtio GPU, then halts. It must not
   run in a browser that still uses the firmware driver's graphics queues. */
struct QueueDescriptor { uint64_t address; uint32_t length; uint16_t flags, next; };
struct AvailableRing { uint16_t flags, index, entries[8]; };
struct UsedEntry { uint32_t identifier, length; };
struct UsedRing { uint16_t flags, index; struct UsedEntry entries[8]; };
struct Command { uint32_t type, flags; uint64_t fence; uint32_t context, padding, first, second; };
_Static_assert(sizeof(struct QueueDescriptor)==16,"Virtio descriptor layout");
_Static_assert(sizeof(struct Command)==32,"GPU command layout");
static SystemTable *firmware;
static volatile uint8_t *common, *device_config, *notification;
static uint32_t notification_length, notification_multiplier;
static uint64_t storage;
static uint64_t blob_storage;
#ifdef ENABLE_VENUS_COMMANDS
static uint64_t host_visible_base, host_visible_size;
#endif
static uint32_t device, saved_command;
static volatile struct QueueDescriptor *descriptors;
static volatile struct AvailableRing *available;
static volatile struct UsedRing *used;
static uint16_t submitted;
static uint64_t next_fence;
static unsigned reset_started;

void *memset(void *destination,int value,size_t size) {
  unsigned char *output=destination; while(size--) *output++=(unsigned char)value; return destination;
}
void *memcpy(void *destination,const void *source,size_t size) {
  unsigned char *output=destination; const unsigned char *input=source;
  while(size--) *output++=*input++; return destination;
}
static uint8_t input8(uint16_t port) { uint8_t value; __asm__ volatile("inb %1,%0":"=a"(value):"Nd"(port)); return value; }
static uint32_t input32(uint16_t port) { uint32_t value; __asm__ volatile("inl %1,%0":"=a"(value):"Nd"(port)); return value; }
static void output8(uint16_t port,uint8_t value) { __asm__ volatile("outb %0,%1"::"a"(value),"Nd"(port):"memory"); }
static void output32(uint16_t port,uint32_t value) { __asm__ volatile("outl %0,%1"::"a"(value),"Nd"(port):"memory"); }
static uint32_t configuration(uint32_t address) {
  uint64_t flags; __asm__ volatile("pushfq; pop %0; cli":"=r"(flags)::"memory");
  output32(0xcf8,address); uint32_t value=input32(0xcfc);
  __asm__ volatile("push %0; popfq"::"r"(flags):"memory","cc"); return value;
}
static void configure(uint32_t address,uint32_t value) {
  uint64_t flags; __asm__ volatile("pushfq; pop %0; cli":"=r"(flags)::"memory");
  output32(0xcf8,address); output32(0xcfc,value);
  __asm__ volatile("push %0; popfq"::"r"(flags):"memory","cc");
}
static uint32_t read_config(unsigned offset) { return configuration(device+(offset&~3u))>>((offset&3)*8); }
static void text(const char *message) {
  while (*message) {
    for(unsigned count=0;count<100000 && !(input8(0x3fd)&0x20);++count) __asm__ volatile("pause");
    output8(0x3f8,(uint8_t)*message++);
  }
}
static void number(const char *name,uint64_t value) {
  static const char digits[]="0123456789abcdef"; char buffer[19]="0x";
  for(unsigned index=0;index<16;++index) buffer[index+2]=digits[(value>>((15-index)*4))&15];
  buffer[18]=0; text(name); text(buffer); text("\n");
}
static void stall(void) { ((Status(UEFI_API *)(size_t))firmware->boot->function[28])(1000); }
static uint16_t read16(volatile uint8_t *base,unsigned offset) { return *(volatile uint16_t*)(base+offset); }
static uint32_t read32(volatile uint8_t *base,unsigned offset) { return *(volatile uint32_t*)(base+offset); }
static void write16(volatile uint8_t *base,unsigned offset,uint16_t value) { *(volatile uint16_t*)(base+offset)=value; }
static void write32(volatile uint8_t *base,unsigned offset,uint32_t value) { *(volatile uint32_t*)(base+offset)=value; }
static void address(unsigned offset,uint64_t value) { write32(common,offset,value); write32(common,offset+4,value>>32); }
static int reset(void) {
  common[20]=0;
  for(unsigned attempt=0;attempt<5000;++attempt) { if (!common[20]) return 1; stall(); }
  return 0;
}
static _Noreturn void finish(const char *message) {
  /* Reset acknowledgement is required before memory can cease being DMA backing. */
  if (reset_started) {
    if (!reset()) message="FAIL: device reset did not stop DMA";
    else {
      text("VIRTIO GPU: reset acknowledged\n");
      Status released=storage ? ((Status(UEFI_API *)(uint64_t,size_t))firmware->boot->function[3])(storage,20) : 0;
      if (blob_storage)
        released|=((Status(UEFI_API *)(uint64_t,size_t))firmware->boot->function[3])(blob_storage,1);
      if (released) message="FAIL: DMA backing release";
      else if (storage) text("VIRTIO GPU: DMA backing released\n");
      configure(device+4,saved_command);
    }
  }
  text(message); text("\n");
  for(;;) __asm__ volatile("cli; hlt");
}
#ifdef ENABLE_VENUS_RENDER
int _fltused=0;
#include "venus/present.inc"
#endif
static int find_device(void) {
  unsigned count=0;
  for(unsigned bus=0;bus<256;++bus) for(unsigned slot=0;slot<32;++slot) {
    uint32_t base=0x80000000u|(bus<<16)|(slot<<11);
    if ((configuration(base)&0xffff)==0xffff) continue;
    unsigned functions=configuration(base+12)&0x00800000 ? 8 : 1;
    for(unsigned function=0;function<functions;++function)
      if (configuration(base+(function<<8))==0x10501af4) { device=base+(function<<8); ++count; }
  }
  return count==1;
}
static int capabilities(void) {
  if (!(read_config(6)&0x10)) return 0;
  unsigned visited[8]={0}; unsigned capability=read_config(0x34)&0xfc;
  for(unsigned count=0;capability && count<48;++count) {
    if (capability<0x40 || capability>0xfc || (capability&3)) return 0;
    unsigned bit=1u<<((capability/4)%32),index=(capability/4)/32;
    if (visited[index]&bit) return 0; visited[index]|=bit;
    uint32_t header=read_config(capability);
    if ((header&255)==9) {
      unsigned length=(header>>16)&255,kind=header>>24;
      if (length<16 || capability+length>256) return 0;
#ifdef ENABLE_VENUS_COMMANDS
      if (kind==8 && ((read_config(capability+4)>>8)&255)==1) {
        if (host_visible_base || length<24) return 0;
        unsigned bar=read_config(capability+4)&255;
        if (bar>=5) return 0;
        uint32_t low=read_config(0x10+bar*4);
        if ((low&7)!=4) return 0;
        uint64_t base=(low&~15u)|((uint64_t)read_config(0x14+bar*4)<<32);
        uint64_t offset=read_config(capability+8)|((uint64_t)read_config(capability+16)<<32);
        uint64_t size=read_config(capability+12)|((uint64_t)read_config(capability+20)<<32);
        if (!base || size<4096 || offset>UINT64_MAX-size || base>UINT64_MAX-offset-size) return 0;
        host_visible_base=base+offset; host_visible_size=size;
      }
#endif
      if (kind==1 || kind==2 || kind==4) {
        unsigned bar=read_config(capability+4)&255;
        if (bar>=6) return 0;
        uint32_t low=read_config(0x10+bar*4);
        if (low&1) return 0;
        uint64_t base=low&~15u;
        if ((low&6)==4) { if (bar==5) return 0; base|=(uint64_t)read_config(0x14+bar*4)<<32; }
        else if (low&6) return 0;
        uint32_t offset=read_config(capability+8),bytes=read_config(capability+12);
        if (!base || !bytes || offset>0x100000 || bytes>0x100000 || base>UINT64_MAX-offset-bytes) return 0;
        volatile uint8_t *region=(void*)(uintptr_t)(base+offset);
        if (kind==1) { if (common || bytes<56) return 0; common=region; }
        if (kind==4) { if (device_config || bytes<16) return 0; device_config=region; }
        if (kind==2) {
          if (notification || length<20) return 0;
          notification=region; notification_length=bytes; notification_multiplier=read_config(capability+16);
        }
      }
    }
    capability=(header>>8)&255;
  }
  return !capability && common && notification && device_config;
}
static uint32_t submit(const void *source,uint32_t size,uint32_t expected) {
  struct Command *request=(void*)(uintptr_t)(storage+3*4096);
  volatile uint32_t *response=(void*)(uintptr_t)(storage+4*4096);
  if (size<24 || size>4096 || next_fence==UINT64_MAX) return 0;
  memcpy(request,source,size); memset((void*)response,0,16*4096);
  /* SUBMIT_3D must fence the Venus context CPU timeline, not the legacy global timeline. */
  request->flags=request->type==0x207 ? 3 : 1; request->fence=++next_fence;
  descriptors[0]=(struct QueueDescriptor){(uintptr_t)request,size,1,1};
  descriptors[1]=(struct QueueDescriptor){(uintptr_t)response,16*4096,2,0};
  available->entries[submitted%8]=0;
  __atomic_thread_fence(__ATOMIC_RELEASE); available->index=++submitted;
  __atomic_thread_fence(__ATOMIC_SEQ_CST);
  uint64_t offset=(uint64_t)read16(common,30)*notification_multiplier;
  if (offset+2>notification_length) return 0;
  write16(notification,(unsigned)offset,0);
  for(unsigned attempt=0;attempt<5000;++attempt) {
    if (used->index==submitted) {
      __atomic_thread_fence(__ATOMIC_ACQUIRE);
      /* The counter wraps at 16 bits before the ring slot is selected. */
      struct UsedEntry entry=used->entries[(uint16_t)(submitted-1)%8];
      if (entry.identifier || entry.length<24 || entry.length>16*4096) return 0;
      if (response[0]!=expected) { number("VIRTIO GPU: unexpected response=",response[0]); return 0; }
      uint64_t completed=(uint64_t)response[2]|((uint64_t)response[3]<<32);
      /* QEMU returns the context and fence ID but does not echo INFO_RING_IDX. */
      if (!(response[1]&1) || completed!=next_fence || response[4]!=request->context) {
        number("VIRTIO GPU: unexpected fence=",completed);
        number("VIRTIO GPU: response_flags=",response[1]); number("VIRTIO GPU: response_context=",response[4]);
        number("VIRTIO GPU: response_ring=",response[5]); return 0;
      }
      return entry.length;
    }
    if (common[20]&128) return 0;
    stall();
  }
  return 0;
}
static uint32_t command(uint32_t type,uint32_t first,uint32_t second,uint32_t expected) {
  const struct Command request={.type=type,.first=first,.second=second};
  return submit(&request,sizeof(request),expected);
}
#ifdef ENABLE_VENUS_COMMANDS
#include "venus/commands.inc"
#endif
static void context_lifecycle(void) {
  struct ContextCreate { struct Command header; char name[64]; };
  struct BlobCreate {
    struct Command header;
    uint32_t flags, entries;
    uint64_t identifier, size, address;
    uint32_t length, padding;
  };
  _Static_assert(sizeof(struct ContextCreate)==96,"GPU context creation layout");
  _Static_assert(sizeof(struct BlobCreate)==72,"GPU blob creation layout");
  Status allocated=((Status(UEFI_API *)(uint32_t,uint32_t,size_t,uint64_t*))firmware->boot->function[2])(0,2,1,&blob_storage);
  if (allocated) finish("FAIL: guest blob allocation");
  memset((void*)(uintptr_t)blob_storage,0,4096);
  for (unsigned index=0;index<3;++index) {
    const struct ContextCreate create={
      .header={.type=0x200,.context=1,.first=10,.second=4},.name="UEFI Venus"};
    if (submit(&create,sizeof(create),0x1100)!=24) finish("FAIL: Venus context creation");
    /* Guest-backed blobs provide shared storage for the later Venus command ring. */
    const struct BlobCreate blob={
      .header={.type=0x10c,.context=1,.first=0x100,.second=1},
      .entries=1,.size=4096,.address=blob_storage,.length=4096};
    if (submit(&blob,sizeof(blob),0x1100)!=24) finish("FAIL: guest blob resource creation");
    const struct Command attach={.type=0x202,.context=1,.first=0x100};
    if (submit(&attach,sizeof(attach),0x1100)!=24) finish("FAIL: guest blob context attachment");
#ifdef ENABLE_VENUS_COMMANDS
    venus_commands();
#endif
    const struct Command detach={.type=0x203,.context=1,.first=0x100};
    if (submit(&detach,sizeof(detach),0x1100)!=24) finish("FAIL: guest blob context detachment");
    const struct Command release={.type=0x102,.first=0x100};
    if (submit(&release,sizeof(release),0x1100)!=24) finish("FAIL: guest blob resource release");
    const struct Command destroy={.type=0x201,.context=1};
    if (submit(&destroy,24,0x1100)!=24) finish("FAIL: Venus context destruction");
  }
  text("PASS: Venus context created, destroyed and identifier reused three times\n");
  text("PASS: guest blob created, attached, detached and released three times\n");
}
Status UEFI_API efi_main(Handle image,SystemTable *table) {
  (void)image; firmware=table;
  if (!table || !table->boot) return ERROR(2);
  ((Status(UEFI_API *)(size_t,uint64_t,size_t,Character*))table->boot->function[29])(0,0,0,0);
  text("VIRTIO GPU: standalone control-queue probe\n");
#ifdef ENABLE_VENUS_RENDER
  venus_display_initialize();
#endif
  if (!find_device()) finish("FAIL: expected exactly one Virtio GPU");
  number("VIRTIO GPU: pci=",device);
  if (!capabilities()) finish("FAIL: invalid modern PCI capabilities");
  saved_command=read_config(4)&0xffff; configure(device+4,saved_command|6);
  reset_started=1;
  if (!reset()) finish("FAIL: device reset timeout");
  common[20]=3;
  write32(common,0,0); uint64_t features=read32(common,4);
  write32(common,0,1); features|=(uint64_t)read32(common,4)<<32;
  number("VIRTIO GPU: offered_features=",features);
  const uint64_t required=(1ULL<<32)|1|8|16;
  if ((features&required)!=required) finish("FAIL: required Venus transport features absent");
  write32(common,8,0); write32(common,12,(uint32_t)required);
  write32(common,8,1); write32(common,12,required>>32);
  common[20]=11;
  if (!(common[20]&8)) finish("FAIL: feature negotiation rejected");
  Status status=((Status(UEFI_API *)(uint32_t,uint32_t,size_t,uint64_t*))table->boot->function[2])(0,2,20,&storage);
  if (status) finish("FAIL: DMA memory allocation");
  memset((void*)(uintptr_t)storage,0,20*4096);
  descriptors=(void*)(uintptr_t)storage; available=(void*)(uintptr_t)(storage+4096); used=(void*)(uintptr_t)(storage+2*4096);
  available->flags=1;
  write16(common,16,0xffff); write16(common,22,0);
  if (read16(common,24)<8) finish("FAIL: control queue is too small");
  write16(common,24,8); write16(common,26,0xffff);
  address(32,storage); address(40,storage+4096); address(48,storage+2*4096);
  write16(common,28,1); common[20]=15;
  unsigned capsets=read32(device_config,12);
  number("VIRTIO GPU: capset_count=",capsets);
  if (!capsets || capsets>64) finish("FAIL: invalid capset count");
  if (!command(0x100,0,0,0x1101)) finish("FAIL: display information request");
  text("PASS: Virtio GPU display information command completed\n");
  volatile uint32_t *response=(void*)(uintptr_t)(storage+4*4096);
  unsigned venus_version=0,venus_size=0;
  for(unsigned index=0;index<capsets;++index) {
    if (command(0x108,index,0,0x1102)<40) finish("FAIL: capset information request");
    number("VIRTIO GPU: capset_id=",response[6]); number("VIRTIO GPU: capset_version=",response[7]); number("VIRTIO GPU: capset_bytes=",response[8]);
    if (response[6]==4) { venus_version=response[7]; venus_size=response[8]; }
  }
  if (!venus_size || venus_size>16*4096-24) finish("FAIL: missing or oversized Venus capabilities");
  if (command(0x109,4,venus_version,0x1103)!=venus_size+24) finish("FAIL: Venus capability payload length");
  uint32_t checksum=2166136261u; const volatile uint8_t *bytes=(void*)(response+6);
  for(unsigned index=0;index<venus_size;++index) checksum=(checksum^bytes[index])*16777619u;
  number("VIRTIO GPU: venus_payload_fnv1a=",checksum);
#ifdef ENABLE_VENUS_COMMANDS
  venus_check_capabilities((const void*)bytes,venus_size);
#endif
  context_lifecycle();
  for(unsigned index=0;index<65536;++index)
    if (!command(0x100,0,0,0x1101)) finish("FAIL: control queue wraparound");
  text("PASS: Virtio GPU control queue completed three ring wraps\n");
  text("PASS: Virtio GPU control queue crossed the 16-bit index boundary\n");
  number("VIRTIO GPU: validated_fences=",next_fence);
  finish("PASS: UEFI Virtio control queue and Venus capability payload");
}

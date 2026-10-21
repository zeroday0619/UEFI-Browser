/* SPDX-License-Identifier: BSD-3-Clause */
/* PCI discovery and modern capability decoding share the standalone probe layout. */
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

static uint16_t read16(volatile uint8_t *base,unsigned offset) { return *(volatile uint16_t*)(base+offset); }
static uint32_t read32(volatile uint8_t *base,unsigned offset) { return *(volatile uint32_t*)(base+offset); }
static void write16(volatile uint8_t *base,unsigned offset,uint16_t value) { *(volatile uint16_t*)(base+offset)=value; }
static void write32(volatile uint8_t *base,unsigned offset,uint32_t value) { *(volatile uint32_t*)(base+offset)=value; }
static void address(unsigned offset,uint64_t value) { write32(common,offset,value); write32(common,offset+4,value>>32); }

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

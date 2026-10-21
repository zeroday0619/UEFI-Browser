struct VenusDisplayInformation {
  uint32_t version,width,height,format,masks[4],stride;
};
struct VenusDisplayMode {
  uint32_t maximum,current;
  struct VenusDisplayInformation *information;
  size_t size;
  uint64_t framebuffer;
  size_t framebuffer_size;
};
struct VenusGraphics { void *query,*set,*blit; struct VenusDisplayMode *mode; };
static struct VenusDisplayInformation venus_display;
static uint64_t venus_framebuffer;
static void venus_display_initialize(void) {
  Guid guid={0x9042a9de,0x23dc,0x4a38,{0x96,0xfb,0x7a,0xde,0xd0,0x80,0x51,0x6a}};
  size_t count=0; Handle *handles=NULL;
  Status status=((Status(UEFI_API *)(uint32_t,Guid*,void*,size_t*,Handle**))firmware->boot->function[36])(2,&guid,NULL,&count,&handles);
  if(status || !handles || !count || count>32) finish("FAIL: GOP handle discovery");
  uint64_t vga_base=0;
  for(unsigned slot=0;slot<32;++slot) {
    uint32_t address=0x80000000u|(slot<<11);
    if(configuration(address)==0x11111234) {
      const uint32_t bar=configuration(address+0x10);
      if(!(bar&1)) vga_base=bar&~15u;
    }
  }
  for(size_t index=0;index<count;++index) {
    struct VenusGraphics *graphics=NULL;
    status=((Status(UEFI_API *)(Handle,Guid*,void**))firmware->boot->function[16])(handles[index],&guid,(void**)&graphics);
    if(status || !graphics || !graphics->mode || !graphics->mode->information) continue;
    struct VenusDisplayMode *mode=graphics->mode;
    const struct VenusDisplayInformation *information=mode->information;
    /* The independent VGA framebuffer keeps presentation outside the owned Virtio queues. */
    if(!vga_base || mode->framebuffer!=vga_base || information->format>1 ||
       information->width<544 || information->height<608 || information->stride<information->width ||
       information->stride>16384 || information->height>16384 ||
       (uint64_t)information->stride*information->height*4>mode->framebuffer_size) continue;
    venus_display=*information; venus_framebuffer=mode->framebuffer; break;
  }
  ((Status(UEFI_API *)(void*))firmware->boot->function[6])(handles);
  if(!venus_framebuffer) finish("FAIL: independent standard VGA GOP framebuffer");
  number("VENUS: preview_display_width=",venus_display.width); number("VENUS: preview_display_height=",venus_display.height);
}
static void venus_present(const uint8_t *pixels,uint32_t width) {
  if(width!=128 || !venus_framebuffer) finish("FAIL: preview dimensions");
  volatile uint8_t *output=(void*)(uintptr_t)venus_framebuffer;
  for(uint32_t y=0;y<venus_display.height;++y) for(uint32_t x=0;x<venus_display.width;++x) {
    volatile uint8_t *destination=output+4*((size_t)y*venus_display.stride+x);
    uint8_t red=12,green=18,blue=28;
    if(x>=32 && x<544 && y>=96 && y<608) {
      const uint8_t *source=pixels+4*(((y-96)/4)*width+(x-32)/4);
      red=source[0]; green=source[1]; blue=source[2];
    }
    destination[0]=venus_display.format ? blue : red;
    destination[1]=green; destination[2]=venus_display.format ? red : blue; destination[3]=0;
  }
  __atomic_thread_fence(__ATOMIC_SEQ_CST);
  text("PASS: Venus GPU image presented through independent GOP framebuffer\n");
}

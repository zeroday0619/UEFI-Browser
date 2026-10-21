/* Application startup diagnostics run before loading the browser runtime. */
static SystemTable *post_table;
static void post_text(const char *text) {
  Character buffer[128];
  size_t used=0;
  while (*text) {
    unsigned char character=*text++;
    if (character=='\n') buffer[used++]='\r';
    buffer[used++]=character;
    if (used>=125 || !*text) {
      buffer[used]=0;
      post_table->output->write(post_table->output,buffer);
      used=0;
    }
  }
}
static void post_number(uint64_t value) {
  static const char digits[]="0123456789abcdef";
  char text[19]="0x";
  for (unsigned index=0;index<16;++index) text[index+2]=digits[(value>>((15-index)*4))&15];
  text[18]=0; post_text(text);
}
static void post_line(const char *state,const char *name,Status status) {
  post_text("["); post_text(state); post_text("] "); post_text(name);
  if (status) { post_text(" - status "); post_number(status); }
  post_text("\n");
}
static Status post_failure(const char *name,Status status) {
  if (post_table->output->set_attribute)
    ((Status(UEFI_API *)(Output *,size_t))post_table->output->set_attribute)(post_table->output,0x0c);
  post_line("FAIL",name,status);
  post_text("\nStartup stopped. Press a key to return to firmware.\n");
  if (post_table->input && post_table->input->read) {
    Key key;
    while (post_table->input->read(post_table->input,&key)==ERROR(6))
      ((Status(UEFI_API *)(size_t))post_table->boot->function[28])(10000);
  }
  return status;
}
static Status post_initialize(SystemTable *table) {
  post_table=table;
  if (!table || !table->output || !table->boot) return ERROR(2);
  if (table->output->set_attribute)
    ((Status(UEFI_API *)(Output *,size_t))table->output->set_attribute)(table->output,0x0f);
  table->output->clear(table->output);
  post_text("UEFI BROWSER POST\nApplication startup self-test\n\n");
  post_line("RUN","Disable firmware boot watchdog",0);
  if (!table->boot->function[29])
    return post_failure("Firmware watchdog service missing",ERROR(3));
  /* The browser retains boot services beyond the boot manager's five-minute limit. */
  Status watchdog_status =
      ((Status(UEFI_API *)(size_t,uint64_t,size_t,Character*))table->boot->function[29])(
          0,0,0,0);
  if (watchdog_status==ERROR(3))
    post_line("SKIP","Firmware watchdog is unavailable",0);
  else if (watchdog_status)
    return post_failure("Disable firmware boot watchdog",watchdog_status);
  else
    post_line("PASS","Firmware boot watchdog disabled",0);
  post_line("RUN","CPU long mode, SSE2 and execute-disable",0);
  uint32_t first,second,third,fourth;
  __asm__ volatile("cpuid" : "=a"(first),"=b"(second),"=c"(third),"=d"(fourth) : "a"(1),"c"(0));
  if (!(fourth&(1u<<26)) || !(fourth&(1u<<24))) return post_failure("CPU SSE2/FXSAVE required",ERROR(3));
  __asm__ volatile("cpuid" : "=a"(first),"=b"(second),"=c"(third),"=d"(fourth) : "a"(0x80000000u),"c"(0));
  if (first<0x80000001u) return post_failure("CPU extended features missing",ERROR(3));
  __asm__ volatile("cpuid" : "=a"(first),"=b"(second),"=c"(third),"=d"(fourth) : "a"(0x80000001u),"c"(0));
  if (!(fourth&(1u<<29)) || !(fourth&(1u<<20))) return post_failure("CPU long mode/NX required",ERROR(3));
  post_line("PASS","CPU long mode, SSE2 and execute-disable",0);
  /* Exercise allocated writable RAM rather than claiming to test all installed memory. */
  uint64_t address=0;
  Status status=((Status(UEFI_API *)(uint32_t,uint32_t,size_t,uint64_t*))table->boot->function[2])(0,2,16,&address);
  if (status) return post_failure("64 KiB writable-memory allocation",status);
  volatile uint64_t *memory=(void*)address;
  for (unsigned pass=0;pass<2;++pass) {
    for (size_t index=0;index<8192;++index) memory[index]=(pass ? 0xaaaaaaaaaaaaaaaaULL : 0x5555555555555555ULL)^index;
    for (size_t index=0;index<8192;++index)
      if (memory[index]!=((pass ? 0xaaaaaaaaaaaaaaaaULL : 0x5555555555555555ULL)^index)) status=ERROR(7);
  }
  Status released=((Status(UEFI_API *)(uint64_t,size_t))table->boot->function[3])(address,16);
  if (status || released) return post_failure("64 KiB writable-memory test",status ? status : released);
  post_line("PASS","64 KiB allocation, pattern read/write and release",0);
  return 0;
}
static void post_ready(void) {
#ifdef ENABLE_RENDERER_CPL3
  post_line("INFO","CPL3 renderer required; isolation checks run at runtime startup",0);
#else
  post_line("INFO","Unconfined runtime build",0);
#endif
  post_text("\nPOST complete. Starting UEFI BROWSER...\n");
  ((Status(UEFI_API *)(size_t))post_table->boot->function[28])(750000);
}

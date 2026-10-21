#include "uefi.h"
#ifdef ENABLE_INPUT_DRIVER
#include "input-driver.h"
#include "http-driver.h"
#endif
#include "loader-image.h"
#ifdef ENABLE_MEDIA_SERVICE
extern RendererBootstrap *loader_media_initialize(Handle, SystemTable *,
    const ElfHeader *, const ElfProgram *, const ElfProgram *);
#endif
static RendererBootstrap browser_bootstrap;
static Engine engine;
static void *engine_stack;
static void *engine_thread;
extern Status UEFI_API browser_call_on_stack(void *, Handle, SystemTable *,
                                        Status(UEFI_API *)(Handle, SystemTable *));
static Status UEFI_API invoke(Handle image, SystemTable *table) {
  uint32_t low, high;
  __asm__ volatile("rdmsr" : "=a"(low), "=d"(high) : "c"(0xc0000100u));
  uint64_t saved = ((uint64_t)high << 32) | low;
  uint64_t current = (uintptr_t)engine_thread;
  __asm__ volatile("wrmsr" ::"a"((uint32_t)current),
                   "d"((uint32_t)(current >> 32)), "c"(0xc0000100u)
                   : "memory");
  int result = engine(image, table, engine_stack, &browser_bootstrap);
  __asm__ volatile("wrmsr" ::"a"((uint32_t)saved), "d"((uint32_t)(saved >> 32)),
                   "c"(0xc0000100u)
                   : "memory");
  return result ? ERROR(21) : 0;
}
static void copy(void *destination, const void *source, size_t size) {
  unsigned char *output = destination;
  const unsigned char *input = source;
  while (size--)
    *output++ = *input++;
}
#include "loader-renderer.h"
#include "loader-post.h"
Status UEFI_API efi_main(Handle image, SystemTable *table) {
  Status startup=post_initialize(table);
  if (startup) return startup;
#ifdef ENABLE_INPUT_DRIVER
  post_line("RUN","Firmware HTTP driver",0);
  Status input_status = install_http_driver(image, table);
  if (input_status) return post_failure("Firmware HTTP driver",input_status);
  post_line("PASS","Firmware HTTP driver",0);
  post_line("RUN","Pointer and wheel input driver",0);
  input_status = install_input_driver(image, table);
  if (input_status) return post_failure("Pointer and wheel input driver",input_status);
  post_line("PASS","Pointer and wheel input driver",0);
#endif
  post_line("RUN","ELF image structure, bounds and executable entry",0);
  size_t length = (size_t)(payload_end - payload_start);
  if (length < sizeof(ElfHeader))
    return post_failure("ELF image bounds or segment validation",ERROR(1));
  const ElfHeader *header = (const ElfHeader *)payload_start;
  if (header->identity[0] != 127 || header->identity[1] != 'E' ||
      header->identity[2] != 'L' || header->identity[3] != 'F' ||
      header->identity[4] != 2 || header->identity[5] != 1 ||
      header->machine != 62 || header->type != 2 ||
      header->program_size != sizeof(ElfProgram))
    return post_failure("ELF image format or unsupported segment",ERROR(3));
  if (header->programs > length ||
      header->program_count > (length - header->programs) / sizeof(ElfProgram))
    return post_failure("ELF image bounds or segment validation",ERROR(1));
  const ElfProgram *programs =
      (const ElfProgram *)(payload_start + header->programs);
  uint64_t low = UINT64_MAX, high = 0;
  int executable = 0;
  const ElfProgram *tls = 0;
  for (unsigned index = 0; index < header->program_count; index++) {
    const ElfProgram *program = programs + index;
    if (program->type == 2 || program->type == 3)
      return post_failure("ELF image format or unsupported segment",ERROR(3));
    if (program->type == 7) {
      if (tls || program->memory_size > 16 * 1024 * 1024 ||
          program->file_size > program->memory_size ||
          program->offset > length ||
          program->file_size > length - program->offset ||
          program->alignment > 4096 ||
          (program->alignment &&
           (program->alignment & (program->alignment - 1))))
        return post_failure("ELF image bounds or segment validation",ERROR(1));
      tls = program;
    }
    if (program->type != 1)
      continue;
    if (program->address < 0x10000000 || program->address > 0x20000000 ||
        program->memory_size > 0x20000000 - program->address ||
        program->file_size > program->memory_size || program->offset > length ||
        program->file_size > length - program->offset)
      return post_failure("ELF image bounds or segment validation",ERROR(1));
    if (program->address < low)
      low = program->address;
    if (program->address + program->memory_size > high)
      high = program->address + program->memory_size;
    if ((program->flags & 1) && header->entry >= program->address &&
        header->entry < program->address + program->file_size)
      executable = 1;
  }
  if (!executable || low == UINT64_MAX || high <= low)
    return post_failure("ELF image bounds or segment validation",ERROR(1));
  post_line("PASS","ELF image structure, bounds and executable entry",0);
  post_line("RUN","Payload memory, stack and thread-local storage",0);
  low &= ~4095ULL;
  high = (high + 4095) & ~4095ULL;
  uint64_t address = low;
  size_t pages = (high - low) / 4096;
  Status result = ((Status(UEFI_API *)(uint32_t, uint32_t, size_t, uint64_t *))
                       table->boot->function[2])(2, 1, pages, &address);
  if (result)
    return post_failure("Payload memory allocation",result);
  for (size_t index = 0; index < high - low; index++)
    ((unsigned char *)address)[index] = 0;
  for (unsigned index = 0; index < header->program_count; index++)
    if (programs[index].type == 1)
      copy((void *)programs[index].address,
           payload_start + programs[index].offset, programs[index].file_size);
  void *thread_allocation = 0;
  size_t tls_size = tls ? (tls->memory_size + 4095) & ~(size_t)4095 : 0;
  result = ((Status(UEFI_API *)(uint32_t, size_t, void **))table->boot->function[5])(
      2, tls_size + 4096 + 4096, &thread_allocation);
  if (result) {
    ((Status(UEFI_API *)(uint64_t, size_t))table->boot->function[3])(address, pages);
    return post_failure("Thread-local storage allocation",result);
  }
  unsigned char *thread_base =
      (unsigned char *)(((uintptr_t)thread_allocation + 4095) &
                        ~(uintptr_t)4095);
  for (size_t index = 0; index < tls_size + 4096; index++)
    thread_base[index] = 0;
  // Local-exec TLS places the TLS image immediately before the thread pointer.
  size_t alignment = tls && tls->alignment ? tls->alignment : 1;
  size_t tls_length =
      tls ? (tls->memory_size + alignment - 1) & ~(alignment - 1) : 0;
  engine_thread = thread_base + tls_size;
  if (tls)
    copy((unsigned char *)engine_thread - tls_length,
         payload_start + tls->offset, tls->file_size);
  *(void **)engine_thread = engine_thread;
  void *stack = 0;
  result = ((Status(UEFI_API *)(uint32_t, size_t, void **))table->boot->function[5])(
      2, 4 * 1024 * 1024, &stack);
  if (!result) {
    engine = (Engine)header->entry;
    renderer.table = table; renderer.image = image; renderer.header = header;
    renderer.programs = programs; renderer.tls = tls;
#ifdef ENABLE_MEDIA_SERVICE
    browser_bootstrap.media = loader_media_initialize(image,table,header,programs,tls);
#endif
    browser_bootstrap.start = renderer_start; browser_bootstrap.join = renderer_join;
    browser_bootstrap.terminate = renderer_terminate; browser_bootstrap.fault = renderer_fault;
    browser_bootstrap.buffer = renderer_buffer;
#ifdef ENABLE_RENDERER_CPL3
    browser_bootstrap.sandbox=1;
    browser_bootstrap.grant=renderer_grant;
    browser_bootstrap.reserve_tables=renderer_reserve_tables;
#endif
    engine_stack = stack;
    post_line("PASS","Payload memory, stack and thread-local storage",0);
    post_ready();
    result = browser_call_on_stack((unsigned char *)stack + 4 * 1024 * 1024,
                                   image, table, invoke);
    Status cleanup = ((Status(UEFI_API *)(void *))table->boot->function[6])(stack);
    if (!result)
      result = cleanup;
  }
  Status cleanup =
      ((Status(UEFI_API *)(void *))table->boot->function[6])(thread_allocation);
  if (!result)
    result = cleanup;
  cleanup = ((Status(UEFI_API *)(uint64_t, size_t))table->boot->function[3])(address,
                                                                        pages);
  if (!result)
    result = cleanup;
  if (!result)
    table->output->write(
        table->output,
        (Character *)L"PASS: EFI loader released payload, stack and TLS\r\n");
  if (result) return post_failure("Runtime execution or resource cleanup",result);
  return result;
}

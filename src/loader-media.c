#include "loader-media-symbols.h"
#include "loader-image.h"
static Engine engine;
static void copy(void *destination, const void *source, size_t size) {
  unsigned char *output = destination;
  const unsigned char *input = source;
  while (size--) *output++ = *input++;
}
extern Status UEFI_API browser_call_on_stack(void *, Handle, SystemTable *,
                                           Status(UEFI_API *)(Handle, SystemTable *));
#define RENDERER_PAYLOAD_ROLE 2
#define RENDERER_PROCESSOR_SKIP 1
#include "loader-renderer.h"
static RendererBootstrap media_bootstrap;
RendererBootstrap *loader_media_initialize(Handle image, SystemTable *table,
    const ElfHeader *header, const ElfProgram *programs, const ElfProgram *tls) {
  engine = (Engine)header->entry;
  renderer.table = table;
  renderer.image = image;
  renderer.header = header;
  renderer.programs = programs;
  renderer.tls = tls;
  media_bootstrap.start = renderer_start;
  media_bootstrap.join = renderer_join;
  media_bootstrap.terminate = renderer_terminate;
  media_bootstrap.fault = renderer_fault;
  media_bootstrap.buffer = renderer_buffer;
  media_bootstrap.sandbox = 1;
  media_bootstrap.grant = renderer_grant;
  media_bootstrap.reserve_tables = renderer_reserve_tables;
  return &media_bootstrap;
}

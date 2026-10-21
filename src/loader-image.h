#ifndef UEFI_LOADER_IMAGE_H
#define UEFI_LOADER_IMAGE_H
#include "uefi.h"
#include "renderer_runtime.h"
extern const unsigned char payload_start[], payload_end[];
typedef struct {
  unsigned char identity[16];
  uint16_t type, machine;
  uint32_t version;
  uint64_t entry, programs, sections;
  uint32_t flags;
  uint16_t size, program_size, program_count, section_size, section_count, names;
} ElfHeader;
typedef struct {
  uint32_t type, flags;
  uint64_t offset, address, physical, file_size, memory_size, alignment;
} ElfProgram;
typedef int(__attribute__((sysv_abi)) * Engine)(Handle, SystemTable *, void *, RendererBootstrap *);
#endif

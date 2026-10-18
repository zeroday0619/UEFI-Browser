#ifndef BROWSER_INPUT_DRIVER_H
#define BROWSER_INPUT_DRIVER_H
#include "uefi.h"
extern const unsigned char input_driver_start[], input_driver_end[];

/* Only the input consumer is replaced; the firmware owns the virtio transport. */
static Status install_input_driver(Handle parent, SystemTable *table) {
  Guid virtio_guid = {0xfa920010, 0x6785, 0x4941,
      {0xb6, 0xec, 0x49, 0x8c, 0x57, 0x9f, 0x16, 0x0a}};
  Guid pointer_guid = {0x31878c87, 0x0b75, 0x11d5,
      {0x9a, 0x4f, 0, 0x90, 0x27, 0x3f, 0xc1, 0x4d}};
  typedef struct { uint32_t revision; int32_t device; } VirtioHeader;
  Status (UEFI_API *load)(uint8_t, Handle, void*, void*, size_t, Handle*) =
      (void*)table->boot->function[22];
  Status (UEFI_API *start)(Handle, size_t*, Character**) =
      (void*)table->boot->function[23];
  Status (UEFI_API *handles)(int, Guid*, void*, size_t*, Handle**) =
      (void*)table->boot->function[36];
  Status (UEFI_API *protocol)(Handle, Guid*, void**) =
      (void*)table->boot->function[16];
  Status (UEFI_API *disconnect)(Handle, Handle, Handle) =
      (void*)table->boot->function[31];
  Status (UEFI_API *connect)(Handle, Handle*, void*, uint8_t) =
      (void*)table->boot->function[30];
  Status (UEFI_API *release)(void*) = (void*)table->boot->function[6];
  Handle driver = 0;
  Status result = load(0, parent, 0, (void*)input_driver_start,
      input_driver_end - input_driver_start, &driver);
  if (result) return result;
  result = start(driver, 0, 0);
  if (result) return result;
  Handle *list = 0;
  size_t count = 0;
  result = handles(2, &virtio_guid, 0, &count, &list);
  if (result == ERROR(14)) return 0;
  if (result) return result;
  for (size_t index = 0; index < count; ++index) {
    VirtioHeader *virtio = 0;
    void *pointer = 0;
    if (protocol(list[index], &virtio_guid, (void**)&virtio) ||
        virtio->device != 18 ||
        protocol(list[index], &pointer_guid, &pointer)) continue;
    result = disconnect(list[index], 0, 0);
    if (result) break;
    Handle preferred[] = {driver, 0};
    result = connect(list[index], preferred, 0, 1);
    if (result) {
      connect(list[index], 0, 0, 1);
      break;
    }
    table->output->write(table->output,
        (Character*)L"INPUT: patched virtio pointer driver connected\r\n");
  }
  release(list);
  return result;
}
#endif

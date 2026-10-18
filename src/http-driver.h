#ifndef BROWSER_HTTP_DRIVER_H
#define BROWSER_HTTP_DRIVER_H
#include "uefi.h"
extern const unsigned char http_driver_start[], http_driver_end[];
static Status install_http_driver(Handle parent, SystemTable *table) {
  Guid tcp = {0x00720665,0x67eb,0x4a99,{0xba,0xf7,0xd3,0xc3,0x3a,0x1c,0x7c,0xc9}};
  Status (UEFI_API *load)(uint8_t, Handle, void*, void*, size_t, Handle*) = (void*)table->boot->function[22];
  Status (UEFI_API *start)(Handle, size_t*, Character**) = (void*)table->boot->function[23];
  Status (UEFI_API *handles)(int, Guid*, void*, size_t*, Handle**) = (void*)table->boot->function[36];
  Status (UEFI_API *connect)(Handle, Handle*, void*, uint8_t) = (void*)table->boot->function[30];
  Status (UEFI_API *release)(void*) = (void*)table->boot->function[6];
  Handle driver = 0;
  Status result = load(0, parent, 0, (void*)http_driver_start, http_driver_end - http_driver_start, &driver);
  if (result) return result;
  if ((result = start(driver, 0, 0))) return result;
  Handle *list = 0;
  size_t count = 0;
  if ((result = handles(2, &tcp, 0, &count, &list))) return result == ERROR(14) ? 0 : result;
  Handle preferred[] = {driver, 0};
  for (size_t index = 0; index < count; ++index) {
    result = connect(list[index], preferred, 0, 0);
    if (result) break;
  }
  release(list);
  return result;
}
#endif

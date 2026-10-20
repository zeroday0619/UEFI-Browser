#ifndef UEFI_RENDERER_SANDBOX_H
#define UEFI_RENDERER_SANDBOX_H
#include <stdint.h>
/* The interrupt ABI uses registers so the entry path never trusts a user stack. */
enum {
  RENDERER_SYSTEM_ABI = 0x1000,
  RENDERER_SYSTEM_THREAD,
  RENDERER_SYSTEM_PERMISSIONS,
  RENDERER_SYSTEM_EXIT,
  RENDERER_SYSTEM_MAP,
  RENDERER_SYSTEM_PROTECT,
  RENDERER_SYSTEM_BROKER
};
static inline uint64_t renderer_system_call(uint64_t operation, uint64_t first,
    uint64_t second, uint64_t third, uint64_t fourth, uint64_t fifth) {
  register uint64_t argument_four __asm__("r10")=fourth;
  register uint64_t argument_five __asm__("r8")=fifth;
  __asm__ volatile("int $0x80" : "+a"(operation) : "D"(first), "S"(second),
      "d"(third), "r"(argument_four), "r"(argument_five) : "memory", "cc");
  return operation;
}
#endif

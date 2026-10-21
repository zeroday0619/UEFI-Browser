#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "uefi.h"

static Status watchdog_result;
static unsigned watchdog_calls, allocations, releases;
static uint64_t memory[8192];
static char transcript[4096];
static size_t transcript_length;

static Status UEFI_API output_write(Output *output, Character *text) {
  (void)output;
  while (*text) {
    assert(transcript_length + 1 < sizeof(transcript));
    transcript[transcript_length++] = (char)*text++;
  }
  transcript[transcript_length] = 0;
  return 0;
}
static Status UEFI_API output_clear(Output *output) {
  (void)output;
  transcript_length = 0;
  transcript[0] = 0;
  return 0;
}
static Status UEFI_API watchdog(size_t timeout, uint64_t code, size_t size,
                                Character *data) {
  assert(!timeout && !code && !size && !data);
  assert(!allocations);
  ++watchdog_calls;
  return watchdog_result;
}
static Status UEFI_API allocate(uint32_t type, uint32_t kind, size_t pages,
                                uint64_t *address) {
  assert(watchdog_calls == 1 && !type && kind == 2 && pages == 16);
  ++allocations;
  *address = (uintptr_t)memory;
  return 0;
}
static Status UEFI_API release(uint64_t address, size_t pages) {
  assert(address == (uintptr_t)memory && pages == 16);
  ++releases;
  return 0;
}

#include "../src/loader-post.h"

int main(void) {
  Output output = {.write = output_write, .clear = output_clear};
  BootServices boot = {0};
  SystemTable table = {.output = &output, .boot = &boot};
  boot.function[2] = allocate;
  boot.function[3] = release;
  boot.function[29] = watchdog;
  const Status results[] = {0, ERROR(3), ERROR(7), ERROR(2)};
  for (size_t index = 0; index < sizeof(results) / sizeof(results[0]); ++index) {
    watchdog_calls = allocations = releases = 0;
    watchdog_result = results[index];
    const Status result = post_initialize(&table);
    assert(watchdog_calls == 1);
    if (!watchdog_result || watchdog_result == ERROR(3)) {
      assert(!result && allocations == 1 && releases == 1);
      assert(strstr(transcript, watchdog_result ? "[SKIP] Firmware watchdog is unavailable"
                                               : "[PASS] Firmware boot watchdog disabled"));
    } else {
      assert(result == watchdog_result && !allocations && !releases);
      assert(strstr(transcript, "[FAIL] Disable firmware boot watchdog"));
    }
  }
  watchdog_calls = allocations = releases = 0;
  boot.function[29] = 0;
  assert(post_initialize(&table) == ERROR(3));
  assert(!watchdog_calls && !allocations && !releases);
  assert(strstr(transcript, "[FAIL] Firmware watchdog service missing"));
  puts("PASS: watchdog ABI, startup ordering, unsupported firmware and failure gates");
}

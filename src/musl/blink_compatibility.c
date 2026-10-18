#include <fcntl.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <stdio.h>
#include <pthread.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

_Static_assert(sizeof(off_t) == 8, "The Rust file ABI requires 64-bit offsets.");
_Static_assert(sizeof(struct stat) == 144, "The x86-64 stat ABI must match Rust.");
_Static_assert(offsetof(struct stat, st_mode) == 24, "The stat mode offset must match Rust.");

/* Rust's Linux target uses these names, while x86-64 musl already uses 64-bit offsets. */
off_t firmware_lseek64(int descriptor, off_t offset, int origin) __asm__("lseek64");
off_t firmware_lseek64(int descriptor, off_t offset, int origin) {
  return lseek(descriptor, offset, origin);
}
int firmware_open64(const char *path, int flags, ...) __asm__("open64");
int firmware_open64(const char *path, int flags, ...) {
  mode_t mode = 0;
  if ((flags & O_CREAT) || (flags & O_TMPFILE) == O_TMPFILE) {
    va_list arguments;
    va_start(arguments, flags);
    mode = va_arg(arguments, mode_t);
    va_end(arguments);
  }
  return open(path, flags, mode);
}
int firmware_fstat64(int descriptor, struct stat *information) __asm__("fstat64");
int firmware_fstat64(int descriptor, struct stat *information) {
  return fstat(descriptor, information);
}
int firmware_stat64(const char *path, struct stat *information) __asm__("stat64");
int firmware_stat64(const char *path, struct stat *information) {
  return stat(path, information);
}
void *firmware_mmap64(void *address, size_t length, int protection, int flags,
                     int descriptor, off_t offset) __asm__("mmap64");
void *firmware_mmap64(void *address, size_t length, int protection, int flags,
                     int descriptor, off_t offset) {
  return mmap(address, length, protection, flags, descriptor, offset);
}

void firmware_atomic_load(size_t size, const void *source, void *destination,
                          int order) __asm__("__atomic_load");
void firmware_atomic_load(size_t size, const void *source, void *destination,
                          int order) {
  (void)order;
  /* The firmware engine runs only on the BSP and never shares its objects with APs.
     Masking interrupts makes arbitrary-sized loads indivisible on that processor.
     Full fences provide the strongest ordering accepted by the generic ABI. */
  uintptr_t flags;
  __asm__ volatile("pushfq; popq %0; cli; mfence" : "=r"(flags) : : "memory", "cc");
  memcpy(destination, source, size);
  __asm__ volatile("mfence; pushq %0; popfq" : : "r"(flags) : "memory", "cc");
}

struct FirmwareUnwindContext { uintptr_t instruction_pointer; };
typedef int (*FirmwareTraceCallback)(struct FirmwareUnwindContext *, void *);
uintptr_t firmware_unwind_instruction_pointer(struct FirmwareUnwindContext *context)
    __asm__("_Unwind_GetIP");
uintptr_t firmware_unwind_instruction_pointer(struct FirmwareUnwindContext *context) {
  return context->instruction_pointer;
}
int firmware_backtrace(FirmwareTraceCallback callback, void *argument)
    __asm__("_Unwind_Backtrace");
int firmware_backtrace(FirmwareTraceCallback callback, void *argument) {
  pthread_attr_t attribute;
  void *base;
  size_t size;
  if (!callback || pthread_getattr_np(pthread_self(), &attribute))
    return 3;
  int result = pthread_attr_getstack(&attribute, &base, &size);
  pthread_attr_destroy(&attribute);
  if (result || size < 2 * sizeof(uintptr_t) || (uintptr_t)base > UINTPTR_MAX - size)
    return 3;
  uintptr_t lower = (uintptr_t)base;
  uintptr_t upper = lower + size;
  uintptr_t *frame = __builtin_frame_address(0);
  /* This reports raw frame-pointer addresses only; omitted frames cannot be
     reconstructed without unwind tables. Bounds and monotonic links prevent
     walking outside the owned EFI stack or following corrupt cycles. */
  for (size_t count = 0; count < 256; ++count) {
    uintptr_t address = (uintptr_t)frame;
    if (address < lower || address > upper - 2 * sizeof(uintptr_t) ||
        address % sizeof(uintptr_t))
      break;
    struct FirmwareUnwindContext context = {frame[1]};
    if (!context.instruction_pointer)
      break;
    result = callback(&context, argument);
    if (result)
      return result;
    uintptr_t next = frame[0];
    if (next <= address)
      break;
    frame = (uintptr_t *)next;
  }
  return 5;
}
struct FirmwareTraceBuffer {
  const void **addresses;
  size_t capacity;
  size_t count;
};
static int store_trace_frame(struct FirmwareUnwindContext *context, void *argument) {
  struct FirmwareTraceBuffer *buffer = argument;
  if (buffer->count == buffer->capacity)
    return 4;
  buffer->addresses[buffer->count++] = (const void *)context->instruction_pointer;
  return 0;
}
size_t firmware_collect_stack_trace(const void **addresses, size_t capacity) {
  struct FirmwareTraceBuffer buffer = {addresses, capacity, 0};
  firmware_backtrace(store_trace_frame, &buffer);
  return buffer.count;
}
static int count_trace_frame(struct FirmwareUnwindContext *context, void *argument) {
  if (!firmware_unwind_instruction_pointer(context))
    return 3;
  ++*(size_t *)argument;
  return 0;
}
static int stop_trace_frame(struct FirmwareUnwindContext *context, void *argument) {
  if (!firmware_unwind_instruction_pointer(context))
    return 3;
  ++*(size_t *)argument;
  return 4;
}

int run_blink_compatibility_probe(void) {
  unsigned char source[65], destination[65];
  for (size_t index = 0; index < sizeof(source); ++index)
    source[index] = (unsigned char)(index * 7);
  for (size_t size = 1; size < sizeof(source); ++size) {
    uintptr_t before, after;
    memset(destination, 0, sizeof(destination));
    __asm__ volatile("pushfq; popq %0" : "=r"(before) : : "memory");
    firmware_atomic_load(size, source + 1, destination + 1, __ATOMIC_SEQ_CST);
    __asm__ volatile("pushfq; popq %0" : "=r"(after) : : "memory");
    if (memcmp(source + 1, destination + 1, size) || destination[0] ||
        ((before ^ after) & (1u << 9)))
      return 1;
  }
  void *mapping = firmware_mmap64(NULL, 4096, PROT_READ | PROT_WRITE,
      MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  if (mapping == MAP_FAILED)
    return 1;
  memset(mapping, 0x5a, 4096);
  if (((unsigned char *)mapping)[4095] != 0x5a || munmap(mapping, 4096))
    return 1;
  size_t frames = 0;
  if (firmware_backtrace(count_trace_frame, &frames) != 5 || !frames)
    return 1;
  frames = 0;
  if (firmware_backtrace(stop_trace_frame, &frames) != 4 || frames != 1)
    return 1;
  puts("PASS: bounded EFI frame-pointer backtrace and callback stop");
  puts("PASS: generic atomic loads, interrupt restoration and Rust mmap64 ABI");
  return 0;
}

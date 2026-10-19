#define _GNU_SOURCE
#include <errno.h>
#include "renderer_runtime.h"
#include <limits.h>
#include <stdint.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <sys/uio.h>
#include <time.h>
#include <unistd.h>
extern void graphics_log(const char *);
extern _Bool firmware_entropy(unsigned char *, size_t);
long firmware_serial_write(long descriptor, const char *data, size_t length) {
  if (descriptor != 1 && descriptor != 2)
    return -EBADF;
  if (length > LONG_MAX)
    return -EINVAL;
  if (renderer_runtime_is_child()) {
    extern void renderer_runtime_write(const void *, size_t);
    renderer_runtime_write(data,length);
    return length;
  }
  for (size_t index = 0; index < length; index++) {
    char text[2] = {data[index], 0};
    if (data[index])
      graphics_log(text);
    else {
      /* Binary output must preserve embedded NUL bytes. */
      unsigned char state;
      do {
        __asm__ volatile("inb %1,%0"
                         : "=a"(state)
                         : "Nd"((unsigned short)0x3fd));
      } while (!(state & 0x20));
      __asm__ volatile("outb %0,%1" ::"a"((unsigned char)0),
                       "Nd"((unsigned short)0x3f8));
    }
  }
  return (long)length;
}
long firmware_syscall(long number, long first, long second, long third,
                      long fourth, long fifth, long sixth) {
  switch (number) {
  case SYS_write:
    return firmware_serial_write(first, (void *)second, third);
  case SYS_writev: {
    if (third < 0 || third > IOV_MAX)
      return -EINVAL;
    const struct iovec *vectors = (void *)second;
    size_t total = 0;
    for (long index = 0; index < third; index++) {
      if (vectors[index].iov_len > LONG_MAX - total)
        return -EINVAL;
      total += vectors[index].iov_len;
    }
    for (long index = 0; index < third; index++) {
      long result =
          firmware_serial_write(first, vectors[index].iov_base, vectors[index].iov_len);
      if (result < 0)
        return result;
    }
    return total;
  }
  case SYS_ioctl:
    return -ENOTTY;
  case SYS_read:
    return first == 0 ? -ENOTSUP : -EBADF;
  case SYS_open:
  case SYS_openat:
    return -ENOTSUP;
  case SYS_close:
    return -EBADF;
  case SYS_lseek:
    return first >= 0 && first <= 2 ? -ESPIPE : -EBADF;
  case SYS_getpid:
  case SYS_gettid:
    return renderer_runtime_is_child() ? 2 : 1;
  case SYS_clock_gettime:
    return clock_gettime(first, (void *)second) ? -errno : 0;
  case SYS_nanosleep:
    return nanosleep((void *)first, (void *)second) ? -errno : 0;
  case SYS_mmap: {
    void *result = mmap((void *)first, second, third, fourth, fifth, sixth);
    return result == MAP_FAILED ? -errno : (long)result;
  }
  case SYS_munmap:
    return munmap((void *)first, second) ? -errno : 0;
  case SYS_mprotect:
    return mprotect((void *)first, second, third) ? -errno : 0;
  case SYS_madvise:
    return madvise((void *)first, second, third) ? -errno : 0;
  case SYS_getrandom:
    if (third & ~3L)
      return -EINVAL;
    if (second < 0 || second > 256 * 1024)
      return -EINVAL;
    return firmware_entropy((void *)first, second) ? second : -EIO;
  case SYS_exit:
  case SYS_exit_group:
    abort();
  default:
    return -ENOSYS;
  }
}

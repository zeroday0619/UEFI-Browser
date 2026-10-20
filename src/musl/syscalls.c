#define _GNU_SOURCE
#include <errno.h>
#include "renderer_runtime.h"
#include "firmware_threads.h"
#include "firmware_io.h"
#include "firmware_shared.h"
#include <string.h>
#include <limits.h>
#include <stdint.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <sys/uio.h>
#include <sys/utsname.h>
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
  if (number == SYS_memfd_create || ((number==SYS_close || number==SYS_dup || number==SYS_fcntl || number==SYS_ftruncate || number==SYS_fstat) && firmware_shared_descriptor(first)))
    return firmware_shared_syscall(number,first,second,third);
  if ((number==SYS_open || number==SYS_openat)) {
    const char *path=(const char *)(number==SYS_open ? first : second);
    if(path && !strncmp(path,"/proc/self/fd/",14)) return firmware_shared_reopen(path,number==SYS_open ? second : third);
  }
  if (number == SYS_socket || number == SYS_connect || number == SYS_sendto ||
      number == SYS_recvfrom || number == SYS_getsockopt || number == SYS_setsockopt ||
      number == SYS_getsockname || number == SYS_getpeername || number == SYS_poll ||
      ((number == SYS_read || number == SYS_write || number == SYS_close || number == SYS_fcntl) &&
       firmware_socket_descriptor(first)))
    return firmware_socket_syscall(number,first,second,third,fourth,fifth,sixth);
  if (number == SYS_open || number == SYS_openat || number == SYS_unlink || number == SYS_mkdir ||
      ((number == SYS_read || number == SYS_write || number == SYS_close || number == SYS_fcntl ||
        number == SYS_lseek || number == SYS_fstat || number == SYS_ftruncate || number == SYS_fsync || number == SYS_fdatasync) &&
       firmware_file_descriptor(first)))
    return firmware_file_syscall(number,first,second,third,fourth,fifth,sixth);
  switch (number) {
  case SYS_uname: {
    if (!first) return -EFAULT;
    struct utsname *information = (void *)first;
    memset(information, 0, sizeof(*information));
    strcpy(information->sysname, "UEFI");
    strcpy(information->nodename, "uefi-browser");
    strcpy(information->release, "0.1");
    strcpy(information->version, "Firmware runtime ABI 1");
    strcpy(information->machine, "x86_64");
    return 0;
  }
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
    size_t written = 0;
    for (long index = 0; index < third; index++) {
      long result = firmware_syscall(SYS_write,first,(long)vectors[index].iov_base,vectors[index].iov_len,0,0,0);
      if (result < 0) return written ? (long)written : result;
      written += result;
      if ((size_t)result != vectors[index].iov_len) return written;
    }
    return written;
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
  case SYS_prctl:
    return firmware_thread_name(first, (char *)second);
  case SYS_getpid:
    return renderer_runtime_is_child() ? 2 : 1;
  case SYS_gettid:
    return firmware_thread_identifier();
  case SYS_sched_yield:
    firmware_threads_yield();
    return 0;
  case SYS_futex:
    return firmware_threads_futex((int *)first, second, third, (void *)fourth, (int *)fifth, sixth);
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

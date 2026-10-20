#ifndef UEFI_FIRMWARE_SHARED_H
#define UEFI_FIRMWARE_SHARED_H
#include <stddef.h>
#include <sys/types.h>
int firmware_shared_descriptor(long descriptor);
long firmware_shared_syscall(long number, long first, long second, long third);
long firmware_shared_reopen(const char *path, int flags);
void *firmware_shared_map(size_t length, int protection, int flags,
                          int descriptor, off_t offset);
int firmware_shared_mapping(const void *address);
int firmware_shared_unmap(void *address, size_t length);
void firmware_shared_reset(void);
#endif

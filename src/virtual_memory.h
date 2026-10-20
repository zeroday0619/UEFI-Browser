#ifndef UEFI_VIRTUAL_MEMORY_H
#define UEFI_VIRTUAL_MEMORY_H
#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>
size_t virtual_memory_reservation_limit(void);
int virtual_memory_contains(const void *address);
void *virtual_memory_map(void *hint, size_t length, int protection, int flags,
                         int descriptor, off_t offset);
int virtual_memory_protect(void *address, size_t length, int protection);
int virtual_memory_unmap(void *address, size_t length);
int virtual_memory_discard(void *address, size_t length);
void virtual_memory_release(void);
void *virtual_memory_alias(uintptr_t physical,size_t length,int protection,int writable);
#endif

#ifndef UEFI_FIRMWARE_THREADS_H
#define UEFI_FIRMWARE_THREADS_H
#include <time.h>
#ifdef __cplusplus
extern "C" {
#endif
void firmware_threads_yield(void);
int firmware_thread_identifier(void);
unsigned firmware_threads_available(void);
long firmware_thread_name(int operation, char *name);
long firmware_threads_futex(int *address, int operation, int expected,
                            const struct timespec *timeout, int *destination,
                            unsigned mask);
#ifdef __cplusplus
}
#endif
#endif

#ifndef UEFI_FIRMWARE_IO_H
#define UEFI_FIRMWARE_IO_H
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
/* The BSP owns firmware protocol instances and dispatches bounded I/O requests. */
long firmware_socket_syscall(long number, long first, long second, long third,
                             long fourth, long fifth, long sixth);
long firmware_socket_service(unsigned owner, long number, const long arguments[6]);
void firmware_socket_reset(unsigned owner);
int firmware_socket_descriptor(long descriptor);
#define RENDERER_SOCKET_REQUEST 128
void firmware_file_initialize(void *image);
int firmware_file_descriptor(long descriptor);
long firmware_file_syscall(long number, long first, long second, long third,
                           long fourth, long fifth, long sixth);
long firmware_file_service(unsigned owner, long number, const long arguments[6]);
void firmware_file_reset(unsigned owner);
#define RENDERER_FILE_REQUEST 129
#define RENDERER_DNS_REQUEST 130
long firmware_dns_request(unsigned operation, long handle, void *buffer);
long firmware_dns_service(unsigned owner, unsigned operation, long handle, void *buffer);
void firmware_dns_reset(unsigned owner);
#ifdef __cplusplus
}
#endif
#endif

#ifndef UEFI_RENDERER_RUNTIME_H
#define UEFI_RENDERER_RUNTIME_H
#include "uefi.h"
#ifdef __cplusplus
extern "C" {
#endif
/* The loader owns each address space until its AP has returned to firmware. */
typedef struct RendererBootstrap RendererBootstrap;
struct RendererBootstrap {
  unsigned renderer;
  unsigned sandbox, boundary_test;
  uint64_t supervisor_address, root_address;
  void *channel;
  void *application;
  uint64_t data_start, data_end, data_physical;
  Status (UEFI_API *start)(void *channel);
  Status (UEFI_API *join)(void);
  Status (UEFI_API *terminate)(void);
  const uint64_t *fault;
  int (UEFI_API *grant)(uint64_t address, size_t size, unsigned permissions);
  int (UEFI_API *buffer)(uint64_t address, size_t size, unsigned writable);
  int (UEFI_API *reserve_tables)(size_t minimum);
  RendererBootstrap *media;
};
/* Only the supervisor maps this request slot in CPL3 mode. */
typedef struct {
  uint32_t pending;
  unsigned operation;
  uint64_t arguments[5], result;
  void *application;
  uint32_t private_verified;
  size_t application_size;
} RendererRuntimeChannel;
void renderer_runtime_initialize(RendererBootstrap *bootstrap);
int renderer_runtime_is_child(void);
int renderer_runtime_available(void);
void *renderer_runtime_channel(void);
Status renderer_runtime_start(void *channel);
void *renderer_runtime_shared_allocate(size_t size);
void renderer_runtime_shared_free(void *address, size_t size);
void renderer_runtime_application_size(size_t size);
void *renderer_runtime_export_allocate(size_t size);
void renderer_runtime_export_free(void *address,size_t size);
/* Device grants are non-executable and cannot acquire virtual aliases. */
int renderer_runtime_device_grant(uint64_t address, size_t size, unsigned cache_type);
Status renderer_runtime_join(void);
Status renderer_runtime_terminate(void);
void renderer_network_reset(void);
SystemTable *renderer_runtime_table(SystemTable *table);
void renderer_runtime_service(void);
uint64_t renderer_runtime_frequency(void);
enum { RENDERER_MEMORY_PAGES=13, RENDERER_MEMORY_STATISTICS=14 };
/* Fields are charged physical bytes, quota bytes and allocation count. */
uint64_t renderer_runtime_memory_statistic(unsigned field);
/* The BSP checks ownership before dereferencing a renderer-supplied address. */
int renderer_runtime_buffer(uint64_t address, size_t size, unsigned writable);
int renderer_runtime_string(char *output, size_t capacity, uint64_t address);
int renderer_runtime_sandboxed(void);
RendererBootstrap *renderer_runtime_media_bootstrap(void);
/* Runtime requests are synchronous; UI and network service polling never wait for the AP. */
uint64_t renderer_runtime_request(unsigned operation, uint64_t first, uint64_t second,
                                  uint64_t third, uint64_t fourth, uint64_t fifth);
enum { RENDERER_NETWORK_START = 64, RENDERER_NETWORK_RESULT,
       RENDERER_NETWORK_RELEASE, RENDERER_RESPONSE_RELEASE,
       RENDERER_NETWORK_REQUEST };
uint64_t renderer_network_service(unsigned operation, uint64_t first, uint64_t second,
                                  uint64_t third, uint64_t fourth, uint64_t fifth);
#ifdef __cplusplus
}
#endif
#endif

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
  void *channel;
  uint64_t data_start, data_end, data_physical;
  Status (UEFI_API *start)(void *channel);
  Status (UEFI_API *join)(void);
  Status (UEFI_API *terminate)(void);
  const uint64_t *fault;
};
void renderer_runtime_initialize(RendererBootstrap *bootstrap);
int renderer_runtime_is_child(void);
int renderer_runtime_available(void);
void *renderer_runtime_channel(void);
Status renderer_runtime_start(void *channel);
Status renderer_runtime_join(void);
Status renderer_runtime_terminate(void);
void renderer_network_reset(void);
SystemTable *renderer_runtime_table(SystemTable *table);
void renderer_runtime_service(void);
uint64_t renderer_runtime_frequency(void);
/* Runtime requests are synchronous; UI and network service polling never wait for the AP. */
uint64_t renderer_runtime_request(unsigned operation, uint64_t first, uint64_t second,
                                  uint64_t third, uint64_t fourth, uint64_t fifth);
enum { RENDERER_NETWORK_START = 64, RENDERER_NETWORK_RESULT,
       RENDERER_NETWORK_RELEASE, RENDERER_RESPONSE_RELEASE };
uint64_t renderer_network_service(unsigned operation, uint64_t first, uint64_t second,
                                  uint64_t third, uint64_t fourth, uint64_t fifth);
#ifdef __cplusplus
}
#endif
#endif

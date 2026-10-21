/* SPDX-License-Identifier: BSD-3-Clause */
#ifndef UEFI_VENUS_BROKER_H
#define UEFI_VENUS_BROKER_H
#include <stdint.h>
#define RENDERER_VENUS_REQUEST 133u
#define VENUS_BROKER_VERSION 1u
#define VENUS_BROKER_CONTEXT_LIMIT 16u
#define VENUS_BROKER_BLOB_LIMIT 256u
#define VENUS_BROKER_MEMORY_LIMIT (256ULL * 1024 * 1024)
enum { VENUS_OPEN=1, VENUS_CLOSE, VENUS_SUBMIT, VENUS_POLL, VENUS_RELEASE,
       VENUS_CREATE_BLOB, VENUS_DESTROY_BLOB, VENUS_PRESENT, VENUS_PUBLISH_IMAGE, VENUS_RELEASE_IMAGE };
typedef struct {
    uint32_t version, mappable;
    uint64_t identifier, size, mapping;
    uint32_t resource, reserved;
} VenusBlobMessage;
struct firmware_venus_image;
int firmware_venus_broker_acquire_image(uint64_t, struct firmware_venus_image *);
void firmware_venus_broker_release_image(uint64_t);
long firmware_venus_broker(const uint64_t arguments[5]);
void firmware_venus_reset(void);
#endif

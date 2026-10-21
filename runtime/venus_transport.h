/* SPDX-License-Identifier: BSD-3-Clause */
#ifndef FIRMWARE_VENUS_TRANSPORT_H
#define FIRMWARE_VENUS_TRANSPORT_H

#include <stddef.h>
#include <stdint.h>
#include "venus_image.h"

/* This is a trusted service interface, not a renderer system-call ABI.
 * A provider owns the Virtio context and retains DMA storage until teardown
 * has acknowledged device reset or every relevant fence has completed.
 * Up to 16 contexts share one physical queue and aperture; tickets and resources
 * remain owned by their creating context. Providers serialize operations across
 * callers. The physical device resets only after its last context closes. Closing a context
 * retires all remaining fence tickets after stopping host access. */
struct firmware_venus_context;

struct firmware_venus_capabilities {
    uint32_t wire_format_version;
    uint32_t vulkan_version;
    uint32_t command_serialization_version;
    uint32_t protocol_version;
    uint32_t extension_mask[32];
    uint32_t timeline_count;
};

struct firmware_venus_blob {
    uint32_t resource_id;
    uint32_t cache_type;
    uint64_t size;
    void *mapping;
};

/* Zero means success. Implementations must fail closed on invalid capabilities,
 * context identifiers, mapping ranges, command lengths, or fence completions. */
int firmware_venus_open(struct firmware_venus_context **context,
                        struct firmware_venus_capabilities *capabilities);
void firmware_venus_close(struct firmware_venus_context *context);

/* Submission copies the command before returning. A nonzero ticket identifies
 * a context-local completion and remains valid until release_fence is called.
 * Poll returns 1 for completion, 0 for pending, and -1 for device loss. */
int firmware_venus_submit(struct firmware_venus_context *context,
                          const void *command, size_t size, uint32_t timeline,
                          uint64_t *ticket);
int firmware_venus_poll(struct firmware_venus_context *context, uint64_t ticket);
void firmware_venus_release_fence(struct firmware_venus_context *context,
                                 uint64_t ticket);

/* An identifier of zero creates HOST3D shared memory. Other identifiers export
 * Vulkan allocations. Returned mappings must be coherent, including host writes.
 * Unmapped device-local exports have a null mapping. Release unmaps and detaches
 * the resource before returning, or quarantines its backing after device loss. */
int firmware_venus_create_blob(struct firmware_venus_context *context,
                               uint64_t memory_identifier, uint64_t size,
                               int mappable, struct firmware_venus_blob *blob);
void firmware_venus_destroy_blob(struct firmware_venus_context *context,
                                 struct firmware_venus_blob *blob);

/* Only the trusted compositor may select a scanout. The resource stays owned
 * by its context until a completed replacement or disable command. */
int firmware_venus_scanout(struct firmware_venus_context *context, uint32_t resource,
                           uint32_t width, uint32_t height, uint32_t stride, uint32_t offset, uint64_t modifier);

/* Shared images use immutable device-local allocations and bounded capabilities. */
int firmware_venus_retain_blob(struct firmware_venus_context *, uint32_t);
void firmware_venus_release_blob(uint32_t);
int firmware_venus_attach_blob(struct firmware_venus_context *, uint32_t);
void firmware_venus_detach_blob(struct firmware_venus_context *, uint32_t);
uint64_t firmware_venus_publish_image(struct firmware_venus_context *, const struct firmware_venus_image *);
void firmware_venus_release_image(uint64_t);
int firmware_venus_acquire_image(uint64_t, struct firmware_venus_image *);
#endif

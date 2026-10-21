/* SPDX-License-Identifier: BSD-3-Clause */
#include "venus_transport.h"
#include "venus_device.h"
#include "uefi.h"
#include "../../virtual_memory.h"
#include <sys/mman.h>
#include "venus_broker.h"
#include "renderer_runtime.h"
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <vulkan/vulkan.h>
extern PFN_vkVoidFunction vk_icdGetInstanceProcAddr(VkInstance, const char *);
PFN_vkVoidFunction firmware_venus_get_proc(VkInstance instance, const char *name) {
    return vk_icdGetInstanceProcAddr(instance, name);
}
int firmware_gpu_composition_enabled(void) {
#ifdef ENABLE_GPU_COMPOSITION
    return 1;
#else
    return 0;
#endif
}
int firmware_gpu_canvas_enabled(void) {
#ifdef ENABLE_GPU_CANVAS
    return 1;
#else
    return 0;
#endif
}
struct compositor_mapping {
    struct firmware_venus_context *context;
    struct firmware_venus_blob physical;
    void *alias;
    struct compositor_mapping *next;
};
static struct compositor_mapping *compositor_mappings;
/* Private supervisor aliases avoid changing firmware MTRRs for each blob. */
static int map_supervisor_blob(struct firmware_venus_context *context, struct firmware_venus_blob *blob)
{
    if (!blob->mapping) return 0;
    struct compositor_mapping *mapping=calloc(1,sizeof(*mapping));
    if (!mapping) return -1;
    mapping->alias=virtual_memory_device_alias((uintptr_t)blob->mapping,blob->size,blob->cache_type);
    if (mapping->alias==MAP_FAILED) { free(mapping); return -1; }
    mapping->context=context; mapping->physical=*blob;
    mapping->next=compositor_mappings; compositor_mappings=mapping;
    blob->mapping=mapping->alias;
    return 0;
}
static uint64_t presentation_session;
uint64_t firmware_venus_client_session(void) { return presentation_session; }
static long request(unsigned operation, struct firmware_venus_context *context,
                    uint64_t first, uint64_t second, uint64_t third)
{
    if (!renderer_runtime_is_child() || !renderer_runtime_sandboxed()) return -1;
    return renderer_runtime_request(RENDERER_VENUS_REQUEST, operation,
                                    (uintptr_t)context, first, second, third);
}
int firmware_venus_open(struct firmware_venus_context **context,
                        struct firmware_venus_capabilities *capabilities)
{
    if (!renderer_runtime_is_child())
        return firmware_gpu_composition_enabled() ? firmware_venus_device_open(context, capabilities) : -1;
    long result=request(VENUS_OPEN, (void *)(uintptr_t)VENUS_BROKER_VERSION,
                        (uintptr_t)capabilities, 0, 0);
    if (result <= 0) return -1;
    *context=(void *)(uintptr_t)result;
    presentation_session=result;
    return 0;
}
void firmware_venus_close(struct firmware_venus_context *context)
{
    if (!renderer_runtime_is_child()) {
        firmware_venus_device_close(context);
        struct compositor_mapping **position=&compositor_mappings;
        while (*position) {
            struct compositor_mapping *mapping=*position;
            if (mapping->context!=context) { position=&mapping->next; continue; }
            virtual_memory_unmap(mapping->alias,mapping->physical.size);
            *position=mapping->next; free(mapping);
        }
        return;
    }
    request(VENUS_CLOSE, context, 0, 0, 0);
    if (presentation_session == (uintptr_t)context) presentation_session=0;
}
int firmware_venus_submit(struct firmware_venus_context *context, const void *data,
                          size_t size, uint32_t timeline, uint64_t *ticket)
{
    if (!renderer_runtime_is_child()) return firmware_venus_device_submit(context,data,size,timeline,ticket);
    long result=request(VENUS_SUBMIT, context, (uintptr_t)data, size, timeline);
    if (result <= 0) return -1;
    *ticket=result;
    return 0;
}
int firmware_venus_poll(struct firmware_venus_context *context, uint64_t ticket)
{
    if (!renderer_runtime_is_child()) return firmware_venus_device_poll(context,ticket);
    return request(VENUS_POLL, context, ticket, 0, 0);
}
void firmware_venus_release_fence(struct firmware_venus_context *context, uint64_t ticket)
{
    if (!renderer_runtime_is_child()) { firmware_venus_device_release_fence(context,ticket); return; }
    request(VENUS_RELEASE, context, ticket, 0, 0);
}
int firmware_venus_create_blob(struct firmware_venus_context *context,
                               uint64_t identifier, uint64_t size, int mappable,
                               struct firmware_venus_blob *blob)
{
    if (!renderer_runtime_is_child()) {
        if (firmware_venus_device_create_blob(context,identifier,size,mappable,blob)) return -1;
        if (!map_supervisor_blob(context,blob)) return 0;
        firmware_venus_device_destroy_blob(context,blob);
        return -1;
    }
    VenusBlobMessage message={.version=VENUS_BROKER_VERSION, .identifier=identifier,
                              .size=size, .mappable=mappable};
    if (request(VENUS_CREATE_BLOB, context, (uintptr_t)&message, 0, 0)) return -1;
    *blob=(struct firmware_venus_blob){.resource_id=message.resource,
                                     .size=message.size, .mapping=(void *)message.mapping};
    return 0;
}
void firmware_venus_destroy_blob(struct firmware_venus_context *context,
                                 struct firmware_venus_blob *blob)
{
    if (!renderer_runtime_is_child()) {
        struct compositor_mapping **position=&compositor_mappings;
        while (*position && ((*position)->context!=context || (*position)->physical.resource_id!=blob->resource_id)) position=&(*position)->next;
        if (*position) {
            struct compositor_mapping *mapping=*position;
            if (blob->mapping!=mapping->alias || blob->size!=mapping->physical.size) return;
            firmware_venus_device_destroy_blob(context,&mapping->physical);
            if (mapping->physical.resource_id) return;
            virtual_memory_unmap(mapping->alias,blob->size);
            *position=mapping->next; free(mapping); memset(blob,0,sizeof(*blob));
        } else firmware_venus_device_destroy_blob(context,blob);
        return;
    }
    if (!request(VENUS_DESTROY_BLOB, context, blob->resource_id, 0, 0))
        memset(blob, 0, sizeof(*blob));
}
void firmware_venus_present_image(const uint8_t *pixels, uint32_t width)
{
    if (width != 128) abort();
    uint8_t *copy=malloc(65536);
    if (!copy) abort();
    memcpy(copy, pixels, 65536);
    long result=request(VENUS_PRESENT, (void *)presentation_session, (uintptr_t)copy, width, 0);
    free(copy);
    if (result) abort();
}

int firmware_venus_scanout(struct firmware_venus_context *context, uint32_t resource,
                           uint32_t width, uint32_t height, uint32_t stride, uint32_t offset, uint64_t modifier)
{
    if (renderer_runtime_is_child() || !firmware_gpu_composition_enabled()) return -1;
    return firmware_venus_device_scanout(context,resource,width,height,stride,offset,modifier);
}

uint64_t firmware_venus_publish_image(struct firmware_venus_context *context, const struct firmware_venus_image *image)
{
    long token=request(VENUS_PUBLISH_IMAGE,context,(uintptr_t)image,0,0);
    return token>0 ? (uint64_t)token : 0;
}
void firmware_venus_release_image(uint64_t token)
{
    if (renderer_runtime_is_child()) request(VENUS_RELEASE_IMAGE,(void *)(uintptr_t)token,0,0,0);
    else firmware_venus_broker_release_image(token);
}
int firmware_venus_acquire_image(uint64_t token, struct firmware_venus_image *image)
{
    return renderer_runtime_is_child() ? -1 : firmware_venus_broker_acquire_image(token,image);
}
int firmware_venus_attach_blob(struct firmware_venus_context *context,uint32_t resource)
{
    return renderer_runtime_is_child() ? -1 : firmware_venus_device_attach_blob(context,resource);
}
void firmware_venus_detach_blob(struct firmware_venus_context *context,uint32_t resource)
{
    if (!renderer_runtime_is_child()) firmware_venus_device_detach_blob(context,resource);
}

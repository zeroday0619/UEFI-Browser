/* SPDX-License-Identifier: BSD-3-Clause */
#include <stdio.h>
#include <time.h>
#include "../src/gpu/venus-runtime/renderer.c"
#include "vn_renderer_util.c"

/* This deterministic transport exercises ownership and completion handling.
 * It does not emulate Vulkan execution or provide hardware validation. */
struct firmware_venus_context { unsigned opened; };
static struct firmware_venus_context context;
static int completions[256];
static unsigned tickets, released, blob_count, closes;
static bool malformed_capabilities, malformed_blob, automatic_completion;
static bool fail_submission, fail_blob;

int firmware_venus_open(struct firmware_venus_context **output,
                        struct firmware_venus_capabilities *capabilities)
{
    *output = &context;
    context.opened = 1;
    *capabilities = (struct firmware_venus_capabilities){
        .wire_format_version = malformed_capabilities ? 2 : 1,
        .vulkan_version = VK_API_VERSION_1_3, .timeline_count = 64,
    };
    return 0;
}
void firmware_venus_close(struct firmware_venus_context *value)
{
    assert(value == &context && context.opened);
    context.opened = 0;
    ++closes;
}
int firmware_venus_submit(struct firmware_venus_context *value,
                          const void *command, size_t size, uint32_t timeline,
                          uint64_t *ticket)
{
    assert(value == &context && context.opened && timeline < 64);
    assert(!size || command);
    if (fail_submission)
        return -1;
    assert(tickets + 1 < 256);
    *ticket = ++tickets;
    completions[tickets] = automatic_completion;
    return 0;
}
int firmware_venus_poll(struct firmware_venus_context *value, uint64_t ticket)
{
    assert(value == &context && context.opened && ticket && ticket <= tickets);
    return completions[ticket];
}
void firmware_venus_release_fence(struct firmware_venus_context *value, uint64_t ticket)
{
    assert(value == &context && context.opened && completions[ticket]);
    ++released;
}
int firmware_venus_create_blob(struct firmware_venus_context *value,
                               uint64_t identifier, uint64_t size, int mappable,
                               struct firmware_venus_blob *blob)
{
    assert(value == &context && context.opened && !(size & 4095));
    if (fail_blob)
        return -1;
    if (identifier)
        assert(released == tickets);
    *blob = (struct firmware_venus_blob){
        .resource_id = ++blob_count, .size = malformed_blob ? size - 1 : size,
        .mapping = mappable ? calloc(1, size) : NULL,
    };
    return 0;
}
void firmware_venus_destroy_blob(struct firmware_venus_context *value,
                                 struct firmware_venus_blob *blob)
{
    assert(value == &context && context.opened && blob_count);
    free(blob->mapping);
    --blob_count;
}
int firmware_venus_scanout(struct firmware_venus_context *value, uint32_t resource,
                          uint32_t width, uint32_t height, uint32_t stride, uint32_t offset, uint64_t modifier)
{
    (void)value; (void)resource; (void)width; (void)height; (void)stride; (void)offset; (void)modifier;
    return -1;
}
int firmware_venus_attach_blob(struct firmware_venus_context *value,uint32_t resource) {
    return value==&context && context.opened && resource==77 ? 0 : -1;
}
void firmware_venus_detach_blob(struct firmware_venus_context *value,uint32_t resource) {
    assert(value==&context && context.opened && resource==77);
}
uint64_t firmware_venus_publish_image(struct firmware_venus_context *value,const struct firmware_venus_image *image) {
    (void)value; (void)image; return 0;
}
int64_t os_time_get_nano(void)
{
    struct timespec value;
    assert(!clock_gettime(CLOCK_MONOTONIC, &value));
    return (int64_t)value.tv_sec * 1000000000 + value.tv_nsec;
}
void os_time_sleep(int64_t microseconds)
{
    struct timespec delay = {.tv_sec = microseconds / 1000000,
                             .tv_nsec = microseconds % 1000000 * 1000};
    nanosleep(&delay, NULL);
}
static void *allocate(void *user, size_t size, size_t alignment,
                       VkSystemAllocationScope scope)
{
    (void)user; (void)scope;
    void *memory = NULL;
    return posix_memalign(&memory, alignment, size) ? NULL : memory;
}
static void deallocate(void *user, void *memory) { (void)user; free(memory); }
static const VkAllocationCallbacks allocator = {
    .pfnAllocation = allocate, .pfnFree = deallocate,
};
static void submit_signal(struct vn_renderer *renderer, struct vn_renderer_sync *sync,
                    uint64_t value)
{
    struct vn_renderer_submit_batch batch = {
        .ring_idx = 1, .syncs = &sync, .sync_values = &value, .sync_count = 1,
    };
    assert(submit(renderer, &batch) == VK_SUCCESS);
}
static uint64_t read_value(struct vn_renderer *renderer, struct vn_renderer_sync *sync)
{
    uint64_t value = UINT64_MAX;
    assert(read_sync(renderer, sync, &value) == VK_SUCCESS);
    return value;
}
int main(void)
{
    struct vn_renderer *renderer = NULL;
    malformed_capabilities = true;
    assert(vn_renderer_create_uefi(NULL, &allocator, &renderer) == VK_ERROR_INCOMPATIBLE_DRIVER);
    assert(!renderer && closes == 1);
    malformed_capabilities = false;
    assert(vn_renderer_create_uefi(NULL, &allocator, &renderer) == VK_SUCCESS);
    assert(!renderer->info.has_external_sync && !renderer->info.has_dma_buf_import);
    struct vn_renderer_sync *sync;
    assert(create_sync(renderer, 0, &sync) == VK_SUCCESS);
    submit_signal(renderer, sync, 1);
    assert(read_value(renderer, sync) == 0);
    struct vn_renderer_wait wait = {.sync_count = 1, .syncs = &sync,
                                    .sync_values = &(uint64_t){1}};
    assert(wait_syncs(renderer, &wait) == VK_TIMEOUT);
    completions[1] = 1;
    assert(wait_syncs(renderer, &wait) == VK_SUCCESS && released == 1);
    submit_signal(renderer, sync, 9);
    submit_signal(renderer, sync, 5);
    completions[2] = completions[3] = 1;
    assert(read_value(renderer, sync) == 9);
    submit_signal(renderer, sync, 10);
    assert(reset_sync(renderer, sync) == VK_SUCCESS);
    completions[4] = 1;
    assert(read_value(renderer, sync) == 0);
    submit_signal(renderer, sync, 20);
    destroy_sync(renderer, sync);
    completions[5] = 1;
    assert(create_sync(renderer, 3, &sync) == VK_SUCCESS);
    assert(read_value(renderer, sync) == 3 && released == 5);
    struct vn_renderer_sync *second;
    assert(create_sync(renderer, 0, &second) == VK_SUCCESS);
    struct vn_renderer_sync *syncs[] = {sync, second};
    uint64_t values[] = {3, 1};
    wait = (struct vn_renderer_wait){.sync_count = 2, .syncs = syncs, .sync_values = values};
    assert(wait_syncs(renderer, &wait) == VK_TIMEOUT);
    wait.wait_any = true;
    assert(wait_syncs(renderer, &wait) == VK_SUCCESS);
    assert(write_sync(renderer, second, 2) == VK_SUCCESS);
    assert(write_sync(renderer, second, 1) == VK_SUCCESS);
    assert(read_value(renderer, second) == 2);
    destroy_sync(renderer, second);
    assert(!create_shmem(renderer, 0));
    assert(!create_shmem(renderer, SIZE_MAX));
    malformed_blob = true;
    assert(!create_shmem(renderer, 1) && !blob_count);
    malformed_blob = false;
    struct vn_renderer_shmem *shared = create_shmem(renderer, 4097);
    assert(shared && shared->mmap_size == 8192 && shared->mmap_ptr);
    destroy_shmem(renderer, shared);
    fail_blob = true;
    assert(!create_shmem(renderer, 4096));
    fail_blob = false;
    automatic_completion = true;
    uint32_t command = 0;
    struct vn_renderer_submit_batch batch = {.cs_data = &command, .cs_size = sizeof(command)};
    struct vn_renderer_bo *buffer;
    assert(create_buffer(renderer, &batch, 65536, 202,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT, 0, &buffer) == VK_SUCCESS);
    assert(released == tickets && buffer->mmap_size == 65536);
    assert(map_buffer(renderer, buffer, NULL) == buffer->mmap_ptr);
    assert(!map_buffer(renderer, buffer, (char *)buffer->mmap_ptr + 1));
    synchronize_buffer(renderer, buffer, 1, VK_WHOLE_SIZE);
    assert(destroy_buffer(renderer, buffer) && !blob_count);
    assert(create_buffer(renderer, NULL, 65536, 203, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
        VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT, &buffer)==VK_SUCCESS);
    assert(!buffer->mmap_ptr && buffer->res_id);
    assert(destroy_buffer(renderer,buffer) && !blob_count);
    assert(create_buffer(renderer,NULL,65536,204,VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
        VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT,&buffer)==VK_ERROR_INVALID_EXTERNAL_HANDLE);
    fail_submission = true;
    assert(submit(renderer, &batch) == VK_ERROR_DEVICE_LOST);
    uint64_t value;
    assert(read_sync(renderer, sync, &value) == VK_ERROR_DEVICE_LOST);
    destroy_sync(renderer, sync);
    destroy(renderer, &allocator);
    assert(closes == 2);
    fail_submission = false;
    automatic_completion = false;
    assert(vn_renderer_create_uefi(NULL, &allocator, &renderer) == VK_SUCCESS);
    assert(create_sync(renderer, 0, &sync) == VK_SUCCESS);
    submit_signal(renderer, sync, 1);
    completions[tickets] = -1;
    assert(read_sync(renderer, sync, &value) == VK_ERROR_DEVICE_LOST);
    destroy_sync(renderer, sync);
    destroy(renderer, &allocator);
    assert(vn_renderer_create_uefi(NULL, &allocator, &renderer) == VK_SUCCESS);
    assert(create_sync(renderer, 0, &sync) == VK_SUCCESS);
    submit_signal(renderer, sync, 1);
    destroy_sync(renderer, sync);
    destroy(renderer, &allocator);
    assert(closes == 4);
    puts("PASS: Venus backend capability rejection, pending fences, timeout, out-of-order completion, reset generation, pending destruction, wait-any/all, monotonic signals, allocation bounds, malformed mappings, blob failure, allocation ordering, mapping placement, device loss, and teardown");
    return 0;
}

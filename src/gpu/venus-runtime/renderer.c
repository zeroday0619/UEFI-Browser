/* SPDX-License-Identifier: BSD-3-Clause */
#include "vn_renderer.h"
#include "vn_device.h"
#include "vn_device_memory.h"
#include "vn_renderer_util.h"
#include "venus_transport.h"

struct firmware_sync {
    struct vn_renderer_sync base;
    uint64_t value;
    uint64_t generation;
    size_t references;
};

struct firmware_signal {
    struct firmware_sync *sync;
    uint64_t value;
    uint64_t generation;
};

struct firmware_submission {
    struct firmware_submission *next;
    uint64_t ticket;
    uint32_t count;
    struct firmware_signal signals[];
};

struct firmware_renderer {
    struct vn_renderer base;
    struct firmware_venus_context *context;
    mtx_t mutex;
    struct firmware_submission *pending;
    bool lost;
};

struct firmware_shmem {
    struct vn_renderer_shmem base;
    struct firmware_venus_blob blob;
};

struct firmware_buffer {
    struct vn_renderer_bo base;
    bool imported;
    struct firmware_venus_blob blob;
};

static void release_sync(struct firmware_sync *sync)
{
    if (!--sync->references)
        free(sync);
}

/* The mutex protects completion publication and references held by submissions.
 * A reset invalidates older signals without invalidating their lifetime pins. */
static void poll_locked(struct firmware_renderer *renderer)
{
    struct firmware_submission **position = &renderer->pending;
    while (*position) {
        struct firmware_submission *submission = *position;
        int result = firmware_venus_poll(renderer->context, submission->ticket);
        if (result < 0)
            renderer->lost = true;
        if (!result) {
            position = &submission->next;
            continue;
        }
        for (uint32_t index = 0; index < submission->count; ++index) {
            struct firmware_signal *signal = &submission->signals[index];
            if (result > 0 && signal->generation == signal->sync->generation &&
                signal->value > signal->sync->value)
                signal->sync->value = signal->value;
            release_sync(signal->sync);
        }
        firmware_venus_release_fence(renderer->context, submission->ticket);
        *position = submission->next;
        free(submission);
    }
}

static VkResult submit(struct vn_renderer *base,
                       const struct vn_renderer_submit_batch *batch)
{
    struct firmware_renderer *renderer = (struct firmware_renderer *)base;
    if (batch->ring_idx >= base->info.max_timeline_count ||
        (batch->cs_size && !batch->cs_data) ||
        (batch->sync_count && (!batch->syncs || !batch->sync_values)))
        return VK_ERROR_DEVICE_LOST;
    for (uint32_t index = 0; index < batch->sync_count; ++index)
        if (!batch->syncs[index])
            return VK_ERROR_DEVICE_LOST;
    size_t bytes;
    if (__builtin_mul_overflow((size_t)batch->sync_count,
                               sizeof(struct firmware_signal), &bytes) ||
        __builtin_add_overflow(bytes, sizeof(struct firmware_submission), &bytes))
        return VK_ERROR_OUT_OF_HOST_MEMORY;
    struct firmware_submission *submission = calloc(1, bytes);
    if (!submission)
        return VK_ERROR_OUT_OF_HOST_MEMORY;
    mtx_lock(&renderer->mutex);
    poll_locked(renderer);
    if (renderer->lost || firmware_venus_submit(renderer->context, batch->cs_data,
            batch->cs_size, batch->ring_idx, &submission->ticket) ||
        !submission->ticket) {
        renderer->lost = true;
        mtx_unlock(&renderer->mutex);
        free(submission);
        return VK_ERROR_DEVICE_LOST;
    }
    submission->count = batch->sync_count;
    for (uint32_t index = 0; index < batch->sync_count; ++index) {
        struct firmware_sync *sync = (struct firmware_sync *)batch->syncs[index];
        ++sync->references;
        submission->signals[index] = (struct firmware_signal){
            sync, batch->sync_values[index], sync->generation,
        };
    }
    submission->next = renderer->pending;
    renderer->pending = submission;
    mtx_unlock(&renderer->mutex);
    return VK_SUCCESS;
}

static VkResult wait_syncs(struct vn_renderer *base,
                           const struct vn_renderer_wait *wait)
{
    struct firmware_renderer *renderer = (struct firmware_renderer *)base;
    if (wait->sync_count && (!wait->syncs || !wait->sync_values))
        return VK_ERROR_DEVICE_LOST;
    for (uint32_t index = 0; index < wait->sync_count; ++index)
        if (!wait->syncs[index])
            return VK_ERROR_DEVICE_LOST;
    uint64_t start = os_time_get_nano();
    for (;;) {
        uint32_t ready = 0;
        mtx_lock(&renderer->mutex);
        poll_locked(renderer);
        bool lost = renderer->lost;
        for (uint32_t index = 0; index < wait->sync_count; ++index)
            ready += ((struct firmware_sync *)wait->syncs[index])->value >=
                     wait->sync_values[index];
        mtx_unlock(&renderer->mutex);
        if (lost)
            return VK_ERROR_DEVICE_LOST;
        if (ready == wait->sync_count || (wait->wait_any && ready))
            return VK_SUCCESS;
        if (wait->timeout != UINT64_MAX &&
            (uint64_t)os_time_get_nano() - start >= wait->timeout)
            return VK_TIMEOUT;
        os_time_sleep(10);
    }
}

static void destroy(struct vn_renderer *base, const VkAllocationCallbacks *alloc)
{
    struct firmware_renderer *renderer = (struct firmware_renderer *)base;
    /* Context teardown must stop host access before pending storage is released. */
    firmware_venus_close(renderer->context);
    while (renderer->pending) {
        struct firmware_submission *submission = renderer->pending;
        renderer->pending = submission->next;
        for (uint32_t index = 0; index < submission->count; ++index)
            release_sync(submission->signals[index].sync);
        free(submission);
    }
    mtx_destroy(&renderer->mutex);
    vk_free(alloc, renderer);
}

static bool create_blob(struct firmware_renderer *renderer, uint64_t identifier,
                         uint64_t size, bool mappable,
                         struct firmware_venus_blob *blob)
{
    if (!size || size > SIZE_MAX - 4095)
        return false;
    uint64_t aligned = (size + 4095) & ~UINT64_C(4095);
    if (firmware_venus_create_blob(renderer->context, identifier, aligned,
                                   mappable, blob))
        return false;
    if (!blob->resource_id || blob->size < aligned || blob->size > SIZE_MAX ||
        (mappable && !blob->mapping)) {
        firmware_venus_destroy_blob(renderer->context, blob);
        return false;
    }
    return true;
}

static struct vn_renderer_shmem *create_shmem(struct vn_renderer *base, size_t size)
{
    struct firmware_shmem *memory = calloc(1, sizeof(*memory));
    if (!memory)
        return NULL;
    if (!create_blob((struct firmware_renderer *)base, 0, size, true, &memory->blob)) {
        free(memory);
        return NULL;
    }
    memory->base = (struct vn_renderer_shmem){
        .refcount = VN_REFCOUNT_INIT(1), .res_id = memory->blob.resource_id,
        .mmap_size = memory->blob.size, .mmap_ptr = memory->blob.mapping,
    };
    return &memory->base;
}

static void destroy_shmem(struct vn_renderer *base, struct vn_renderer_shmem *memory)
{
    struct firmware_shmem *shared = (struct firmware_shmem *)memory;
    firmware_venus_destroy_blob(((struct firmware_renderer *)base)->context,
                                &shared->blob);
    free(shared);
}

static VkResult create_buffer(struct vn_renderer *base,
                               struct vn_renderer_submit_batch *batch,
                               VkDeviceSize size, vn_object_id identifier,
                               VkMemoryPropertyFlags flags,
                               VkExternalMemoryHandleTypeFlags external_handles,
                               struct vn_renderer_bo **output)
{
    if (external_handles && external_handles != VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT)
        return VK_ERROR_INVALID_EXTERNAL_HANDLE;
    if (batch && batch->cs_size) {
        if (batch->ring_idx || batch->sync_count)
            return VK_ERROR_DEVICE_LOST;
        /* Export must follow completion of the host's allocation-ring wait. */
        VkResult result = vn_renderer_submit_simple_sync(base, batch->cs_data,
                                                         batch->cs_size);
        if (result != VK_SUCCESS)
            return result;
    }
    struct firmware_buffer *buffer = calloc(1, sizeof(*buffer));
    if (!buffer)
        return VK_ERROR_OUT_OF_HOST_MEMORY;
    if (!create_blob((struct firmware_renderer *)base, identifier, size,
                     flags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT, &buffer->blob)) {
        free(buffer);
        return VK_ERROR_OUT_OF_DEVICE_MEMORY;
    }
    buffer->base = (struct vn_renderer_bo){
        .refcount = VN_REFCOUNT_INIT(1), .res_id = buffer->blob.resource_id,
        .mmap_size = buffer->blob.size, .mmap_ptr = buffer->blob.mapping,
    };
    *output = &buffer->base;
    return VK_SUCCESS;
}

static bool destroy_buffer(struct vn_renderer *base, struct vn_renderer_bo *memory)
{
    struct firmware_buffer *buffer = (struct firmware_buffer *)memory;
    if (buffer->imported) firmware_venus_detach_blob(((struct firmware_renderer *)base)->context,buffer->base.res_id);
    else firmware_venus_destroy_blob(((struct firmware_renderer *)base)->context,
                                &buffer->blob);
    free(buffer);
    return true;
}

static void *map_buffer(struct vn_renderer *base, struct vn_renderer_bo *buffer,
                        void *placed_address)
{
    (void)base;
    if (placed_address && placed_address != buffer->mmap_ptr)
        return NULL;
    return buffer->mmap_ptr;
}

static void synchronize_buffer(struct vn_renderer *base, struct vn_renderer_bo *buffer,
                                VkDeviceSize offset, VkDeviceSize size)
{
    struct firmware_renderer *renderer = (struct firmware_renderer *)base;
    if (offset > buffer->mmap_size ||
        (size != VK_WHOLE_SIZE && size > buffer->mmap_size - offset)) {
        mtx_lock(&renderer->mutex);
        renderer->lost = true;
        mtx_unlock(&renderer->mutex);
        return;
    }
    /* The transport contract requires coherent CPU and host mappings. */
    atomic_thread_fence(memory_order_seq_cst);
}

static VkResult create_sync(struct vn_renderer *base, uint64_t initial_value,
                            struct vn_renderer_sync **output)
{
    (void)base;
    struct firmware_sync *sync = calloc(1, sizeof(*sync));
    if (!sync)
        return VK_ERROR_OUT_OF_HOST_MEMORY;
    sync->value = initial_value;
    sync->references = 1;
    *output = &sync->base;
    return VK_SUCCESS;
}

static void destroy_sync(struct vn_renderer *base, struct vn_renderer_sync *value)
{
    struct firmware_renderer *renderer = (struct firmware_renderer *)base;
    mtx_lock(&renderer->mutex);
    release_sync((struct firmware_sync *)value);
    mtx_unlock(&renderer->mutex);
}

static VkResult reset_sync(struct vn_renderer *base, struct vn_renderer_sync *value)
{
    struct firmware_renderer *renderer = (struct firmware_renderer *)base;
    struct firmware_sync *sync = (struct firmware_sync *)value;
    mtx_lock(&renderer->mutex);
    sync->value = 0;
    ++sync->generation;
    mtx_unlock(&renderer->mutex);
    return VK_SUCCESS;
}

static VkResult read_sync(struct vn_renderer *base, struct vn_renderer_sync *value,
                          uint64_t *output)
{
    struct firmware_renderer *renderer = (struct firmware_renderer *)base;
    mtx_lock(&renderer->mutex);
    poll_locked(renderer);
    *output = ((struct firmware_sync *)value)->value;
    VkResult result = renderer->lost ? VK_ERROR_DEVICE_LOST : VK_SUCCESS;
    mtx_unlock(&renderer->mutex);
    return result;
}

static VkResult write_sync(struct vn_renderer *base, struct vn_renderer_sync *value,
                           uint64_t counter)
{
    struct firmware_renderer *renderer = (struct firmware_renderer *)base;
    struct firmware_sync *sync = (struct firmware_sync *)value;
    mtx_lock(&renderer->mutex);
    if (counter > sync->value)
        sync->value = counter;
    mtx_unlock(&renderer->mutex);
    return VK_SUCCESS;
}

VkResult vn_renderer_create_uefi(struct vn_instance *instance,
                                 const VkAllocationCallbacks *alloc,
                                 struct vn_renderer **output)
{
    (void)instance;
    *output = NULL;
    struct firmware_renderer *renderer = vk_zalloc(alloc, sizeof(*renderer),
        VN_DEFAULT_ALIGN, VK_SYSTEM_ALLOCATION_SCOPE_INSTANCE);
    if (!renderer)
        return VK_ERROR_OUT_OF_HOST_MEMORY;
    if (mtx_init(&renderer->mutex, mtx_plain) != thrd_success) {
        vk_free(alloc, renderer);
        return VK_ERROR_OUT_OF_HOST_MEMORY;
    }
    struct firmware_venus_capabilities capabilities = {0};
    if (firmware_venus_open(&renderer->context, &capabilities)) {
        mtx_destroy(&renderer->mutex);
        vk_free(alloc, renderer);
        return VK_ERROR_INITIALIZATION_FAILED;
    }
    if (capabilities.wire_format_version != 1 ||
        capabilities.vulkan_version < VK_API_VERSION_1_1 ||
        capabilities.timeline_count < 2 || capabilities.timeline_count > 64) {
        destroy(&renderer->base, alloc);
        return VK_ERROR_INCOMPATIBLE_DRIVER;
    }
    struct vn_renderer_info *info = &renderer->base.info;
    info->pci.vendor_id = 0x1af4;
    info->pci.device_id = 0x1050;
    info->has_timeline_sync = true;
    info->max_timeline_count = capabilities.timeline_count;
    info->wire_format_version = capabilities.wire_format_version;
    info->vk_xml_version = capabilities.vulkan_version;
    info->vk_ext_command_serialization_spec_version = capabilities.command_serialization_version;
    info->vk_mesa_venus_protocol_spec_version = capabilities.protocol_version;
    memcpy(info->vk_extension_mask, capabilities.extension_mask,
           sizeof(info->vk_extension_mask));
    renderer->base.ops = (struct vn_renderer_ops){ destroy, submit, wait_syncs };
    renderer->base.shmem_ops = (struct vn_renderer_shmem_ops){ create_shmem, destroy_shmem };
    renderer->base.bo_ops = (struct vn_renderer_bo_ops){
        .create_from_device_memory = create_buffer, .destroy = destroy_buffer,
        .map = map_buffer, .flush = synchronize_buffer, .invalidate = synchronize_buffer,
    };
    renderer->base.sync_ops = (struct vn_renderer_sync_ops){
        .create = create_sync, .destroy = destroy_sync, .reset = reset_sync,
        .read = read_sync, .write = write_sync,
    };
    *output = &renderer->base;
    return VK_SUCCESS;
}

int firmware_venus_scanout_memory(VkDevice, VkDeviceMemory, uint32_t, uint32_t, uint32_t, uint32_t, uint64_t);
int firmware_venus_scanout_memory(VkDevice device, VkDeviceMemory memory,
                                  uint32_t width, uint32_t height,
                                  uint32_t stride, uint32_t offset, uint64_t modifier)
{
    struct vn_device *logical=vn_device_from_handle(device);
    struct vn_device_memory *allocation=vn_device_memory_from_handle(memory);
    if (!logical || !allocation || !allocation->base_bo) return -1;
    struct firmware_renderer *renderer=(struct firmware_renderer *)logical->renderer;
    return firmware_venus_scanout(renderer->context, allocation->base_bo->res_id,
                                  width,height,stride,offset,modifier);
}

uint64_t firmware_venus_export_image(VkDevice, VkDeviceMemory, const struct firmware_venus_image *);
uint64_t firmware_venus_export_image(VkDevice device, VkDeviceMemory memory, const struct firmware_venus_image *metadata)
{
    struct vn_device *logical=vn_device_from_handle(device);
    struct vn_device_memory *allocation=vn_device_memory_from_handle(memory);
    if (!logical || !allocation || !allocation->base_bo) return 0;
    struct firmware_renderer *renderer=(struct firmware_renderer *)logical->renderer;
    struct firmware_venus_image image=*metadata;
    image.resource=allocation->base_bo->res_id;
    return firmware_venus_publish_image(renderer->context,&image);
}
VkResult firmware_venus_import_bo(struct vn_renderer *, uint32_t, uint64_t, struct vn_renderer_bo **);
VkResult firmware_venus_import_bo(struct vn_renderer *base, uint32_t resource, uint64_t size, struct vn_renderer_bo **output)
{
    struct firmware_buffer *buffer=calloc(1,sizeof(*buffer));
    if (!buffer) return VK_ERROR_OUT_OF_HOST_MEMORY;
    if (firmware_venus_attach_blob(((struct firmware_renderer *)base)->context,resource)) {
        free(buffer); return VK_ERROR_INVALID_EXTERNAL_HANDLE;
    }
    buffer->imported=true;
    buffer->base=(struct vn_renderer_bo){.refcount=VN_REFCOUNT_INIT(1),.res_id=resource,.mmap_size=size};
    *output=&buffer->base;
    return VK_SUCCESS;
}

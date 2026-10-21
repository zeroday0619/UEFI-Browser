/* SPDX-License-Identifier: BSD-3-Clause */
#include "venus_broker.h"
#include "venus_device.h"
#include "renderer_runtime.h"
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct broker_session {
    struct firmware_venus_context *owner;
    uint64_t generation;
};
static struct broker_session sessions[VENUS_BROKER_CONTEXT_LIMIT];
static uint64_t generation, charged;
static uint64_t blob_owners[VENUS_BROKER_BLOB_LIMIT];
static struct firmware_venus_blob blobs[VENUS_BROKER_BLOB_LIMIT];
static struct image_capability {
    uint64_t token, owner;
    unsigned references;
    struct firmware_venus_image image;
} images[64];
static uint64_t image_generation, image_bytes;

void firmware_venus_broker_release_image(uint64_t token)
{
    for (unsigned index=0; index<64; ++index) {
        struct image_capability *image=&images[index];
        if (!token || image->token!=token) continue;
        if (!--image->references) {
            firmware_venus_device_release_blob(image->image.resource);
            image_bytes-=image->image.size;
            memset(image,0,sizeof(*image));
        }
        return;
    }
}
int firmware_venus_broker_acquire_image(uint64_t token, struct firmware_venus_image *output)
{
    for (unsigned index=0; index<64; ++index) {
        struct image_capability *image=&images[index];
        if (!token || image->token!=token || image->references>=4096) continue;
        ++image->references; *output=image->image; return 0;
    }
    return -1;
}

static void revoke(struct firmware_venus_blob *blob)
{
    if (blob->mapping && !renderer_runtime_device_grant((uintptr_t)blob->mapping, blob->size, 0)) {
        puts("FAIL: Venus device mapping revocation; resources retained");
        abort();
    }
}
static void close_session(struct broker_session *session)
{
    for (unsigned index=0; index<64; ++index) if (images[index].owner==session->generation) {
        images[index].owner=0;
        firmware_venus_broker_release_image(images[index].token);
    }
    for (unsigned index=0; index<VENUS_BROKER_BLOB_LIMIT; ++index) {
        if (blob_owners[index] != session->generation) continue;
        revoke(&blobs[index]);
        charged-=blobs[index].size;
        memset(&blobs[index], 0, sizeof(blobs[index]));
        blob_owners[index]=0;
    }
    firmware_venus_device_close(session->owner);
    memset(session, 0, sizeof(*session));
}
void firmware_venus_reset(void)
{
    /* Revoke all renderer mappings before any final device reset releases DMA. */
    for (unsigned index=0; index<VENUS_BROKER_BLOB_LIMIT; ++index) {
        if (blobs[index].resource_id) revoke(&blobs[index]);
        blobs[index].mapping=NULL;
    }
    for (unsigned index=0; index<VENUS_BROKER_CONTEXT_LIMIT; ++index)
        if (sessions[index].owner) close_session(&sessions[index]);
}
long firmware_venus_broker(const uint64_t arguments[5])
{
    const uint64_t operation=arguments[0], session=arguments[1];
    if (operation==VENUS_RELEASE_IMAGE) {
        for (unsigned index=0; index<64; ++index) if (session && images[index].token==session && images[index].owner) {
            images[index].owner=0; firmware_venus_broker_release_image(session); return 0;
        }
        return -EBADF;
    }
    if (operation == VENUS_OPEN) {
        if (session != VENUS_BROKER_VERSION || generation == INT64_MAX ||
            !renderer_runtime_buffer(arguments[2], sizeof(struct firmware_venus_capabilities), 1))
            return -EINVAL;
        unsigned slot=0;
        while (slot<VENUS_BROKER_CONTEXT_LIMIT && sessions[slot].owner) ++slot;
        if (slot==VENUS_BROKER_CONTEXT_LIMIT) return -EMFILE;
        struct firmware_venus_context *owner=NULL;
        struct firmware_venus_capabilities capabilities;
        if (firmware_venus_device_open(&owner, &capabilities)) return -ENODEV;
        uint64_t active=++generation;
        sessions[slot]=(struct broker_session){owner, active};
        memcpy((void *)arguments[2], &capabilities, sizeof(capabilities));
        return active;
    }
    struct broker_session *current=NULL;
    for (unsigned index=0; index<VENUS_BROKER_CONTEXT_LIMIT; ++index)
        if (session && sessions[index].generation==session) current=&sessions[index];
    if (!current) return -EBADF;
    struct firmware_venus_context *owner=current->owner;
    switch (operation) {
    case VENUS_PUBLISH_IMAGE: {
        if (!renderer_runtime_buffer(arguments[2],sizeof(struct firmware_venus_image),0) || image_generation==INT64_MAX) return -EINVAL;
        struct firmware_venus_image image;
        memcpy(&image,(const void *)arguments[2],sizeof(image));
        /* RGBA8 and BGRA8 have identical bounded four-byte texels. Imported
         * Vulkan requirements are checked again before binding the allocation. */
        if (!image.width || !image.height || image.width>4096 || image.height>4096 ||
            (image.format!=37 && image.format!=44 && image.format!=97) || image.alpha>63 || ((image.alpha&4) && !(image.alpha&1)) || image.memory_type>=32 ||
            !image.size || image.size>128*1024*1024-image_bytes || image.size<(uint64_t)image.width*image.height*(image.format==97 ? 8 : 4)) return -EINVAL;
        unsigned blob=0,slot=0;
        while (blob<VENUS_BROKER_BLOB_LIMIT && blobs[blob].resource_id!=image.resource) ++blob;
        while (slot<64 && images[slot].token) ++slot;
        if (blob==VENUS_BROKER_BLOB_LIMIT || blob_owners[blob]!=session || blobs[blob].mapping ||
            image.size>blobs[blob].size || slot==64) return -EINVAL;
        if (firmware_venus_device_retain_blob(owner,image.resource)) return -EIO;
        uint64_t token=++image_generation;
        images[slot]=(struct image_capability){token,session,1,image};
        image_bytes+=image.size;
        return (long)token;
    }
    case VENUS_CLOSE:
        close_session(current);
        return 0;
    case VENUS_SUBMIT: {
        size_t size=arguments[3];
        if (size > 65504 || arguments[4] >= 64 ||
            !renderer_runtime_buffer(arguments[2], size, 0)) return -EINVAL;
        void *command=malloc(size ? size : 1);
        if (!command) return -ENOMEM;
        if (size) memcpy(command, (const void *)arguments[2], size);
        uint64_t ticket=0;
        int result=firmware_venus_device_submit(owner, command, size, arguments[4], &ticket);
        free(command);
        return result || ticket > INT64_MAX ? -EIO : (long)ticket;
    }
    case VENUS_POLL:
        return firmware_venus_device_poll(owner, arguments[2]);
    case VENUS_RELEASE:
        if (firmware_venus_device_poll(owner, arguments[2]) != 1) return -EINVAL;
        firmware_venus_device_release_fence(owner, arguments[2]);
        return 0;
    case VENUS_CREATE_BLOB: {
        if (!renderer_runtime_buffer(arguments[2], sizeof(VenusBlobMessage), 1)) return -EFAULT;
        VenusBlobMessage message;
        memcpy(&message, (const void *)arguments[2], sizeof(message));
        if (message.version != VENUS_BROKER_VERSION || message.mappable > 1 ||
            message.reserved || message.mapping || message.resource || !message.size ||
            (message.size & 4095) || message.size > VENUS_BROKER_MEMORY_LIMIT - charged)
            return -EINVAL;
        unsigned slot=0;
        while (slot < VENUS_BROKER_BLOB_LIMIT && blobs[slot].resource_id) ++slot;
        if (slot == VENUS_BROKER_BLOB_LIMIT) return -ENOMEM;
        struct firmware_venus_blob value;
        if (firmware_venus_device_create_blob(owner, message.identifier, message.size,
                                               message.mappable, &value)) return -EIO;
        if (value.mapping && !renderer_runtime_device_grant((uintptr_t)value.mapping, value.size, value.cache_type)) {
            firmware_venus_device_destroy_blob(owner, &value);
            return -ENOMEM;
        }
        blobs[slot]=value;
        blob_owners[slot]=session;
        charged+=value.size;
        message.mapping=(uintptr_t)value.mapping;
        message.resource=value.resource_id;
        memcpy((void *)arguments[2], &message, sizeof(message));
        return 0;
    }
    case VENUS_DESTROY_BLOB: {
        unsigned slot=0;
        while (slot < VENUS_BROKER_BLOB_LIMIT && blobs[slot].resource_id != arguments[2]) ++slot;
        if (!arguments[2] || slot == VENUS_BROKER_BLOB_LIMIT || blob_owners[slot] != session) return -EBADF;
        revoke(&blobs[slot]);
        charged-=blobs[slot].size;
        firmware_venus_device_destroy_blob(owner, &blobs[slot]);
        memset(&blobs[slot], 0, sizeof(blobs[slot]));
        blob_owners[slot]=0;
        return 0;
    }
#ifdef ENABLE_VENUS_RUNTIME
    case VENUS_PRESENT: {
        /* The isolated preview accepts a fixed image, never framebuffer addresses. */
        if (arguments[3] != 128 || !renderer_runtime_buffer(arguments[2], 65536, 0)) return -EINVAL;
        uint8_t *pixels=malloc(65536);
        if (!pixels) return -ENOMEM;
        memcpy(pixels, (const void *)arguments[2], 65536);
        firmware_venus_device_present_image(pixels, 128);
        free(pixels);
        return 0;
    }
 #endif
    default: return -ENOSYS;
    }
}

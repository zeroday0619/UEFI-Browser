/* SPDX-License-Identifier: BSD-3-Clause */
#include <assert.h>
#include "x86_page_cache.h"
#include "../src/gpu/venus-runtime/broker.c"
struct firmware_venus_context { int open; } contexts[VENUS_BROKER_CONTEXT_LIMIT];
static struct firmware_venus_context *backing_owners[VENUS_BROKER_BLOB_LIMIT];
static struct firmware_venus_context *ticket_owner;
static _Alignas(8) unsigned char user_memory[4096];
static unsigned creates, destroys, closes, grants, revocations, submissions;
static struct firmware_venus_blob backing[VENUS_BROKER_BLOB_LIMIT];
static unsigned next_resource, retained_images;
int firmware_venus_device_retain_blob(struct firmware_venus_context *context,uint32_t resource) {
    for (unsigned index=0; index<VENUS_BROKER_BLOB_LIMIT; ++index)
        if (backing[index].resource_id==resource && backing_owners[index]==context) { ++retained_images; return 0; }
    return -1;
}
void firmware_venus_device_release_blob(uint32_t resource) { assert(resource && retained_images); --retained_images; }
static int refuse_grant;
int renderer_runtime_buffer(uint64_t address, size_t size, unsigned writable) {
    (void)writable;
    return address >= (uintptr_t)user_memory && address - (uintptr_t)user_memory <= sizeof(user_memory) &&
           size <= sizeof(user_memory) - (address - (uintptr_t)user_memory);
}
int renderer_runtime_device_grant(uint64_t address, size_t size, unsigned cache_type) {
    assert(address && size && !(size & 4095));
    if (cache_type) { if (refuse_grant) return 0; ++grants; }
    else ++revocations;
    return 1;
}
int firmware_venus_device_open(struct firmware_venus_context **output, struct firmware_venus_capabilities *caps) {
    unsigned slot=0; while (slot<VENUS_BROKER_CONTEXT_LIMIT && contexts[slot].open) ++slot;
    assert(slot<VENUS_BROKER_CONTEXT_LIMIT);
    contexts[slot].open=1; *output=&contexts[slot]; memset(caps, 0, sizeof(*caps)); return 0;
}
void firmware_venus_device_close(struct firmware_venus_context *value) {
    assert(value && value->open);
    for (unsigned index=0; index<VENUS_BROKER_BLOB_LIMIT; ++index) {
        if (backing_owners[index] != value) continue;
        free(backing[index].mapping); memset(&backing[index], 0, sizeof(backing[index]));
        backing_owners[index]=NULL;
    }
    value->open=0; ++closes;
}
int firmware_venus_device_create_blob(struct firmware_venus_context *value, uint64_t identifier,
    uint64_t size, int mappable, struct firmware_venus_blob *output) {
    assert(value && value->open && size <= VENUS_BROKER_MEMORY_LIMIT);
    (void)identifier;
    unsigned slot=0; while (backing[slot].resource_id) ++slot;
    *output=(struct firmware_venus_blob){.resource_id=++next_resource, .cache_type=1, .size=size,
                                        .mapping=mappable ? malloc(4096) : NULL};
    backing[slot]=*output; backing_owners[slot]=value; ++creates; return 0;
}
void firmware_venus_device_destroy_blob(struct firmware_venus_context *value, struct firmware_venus_blob *blob) {
    assert(value && value->open);
    unsigned slot=0; while (backing[slot].resource_id != blob->resource_id) ++slot;
    assert(backing_owners[slot]==value); backing_owners[slot]=NULL;
    free(backing[slot].mapping); memset(&backing[slot], 0, sizeof(backing[slot]));
    memset(blob, 0, sizeof(*blob)); ++destroys;
}
int firmware_venus_device_submit(struct firmware_venus_context *value, const void *data,
    size_t size, uint32_t timeline, uint64_t *ticket) {
    assert(value && value->open && size == 4 && timeline == 1);
    assert(data != user_memory && !memcmp(data, user_memory, size));
    ticket_owner=value; *ticket=++submissions; return 0;
}
int firmware_venus_device_poll(struct firmware_venus_context *value, uint64_t ticket) {
    assert(value && value->open); return ticket_owner != value ? -1 : ticket == 1 ? 1 : ticket == 2 ? 0 : -1;
}
void firmware_venus_device_release_fence(struct firmware_venus_context *value, uint64_t ticket) {
    assert(value && value->open && ticket == 1);
}
static long call(unsigned operation, uint64_t handle, uint64_t first, uint64_t second, uint64_t third) {
    const uint64_t arguments[5]={operation, handle, first, second, third};
    return firmware_venus_broker(arguments);
}
static void page_cache_cases(void) {
    uint64_t flags=0;
    const uint64_t firmware_pat=UINT64_C(0x0007040600070406);
    assert(x86_device_page_cache(firmware_pat, 1, &flags) && flags==0);
    assert(x86_device_page_cache(firmware_pat, 2, &flags) && flags==24);
    assert(x86_device_page_cache(firmware_pat, 3, &flags) && flags==24);
    for (unsigned index=0; index<8; ++index) {
        uint64_t pat=UINT64_C(0x0707070707070707);
        pat=(pat & ~(UINT64_C(255)<<(index*8))) | (UINT64_C(1)<<(index*8));
        assert(x86_device_page_cache(pat, 3, &flags));
        assert((((flags>>3)&3)|((flags>>5)&4))==index);
    }
    assert(!x86_device_page_cache(UINT64_MAX, 3, &flags));
    assert(!x86_device_page_cache(firmware_pat, 0, &flags));
    assert(!x86_device_page_cache(firmware_pat, 4, &flags));
    puts("PASS: device PAT cache selection, WC fallback and invalid cache rejection");
}
int main(void) {
    page_cache_cases();
    assert(call(VENUS_OPEN, 1, 1, 0, 0) == -EINVAL);
    long handle=call(VENUS_OPEN, 1, (uintptr_t)user_memory, 0, 0); assert(handle > 0);
    long sibling=call(VENUS_OPEN, 1, (uintptr_t)user_memory, 0, 0);
    assert(sibling>handle);
    assert(call(VENUS_CLOSE, sibling+1, 0, 0, 0) == -EBADF && !closes);
    assert(call(VENUS_SUBMIT, handle, 1, 4, 1) == -EINVAL);
    assert(call(VENUS_SUBMIT, handle, (uintptr_t)user_memory, 65505, 1) == -EINVAL);
    assert(call(VENUS_SUBMIT, handle, (uintptr_t)user_memory, 4, 64) == -EINVAL);
    assert(call(VENUS_SUBMIT, handle, (uintptr_t)user_memory, 4, 1) == 1 && submissions == 1);
    assert(call(VENUS_POLL, sibling, 1, 0, 0)==-1);
    assert(call(VENUS_RELEASE, sibling, 1, 0, 0)==-EINVAL);
    assert(call(VENUS_RELEASE, handle, 2, 0, 0) == -EINVAL);
    assert(call(VENUS_RELEASE, handle, UINT64_MAX, 0, 0) == -EINVAL);
    assert(call(VENUS_RELEASE, handle, 1, 0, 0) == 0);
    VenusBlobMessage *message=(void *)user_memory;
    *message=(VenusBlobMessage){.version=1, .size=4096, .mappable=1};
    refuse_grant=1;
    assert(call(VENUS_CREATE_BLOB, handle, (uintptr_t)message, 0, 0) == -ENOMEM);
    assert(creates == 1 && destroys == 1 && charged == 0);
    refuse_grant=0;
    message->size=VENUS_BROKER_MEMORY_LIMIT;
    assert(!call(VENUS_CREATE_BLOB, handle, (uintptr_t)message, 0, 0));
    assert(charged == VENUS_BROKER_MEMORY_LIMIT && grants == 1);
    unsigned resource=message->resource;
    assert(call(VENUS_DESTROY_BLOB, sibling, resource, 0, 0)==-EBADF);
    *message=(VenusBlobMessage){.version=1, .size=4096, .mappable=1};
    assert(call(VENUS_CREATE_BLOB, handle, (uintptr_t)message, 0, 0) == -EINVAL);
    assert(call(VENUS_CREATE_BLOB, sibling, (uintptr_t)message, 0, 0) == -EINVAL);
    assert(call(VENUS_DESTROY_BLOB, handle, resource+1, 0, 0) == -EBADF);
    assert(!call(VENUS_DESTROY_BLOB, handle, resource, 0, 0));
    assert(charged == 0 && revocations == 1);
    assert(call(VENUS_DESTROY_BLOB, handle, resource, 0, 0) == -EBADF);
    assert(!call(VENUS_CREATE_BLOB, handle, (uintptr_t)message, 0, 0));
    assert(call(VENUS_PRESENT, handle, (uintptr_t)user_memory, 128, 0) == -ENOSYS);
    assert(!call(VENUS_CLOSE, sibling, 0, 0, 0));
    assert(charged==4096 && contexts[0].open);
    firmware_venus_reset(); assert(closes == 2 && charged == 0 && revocations == 2);
    long next=call(VENUS_OPEN, 1, (uintptr_t)user_memory, 0, 0); assert(next > handle);
    assert(call(VENUS_POLL, handle, 1, 0, 0) == -EBADF);
    assert(!call(VENUS_CLOSE, next, 0, 0, 0));
    long handles[VENUS_BROKER_CONTEXT_LIMIT];
    for (unsigned index=0; index<VENUS_BROKER_CONTEXT_LIMIT; ++index) {
        handles[index]=call(VENUS_OPEN, 1, (uintptr_t)user_memory, 0, 0);
        assert(handles[index]>next);
    }
    assert(call(VENUS_OPEN, 1, (uintptr_t)user_memory, 0, 0)==-EMFILE);
    assert(!call(VENUS_CLOSE, handles[3], 0, 0, 0));
    long replacement=call(VENUS_OPEN, 1, (uintptr_t)user_memory, 0, 0);
    assert(replacement>handles[VENUS_BROKER_CONTEXT_LIMIT-1]);
    assert(call(VENUS_POLL, handles[3], 1, 0, 0)==-EBADF);
    firmware_venus_reset();
    assert(charged==0);
    long owner=call(VENUS_OPEN,1,(uintptr_t)user_memory,0,0);
    *message=(VenusBlobMessage){.version=1,.size=4096};
    assert(!call(VENUS_CREATE_BLOB,owner,(uintptr_t)message,0,0));
    resource=message->resource;
    struct firmware_venus_image *image=(void *)user_memory;
    *image=(struct firmware_venus_image){.width=16,.height=16,.format=37,.alpha=1,.size=4096,.resource=resource};
    assert(call(VENUS_PUBLISH_IMAGE,owner,1,0,0)==-EINVAL);
    image->width=4097;
    assert(call(VENUS_PUBLISH_IMAGE,owner,(uintptr_t)image,0,0)==-EINVAL);
    image->width=16; image->resource++;
    assert(call(VENUS_PUBLISH_IMAGE,owner,(uintptr_t)image,0,0)==-EINVAL);
    image->resource=resource;
    struct firmware_venus_image valid_image=*image;
    for (unsigned mutation=0; mutation<8; ++mutation) {
        *image=valid_image;
        switch (mutation) {
        case 0: image->width=0; break;
        case 1: image->height=4097; break;
        case 2: image->format=0; break;
        case 3: image->alpha=64; break;
        case 4: image->alpha=4; break;
        case 5: image->memory_type=32; break;
        case 6: image->size=1023; break;
        case 7: image->format=97; image->size=1024; break;
        }
        assert(call(VENUS_PUBLISH_IMAGE,owner,(uintptr_t)image,0,0)==-EINVAL);
        assert(!retained_images && !image_bytes);
    }
    *image=valid_image;
    long token=call(VENUS_PUBLISH_IMAGE,owner,(uintptr_t)image,0,0);
    assert(token>0 && retained_images==1 && image_bytes==4096);
    struct firmware_venus_image metadata;
    assert(!firmware_venus_broker_acquire_image(token,&metadata));
    assert(metadata.resource==resource);
    assert(!call(VENUS_RELEASE_IMAGE,token,0,0,0));
    assert(call(VENUS_RELEASE_IMAGE,token,0,0,0)==-EBADF);
    long automatically_closed=call(VENUS_PUBLISH_IMAGE,owner,(uintptr_t)image,0,0);
    assert(automatically_closed>token);
    assert(!firmware_venus_broker_acquire_image(automatically_closed,&metadata));
    assert(!call(VENUS_CLOSE,owner,0,0,0));
    assert(retained_images==2);
    firmware_venus_broker_release_image(token);
    firmware_venus_broker_release_image(automatically_closed);
    assert(!retained_images && !image_bytes);
    assert(firmware_venus_broker_acquire_image(token,&metadata)==-1);
    puts("PASS: image ownership, dimensions, stale tokens and consumer lifetime after producer close");
    puts("PASS: 16 independent sessions, shared quota, foreign resource and fence denial, stale generations and sibling survival");
    puts("PASS: Venus broker copies, capabilities, generations, quota, resource ownership, failed grants, teardown and disabled browser preview");
}

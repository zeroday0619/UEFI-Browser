/* SPDX-License-Identifier: BSD-3-Clause */
#include "uefi.h"
#include "venus_transport.h"
#include "virtio/virtio-gpu/venus_hw.h"
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <threads.h>
#include <time.h>

extern SystemTable *platform_firmware(void);
extern void graphics_log(const char *);

#define CONTEXT_LIMIT 16
#define QUEUE_SIZE 64
#define RESPONSE_SIZE 4096
#define MAX_COMMAND_SIZE 65536
#define CONTROL_TIMEOUT_NS UINT64_C(5000000000)
struct QueueDescriptor { uint64_t address; uint32_t length; uint16_t flags, next; };
struct AvailableRing { uint16_t flags, index, entries[QUEUE_SIZE]; };
struct UsedEntry { uint32_t identifier, length; };
struct UsedRing { uint16_t flags, index; struct UsedEntry entries[QUEUE_SIZE]; };
struct Command { uint32_t type, flags; uint64_t fence; uint32_t context, padding, first, second; };
_Static_assert(sizeof(struct Command) == 32, "Virtio GPU command layout");
struct transport_job {
    struct transport_job *next;
    uint64_t ticket, storage;
    size_t pages;
    uint32_t context, expected, response_length;
    int state;
    unsigned char response[RESPONSE_SIZE];
};
struct transport_blob {
    struct transport_blob *next;
    struct firmware_venus_blob public;
    uint64_t offset;
    uint32_t context;
    uint32_t references;
    uint32_t attachments[CONTEXT_LIMIT];
};
struct firmware_venus_context { uint32_t identifier; };
struct transport_device {
    mtx_t mutex;
    uint64_t queue_storage, next_ticket, completed;
    uint32_t next_resource, next_context, context_count, scanout_resource;
    int scanout_modifiers;
    struct firmware_venus_context contexts[CONTEXT_LIMIT];
    struct firmware_venus_capabilities capabilities;
    uint16_t submitted, consumed;
    int initialized, lost, owned;
    Handle controller;
    int firmware_detached;
    struct transport_job *jobs, *slots[QUEUE_SIZE / 2];
    struct transport_blob *blobs;
};
static struct transport_device driver;
static unsigned claimed;
static SystemTable *firmware;
static volatile uint8_t *common, *device_config, *notification;
static uint32_t notification_length, notification_multiplier;
static uint64_t host_visible_base, host_visible_size;
static uint32_t device, saved_command;
static volatile struct QueueDescriptor *descriptors;
static volatile struct AvailableRing *available;
static volatile struct UsedRing *used;
#include "pci.inc"

/* Firmware drivers must stop using the queue before the application takes ownership. */
static int detach_firmware(void)
{
    Guid pci_identifier={0x4cf5b200,0x68b8,0x4ca5,{0x9e,0xec,0xb2,0x3e,0x3f,0x50,0x02,0x9a}};
    size_t count=0;
    Handle *handles=NULL;
    Status result=((Status(UEFI_API *)(uint32_t,Guid*,void*,size_t*,Handle**))
        firmware->boot->function[36])(2,&pci_identifier,NULL,&count,&handles);
    if (result || !handles || count>4096) return -1;
    for (size_t index=0; index<count; ++index) {
        void **protocol=NULL;
        result=((Status(UEFI_API *)(Handle,Guid*,void**))firmware->boot->function[16])(
            handles[index],&pci_identifier,(void**)&protocol);
        if (result || !protocol) continue;
        size_t segment,bus,slot,function;
        result=((Status(UEFI_API *)(void*,size_t*,size_t*,size_t*,size_t*))protocol[14])(
            protocol,&segment,&bus,&slot,&function);
        if (!result && !segment && bus<256 && slot<32 && function<8 &&
            device==(0x80000000u|(bus<<16)|(slot<<11)|(function<<8))) {
            driver.controller=handles[index];
            break;
        }
    }
    ((Status(UEFI_API *)(void*))firmware->boot->function[6])(handles);
    if (!driver.controller) return -1;
    result=((Status(UEFI_API *)(Handle,Handle,Handle))firmware->boot->function[31])(
        driver.controller,NULL,NULL);
    if (result) return -1;
    driver.firmware_detached=1;
    graphics_log("VENUS TRANSPORT: firmware controller disconnected\n");
    return 0;
}
static void restore_firmware(void)
{
    if (!driver.firmware_detached) return;
    Status result=((Status(UEFI_API *)(Handle,Handle*,void*,uint8_t))firmware->boot->function[30])(
        driver.controller,NULL,NULL,1);
    if (result) {
        printf("FAIL: Venus firmware controller reconnect status=%llx\n", (unsigned long long)result);
        abort();
    }
    driver.firmware_detached=0;
    graphics_log("PASS: Venus firmware controller restored\n");
}

static uint64_t now(void)
{
    struct timespec value;
    clock_gettime(CLOCK_MONOTONIC, &value);
    return (uint64_t)value.tv_sec * 1000000000 + value.tv_nsec;
}
static void stall(void)
{
    ((Status(UEFI_API *)(size_t))firmware->boot->function[28])(100);
}
static uint64_t allocate_pages(size_t pages)
{
    uint64_t address_value = 0;
    Status result = ((Status(UEFI_API *)(uint32_t,uint32_t,size_t,uint64_t *))
        firmware->boot->function[2])(0, 2, pages, &address_value);
    if (result) return 0;
    memset((void *)(uintptr_t)address_value, 0, pages * 4096);
    return address_value;
}
static void free_pages(uint64_t address_value, size_t pages)
{
    if (address_value && ((Status(UEFI_API *)(uint64_t,size_t))
        firmware->boot->function[3])(address_value, pages)) {
        graphics_log("FAIL: Venus transport page release\n");
        abort();
    }
}
static int reset_device(void)
{
    common[20] = 0;
    uint64_t deadline = now() + CONTROL_TIMEOUT_NS;
    while (common[20]) {
        if (now() >= deadline) return -1;
        stall();
    }
    return 0;
}

/* Used entries may complete out of submission order across Vulkan timelines.
 * Descriptor storage is released only after its exact fence and context match. */
static void drain(void)
{
    if (driver.lost) return;
    if (common[20] & 64) { driver.lost = 1; return; }
    uint16_t end = used->index;
    __atomic_thread_fence(__ATOMIC_ACQUIRE);
    if ((uint16_t)(end - driver.consumed) > QUEUE_SIZE / 2) {
        driver.lost = 1;
        return;
    }
    while (driver.consumed != end) {
        struct UsedEntry entry = used->entries[driver.consumed % QUEUE_SIZE];
        if (entry.identifier >= QUEUE_SIZE || (entry.identifier & 1)) {
            driver.lost = 1;
            return;
        }
        unsigned slot = entry.identifier / 2;
        struct transport_job *job = driver.slots[slot];
        if (!job || entry.length < 24 || entry.length > RESPONSE_SIZE) {
            driver.lost = 1;
            return;
        }
        const struct Command *response = (const void *)(uintptr_t)
            (job->storage + (job->pages - 1) * 4096);
        if (!(response->flags & 1) || response->fence != job->ticket ||
            response->context != job->context) {
            driver.lost = 1;
            return;
        }
        job->state = response->type == job->expected ? 1 : -1;
        if (job->state < 0) {
            printf("VENUS TRANSPORT: response=%x expected=%x fence=%llu\n",
                   response->type, job->expected, (unsigned long long)job->ticket);
            driver.lost = 1;
        }
        memcpy(job->response, response, entry.length);
        job->response_length = entry.length;
        free_pages(job->storage, job->pages);
        job->storage = 0;
        driver.slots[slot] = NULL;
        ++driver.consumed;
        ++driver.completed;
    }
}
static struct transport_job *enqueue(const void *command_data, size_t size,
                                      uint32_t expected)
{
    if (size < 24 || size > MAX_COMMAND_SIZE || driver.lost ||
        driver.next_ticket == UINT64_MAX) return NULL;
    uint64_t deadline = now() + CONTROL_TIMEOUT_NS;
    unsigned slot;
    for (;;) {
        drain();
        if (driver.lost) return NULL;
        for (slot = 0; slot < QUEUE_SIZE / 2 && driver.slots[slot]; ++slot) {}
        if (slot < QUEUE_SIZE / 2) break;
        if (now() >= deadline) { driver.lost = 1; return NULL; }
        stall();
    }
    struct transport_job *job = calloc(1, sizeof(*job));
    if (!job) return NULL;
    job->pages = (size + 4095) / 4096 + 1;
    job->storage = allocate_pages(job->pages);
    if (!job->storage) { free(job); return NULL; }
    struct Command *request = (void *)(uintptr_t)job->storage;
    memcpy(request, command_data, size);
    request->flags = request->type == 0x207 ? 3 : 1;
    request->fence = ++driver.next_ticket;
    job->ticket = request->fence;
    job->context = request->context;
    job->expected = expected;
    job->next = driver.jobs;
    driver.jobs = job;
    driver.slots[slot] = job;
    unsigned descriptor = slot * 2;
    descriptors[descriptor] = (struct QueueDescriptor){
        job->storage, size, 1, descriptor + 1,
    };
    descriptors[descriptor + 1] = (struct QueueDescriptor){
        job->storage + (job->pages - 1) * 4096, RESPONSE_SIZE, 2, 0,
    };
    available->entries[driver.submitted % QUEUE_SIZE] = descriptor;
    __atomic_thread_fence(__ATOMIC_RELEASE);
    available->index = ++driver.submitted;
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
    uint64_t offset = (uint64_t)read16(common, 30) * notification_multiplier;
    if (offset + 2 > notification_length) { driver.lost = 1; return NULL; }
    write16(notification, offset, 0);
    return job;
}
static void release_job(struct transport_job *job)
{
    struct transport_job **position = &driver.jobs;
    while (*position && *position != job) position = &(*position)->next;
    if (!*position || job->storage) { driver.lost = 1; return; }
    *position = job->next;
    free(job);
}
static int control(const void *request, size_t size, uint32_t expected,
                    void *response, size_t response_size)
{
    struct transport_job *job = enqueue(request, size, expected);
    if (!job) return -1;
    uint64_t deadline = now() + CONTROL_TIMEOUT_NS;
    while (!job->state && !driver.lost) {
        drain();
        if (now() >= deadline) { driver.lost = 1; break; }
        if (!job->state) stall();
    }
    int result = job->state == 1 && !driver.lost ? 0 : -1;
    if (!result && response) {
        if (response_size > job->response_length) result = -1;
        else memcpy(response, job->response, response_size);
    }
    if (!job->storage) release_job(job);
    return result;
}
static int simple(uint32_t type, uint32_t resource, uint32_t context)
{
    const struct Command request = {.type=type, .context=context, .first=resource};
    return control(&request, type == 0x201 ? 24 : sizeof(request), 0x1100, NULL, 0);
}

void firmware_venus_dump(void)
{
    static int dumping;
    if (!driver.initialized || !used || !common || dumping) return;
    dumping = 1;
    drain();
    printf("VENUS TRANSPORT: diagnostic lost=%d available=%u used=%u consumed=%u\n",
           driver.lost, driver.submitted, used->index, driver.consumed);
    for (struct transport_job *job = driver.jobs; job; job = job->next)
        printf("VENUS TRANSPORT: pending=%llu state=%d bytes=%u\n",
               (unsigned long long)job->ticket, job->state, job->response_length);
    for (struct transport_blob *blob = driver.blobs; blob; blob = blob->next) {
        if (!blob->public.mapping) continue;
        volatile uint32_t *words = blob->public.mapping;
        printf("VENUS TRANSPORT: shared=%u words=%x,%x,%x,%x,%x,%x,%x,%x\n",
               blob->public.resource_id, words[0],words[1],words[2],words[3],
               words[4],words[5],words[6],words[7]);
    }
}

/* The broker serializes entry and keeps closed session generations unreachable. */
static int valid_context(const struct firmware_venus_context *context)
{
    if (!driver.initialized) return 0;
    for (unsigned index=0; index<CONTEXT_LIMIT; ++index)
        if (context == &driver.contexts[index]) return context->identifier != 0;
    return 0;
}

int firmware_venus_submit(struct firmware_venus_context *context,
                          const void *command_data, size_t size, uint32_t timeline,
                          uint64_t *ticket)
{
    if (!valid_context(context) || !ticket || timeline >= 64 ||
        size > MAX_COMMAND_SIZE - 32 || (size && !command_data)) return -1;
    struct Command *request = calloc(1, 32 + size);
    if (!request) return -1;
    *request = (struct Command){.type=0x207, .context=context->identifier,
                                .padding=timeline, .first=size};
    if (size) memcpy((char *)request + 32, command_data, size);
    mtx_lock(&driver.mutex);
    struct transport_job *job = enqueue(request, 32 + size, 0x1100);
    if (job) *ticket = job->ticket;
    mtx_unlock(&driver.mutex);
    free(request);
    return job ? 0 : -1;
}
int firmware_venus_poll(struct firmware_venus_context *context, uint64_t ticket)
{
    if (!valid_context(context) || !ticket) return -1;
    mtx_lock(&driver.mutex);
    drain();
    struct transport_job *job = driver.jobs;
    while (job && job->ticket != ticket) job = job->next;
    int result = driver.lost || !job || job->context != context->identifier ? -1 : job->state;
    mtx_unlock(&driver.mutex);
    return result;
}
void firmware_venus_release_fence(struct firmware_venus_context *context, uint64_t ticket)
{
    if (!valid_context(context)) return;
    mtx_lock(&driver.mutex);
    struct transport_job *job = driver.jobs;
    while (job && job->ticket != ticket) job = job->next;
    /* Failed DMA is quarantined until context reset, including pending tickets. */
    if (job && job->context == context->identifier) {
        if (job->state && !job->storage) release_job(job);
        else driver.lost = 1;
    }
    mtx_unlock(&driver.mutex);
}

static int reserve_mapping(uint64_t size, uint64_t *offset)
{
    uint64_t candidate = 0;
    for (;;) {
        if (candidate > host_visible_size || size > host_visible_size - candidate)
            return -1;
        int overlap = 0;
        for (struct transport_blob *blob = driver.blobs; blob; blob = blob->next) {
            if (blob->public.mapping && candidate < blob->offset + blob->public.size &&
                blob->offset < candidate + size) {
                candidate = blob->offset + blob->public.size;
                overlap = 1;
                break;
            }
        }
        if (!overlap) { *offset = candidate; return 0; }
    }
}
int firmware_venus_create_blob(struct firmware_venus_context *context,
                               uint64_t identifier, uint64_t size, int mappable,
                               struct firmware_venus_blob *output)
{
    if (!valid_context(context) || !output || !size ||
        (size & 4095) || (!identifier && !mappable)) return -1;
    struct transport_blob *blob = calloc(1, sizeof(*blob));
    if (!blob) return -1;
    mtx_lock(&driver.mutex);
    if (driver.lost || driver.next_resource == UINT32_MAX ||
        (mappable && reserve_mapping(size, &blob->offset))) goto failed;
    struct BlobCreate {
        struct Command header;
        uint32_t flags, entries;
        uint64_t identifier, size;
    } request = {.header={.type=0x10c, .context=context->identifier,
                          .first=++driver.next_resource, .second=2},
                 .flags=mappable ? 1 : 2, .identifier=identifier, .size=size};
    _Static_assert(sizeof(request) == 56, "HOST3D blob layout");
    if (control(&request, sizeof(request), 0x1100, NULL, 0)) goto failed;
    blob->context = context->identifier;
    blob->public.resource_id = request.header.first;
    blob->public.size = size;
    if (simple(0x202, request.header.first, context->identifier)) goto device_failed;
    if (mappable) {
        struct Map { struct Command header; uint64_t offset; } map = {
            .header={.type=0x208, .first=request.header.first}, .offset=blob->offset,
        };
        uint32_t response[8];
        if (control(&map, sizeof(map), 0x1106, response, sizeof(response)))
            goto device_failed;
        if (response[6] < 1 || response[6] > 3) goto device_failed;
        blob->public.cache_type = response[6];
        blob->public.mapping = (void *)(uintptr_t)(host_visible_base + blob->offset);
        printf("VENUS TRANSPORT: blob=%u size=%llu mapping=%p cache=%u\n",
               blob->public.resource_id, (unsigned long long)size,
               blob->public.mapping, response[6]);
    }
    blob->next = driver.blobs;
    driver.blobs = blob;
    *output = blob->public;
    mtx_unlock(&driver.mutex);
    return 0;
device_failed:
    driver.lost = 1;
failed:
    mtx_unlock(&driver.mutex);
    free(blob);
    return -1;
}
int firmware_venus_scanout(struct firmware_venus_context *context, uint32_t resource,
                           uint32_t width, uint32_t height, uint32_t stride, uint32_t offset, uint64_t modifier)
{
    if (!valid_context(context)) return -1;
    if (modifier && !driver.scanout_modifiers) {
        puts("GPU-COMPOSITOR: host lacks the negotiated scanout modifier extension");
        return -1;
    }
    mtx_lock(&driver.mutex);
    struct transport_blob *blob=driver.blobs;
    while (blob && (blob->context!=context->identifier || blob->public.resource_id!=resource)) blob=blob->next;
    if (!resource && driver.scanout_resource) {
        struct transport_blob *owner=driver.blobs;
        while (owner && owner->public.resource_id!=driver.scanout_resource) owner=owner->next;
        if (!owner || owner->context!=context->identifier) { mtx_unlock(&driver.mutex); return -1; }
    }
    if (resource && (!blob || !width || !height || width>4096 || height>4096 ||
        stride < width*4 || stride>65536 || (stride&3) || (offset&3) ||
        (uint64_t)offset+(uint64_t)(height-1)*stride+(uint64_t)width*4>blob->public.size)) {
        mtx_unlock(&driver.mutex);
        return -1;
    }
    struct Scanout {
        uint32_t type, flags; uint64_t fence; uint32_t context, padding;
        uint32_t x, y, width, height, scanout, resource, buffer_width, buffer_height;
        uint32_t format, unused, strides[4], offsets[4];
        uint64_t modifier;
    } request={.type=0x10d, .width=width, .height=height, .resource=resource,
               .buffer_width=width, .buffer_height=height, .format=1,
               .strides={stride}, .offsets={offset}, .modifier=modifier};
    _Static_assert(sizeof(request)==104, "Virtio scanout blob layout");
    int result=control(&request, driver.scanout_modifiers ? sizeof(request) : 96, 0x1100, NULL, 0);
    if (!result) driver.scanout_resource=resource;
    if (!result && resource) {
        struct Flush {
            uint32_t type, flags; uint64_t fence; uint32_t context, padding;
            uint32_t x, y, width, height, resource, unused;
        } flush={.type=0x104, .width=width, .height=height, .resource=resource};
        result=control(&flush, sizeof(flush), 0x1100, NULL, 0);
    }
    mtx_unlock(&driver.mutex);
    return result;
}

void firmware_venus_destroy_blob(struct firmware_venus_context *context,
                                 struct firmware_venus_blob *value)
{
    if (!valid_context(context) || !value) return;
    if (value->resource_id && value->resource_id==driver.scanout_resource &&
        firmware_venus_scanout(context, 0, 0, 0, 0, 0, 0)) return;
    mtx_lock(&driver.mutex);
    struct transport_blob **position = &driver.blobs;
    while (*position && (*position)->public.resource_id != value->resource_id)
        position = &(*position)->next;
    struct transport_blob *blob = *position;
    if (!blob || blob->context != context->identifier ||
        blob->public.mapping != value->mapping || blob->public.size != value->size) {
        mtx_unlock(&driver.mutex);
        return;
    } else if (!driver.lost && blob->references && !blob->public.mapping) {
        if (simple(0x203, value->resource_id, context->identifier)) driver.lost=1;
        else { blob->context=0; memset(value,0,sizeof(*value)); }
    } else if (!driver.lost &&
        (!blob->public.mapping || !simple(0x209, value->resource_id, 0)) &&
        !simple(0x203, value->resource_id, context->identifier) && !simple(0x102, value->resource_id, 0)) {
        *position = blob->next;
        free(blob);
        memset(value, 0, sizeof(*value));
    } else {
        driver.lost = 1;
    }
    mtx_unlock(&driver.mutex);
}

/* Shared exports remain allocated after their producer closes. Only trusted
 * consumers can attach them, and the last reference releases an orphan. */
int firmware_venus_retain_blob(struct firmware_venus_context *owner, uint32_t resource)
{
    if (!valid_context(owner) || !resource) return -1;
    mtx_lock(&driver.mutex);
    struct transport_blob *blob=driver.blobs;
    while (blob && blob->public.resource_id!=resource) blob=blob->next;
    int result=-1;
    if (!driver.lost && blob && blob->context==owner->identifier && !blob->public.mapping && blob->references<256) {
        ++blob->references; result=0;
    }
    mtx_unlock(&driver.mutex);
    return result;
}
void firmware_venus_release_blob(uint32_t resource)
{
    if (!driver.initialized) return;
    mtx_lock(&driver.mutex);
    struct transport_blob **position=&driver.blobs;
    while (*position && (*position)->public.resource_id!=resource) position=&(*position)->next;
    struct transport_blob *blob=*position;
    if (blob && blob->references && !--blob->references && !blob->context && !driver.lost) {
        if (simple(0x102,resource,0)) driver.lost=1;
        else { *position=blob->next; free(blob); }
    }
    mtx_unlock(&driver.mutex);
}
int firmware_venus_attach_blob(struct firmware_venus_context *consumer, uint32_t resource)
{
    if (!valid_context(consumer) || !resource) return -1;
    mtx_lock(&driver.mutex);
    struct transport_blob *blob=driver.blobs;
    while (blob && blob->public.resource_id!=resource) blob=blob->next;
    int result=-1;
    unsigned slot=(unsigned)(consumer-driver.contexts);
    if (!driver.lost && blob && !blob->public.mapping && blob->references && blob->references<256) {
        if (blob->attachments[slot] || !simple(0x202,resource,consumer->identifier)) {
            ++blob->attachments[slot]; ++blob->references; result=0;
        }
    }
    mtx_unlock(&driver.mutex);
    return result;
}
void firmware_venus_detach_blob(struct firmware_venus_context *consumer, uint32_t resource)
{
    if (!valid_context(consumer)) return;
    mtx_lock(&driver.mutex);
    struct transport_blob *blob=driver.blobs;
    while (blob && blob->public.resource_id!=resource) blob=blob->next;
    unsigned slot=(unsigned)(consumer-driver.contexts);
    int release=blob && blob->attachments[slot];
    if (release && !--blob->attachments[slot] && !driver.lost && simple(0x203,resource,consumer->identifier)) driver.lost=1;
    mtx_unlock(&driver.mutex);
    if (release) firmware_venus_release_blob(resource);
}

/* This runs only after the last context closes, or initial device setup fails. */
static void shutdown_device(void)
{
    mtx_lock(&driver.mutex);
    /* Reset acknowledges that all contexts, host rings and DMA have stopped. */
    if (driver.owned && reset_device()) {
        configure(device + 4, saved_command & ~4u);
        graphics_log("FAIL: Venus reset timeout; DMA storage quarantined\n");
        abort();
    }
    while (driver.jobs) {
        struct transport_job *job = driver.jobs;
        driver.jobs = job->next;
        free_pages(job->storage, job->pages);
        free(job);
    }
    while (driver.blobs) {
        struct transport_blob *blob = driver.blobs;
        driver.blobs = blob->next;
        free(blob);
    }
    free_pages(driver.queue_storage, 3);
    if (driver.owned) configure(device + 4, saved_command);
    restore_firmware();
    printf("VENUS TRANSPORT: completed=%llu submitted=%llu\n",
           (unsigned long long)driver.completed, (unsigned long long)driver.next_ticket);
    graphics_log("PASS: Venus runtime transport reset and DMA cleanup\n");
    driver.initialized = 0;
    mtx_unlock(&driver.mutex);
    mtx_destroy(&driver.mutex);
    __atomic_store_n(&claimed, 0, __ATOMIC_RELEASE);
}
static int initialize_device(void)
{
    if (__atomic_exchange_n(&claimed, 1, __ATOMIC_ACQ_REL)) return -1;
    memset(&driver, 0, sizeof(driver));
    common = device_config = notification = NULL;
    notification_length = notification_multiplier = 0;
    host_visible_base = host_visible_size = 0;
    firmware = platform_firmware();
    if (!firmware || !firmware->boot || mtx_init(&driver.mutex, mtx_plain) != thrd_success) {
        __atomic_store_n(&claimed, 0, __ATOMIC_RELEASE);
        return -1;
    }
    driver.initialized = 1;
    if (!find_device() || detach_firmware() || !capabilities() || !host_visible_size) goto failed;
    saved_command = read_config(4) & 0xffff;
    configure(device + 4, saved_command | 6);
    driver.owned = 1;
    if (reset_device()) goto failed;
    common[20] = 3;
    write32(common, 0, 0); uint64_t features = read32(common, 4);
    write32(common, 0, 1); features |= (uint64_t)read32(common, 4) << 32;
    const uint64_t required = (UINT64_C(1) << 32) | 1 | 8 | 16;
    if ((features & required) != required) goto failed;
    driver.scanout_modifiers=!!(features & (UINT64_C(1)<<15));
    uint64_t selected=required | (features & (UINT64_C(1)<<15));
    write32(common, 8, 0); write32(common, 12, (uint32_t)selected);
    write32(common, 8, 1); write32(common, 12, required >> 32);
    common[20] = 11;
    if (!(common[20] & 8)) goto failed;
    driver.queue_storage = allocate_pages(3);
    if (!driver.queue_storage) goto failed;
    descriptors = (void *)(uintptr_t)driver.queue_storage;
    available = (void *)(uintptr_t)(driver.queue_storage + 4096);
    used = (void *)(uintptr_t)(driver.queue_storage + 8192);
    available->flags = 1;
    write16(common, 16, 0xffff); write16(common, 22, 0);
    if (read16(common, 24) < QUEUE_SIZE) goto failed;
    write16(common, 24, QUEUE_SIZE); write16(common, 26, 0xffff);
    address(32, driver.queue_storage); address(40, driver.queue_storage + 4096);
    address(48, driver.queue_storage + 8192);
    write16(common, 28, 1); common[20] = 15;
    unsigned count = read32(device_config, 12), version = 0, size = 0;
    if (!count || count > 64) goto failed;
    for (unsigned index = 0; index < count; ++index) {
        const struct Command query = {.type=0x108, .first=index};
        uint32_t response[10];
        if (control(&query, sizeof(query), 0x1102, response, sizeof(response))) goto failed;
        if (response[6] == 4) { version = response[7]; size = response[8]; }
    }
    struct { unsigned char header[24]; struct virgl_renderer_capset_venus value; } caps;
    const struct Command query = {.type=0x109, .first=4, .second=version};
    if (size != sizeof(caps.value) ||
        control(&query, sizeof(query), 0x1103, &caps, sizeof(caps))) goto failed;
    if (caps.value.wire_format_version != 1 || !caps.value.supports_blob_id_0 ||
        !caps.value.supports_multiple_timelines || !caps.value.allow_vk_wait_syncs ||
        caps.value.use_guest_vram) goto failed;
    driver.capabilities = (struct firmware_venus_capabilities){
        .wire_format_version=caps.value.wire_format_version,
        .vulkan_version=caps.value.vk_xml_version,
        .command_serialization_version=caps.value.vk_ext_command_serialization_spec_version,
        .protocol_version=caps.value.vk_mesa_venus_protocol_spec_version,
        .timeline_count=64,
    };
    memcpy(driver.capabilities.extension_mask, caps.value.vk_extension_mask1,
           sizeof(driver.capabilities.extension_mask));
    graphics_log("PASS: Mesa Venus runtime transport initialized\n");
    return 0;
failed:
    graphics_log("FAIL: Mesa Venus runtime transport initialization\n");
    shutdown_device();
    return -1;
}

int firmware_venus_open(struct firmware_venus_context **context,
                        struct firmware_venus_capabilities *output)
{
    if (!context || !output) return -1;
    *context = NULL;
    if (!driver.initialized && initialize_device()) return -1;
    mtx_lock(&driver.mutex);
    unsigned slot=0;
    while (slot<CONTEXT_LIMIT && driver.contexts[slot].identifier) ++slot;
    if (driver.lost || slot==CONTEXT_LIMIT || driver.next_context==UINT32_MAX) {
        mtx_unlock(&driver.mutex);
        return -1;
    }
    uint32_t identifier=++driver.next_context;
    struct Context { struct Command header; char name[64]; } create = {
        .header={.type=0x200, .context=identifier, .first=12, .second=4},
        .name="UEFI runtime",
    };
    if (control(&create, sizeof(create), 0x1100, NULL, 0)) {
        unsigned count=driver.context_count;
        mtx_unlock(&driver.mutex);
        if (!count) shutdown_device();
        return -1;
    }
    driver.contexts[slot].identifier=identifier;
    ++driver.context_count;
    *context=&driver.contexts[slot];
    *output=driver.capabilities;
    printf("VENUS TRANSPORT: context opened id=%u active=%u\n", identifier, driver.context_count);
    mtx_unlock(&driver.mutex);
    return 0;
}

void firmware_venus_close(struct firmware_venus_context *context)
{
    if (!valid_context(context)) return;
    for (struct transport_blob *blob=driver.blobs; blob; blob=blob->next)
        if (blob->context==context->identifier && blob->public.resource_id==driver.scanout_resource) {
            firmware_venus_scanout(context, 0, 0, 0, 0, 0, 0);
            break;
        }
    mtx_lock(&driver.mutex);
    uint32_t identifier=context->identifier;
    /* Other contexts can still own in-flight DMA. Retire only this owner's jobs. */
    uint64_t deadline=now()+CONTROL_TIMEOUT_NS;
    for (struct transport_job *job=driver.jobs; job && !driver.lost; job=job->next) {
        if (job->context != identifier) continue;
        while (!job->state && !driver.lost) {
            drain();
            if (now() >= deadline) driver.lost=1;
            if (!job->state && !driver.lost) stall();
        }
    }
    /* Context destruction stops host ring threads before their blob mappings disappear.
     * It also drops context attachments, so only global unmap/unref remains. */
    if (!driver.lost && simple(0x201, 0, identifier)) driver.lost=1;
    struct transport_blob **position=&driver.blobs;
    while (*position && !driver.lost) {
        struct transport_blob *blob=*position;
        unsigned slot=(unsigned)(context-driver.contexts);
        blob->references-=blob->attachments[slot];
        blob->attachments[slot]=0;
        if (!blob->context && !blob->references) {
            if (simple(0x102,blob->public.resource_id,0)) { driver.lost=1; break; }
            *position=blob->next; free(blob); continue;
        }
        if (blob->context != identifier) { position=&blob->next; continue; }
        if (blob->references && !blob->public.mapping) { blob->context=0; position=&blob->next; continue; }
        uint32_t resource=blob->public.resource_id;
        if ((blob->public.mapping && simple(0x209, resource, 0)) ||
            simple(0x102, resource, 0)) {
            driver.lost=1;
            break;
        }
        *position=blob->next;
        free(blob);
    }
    struct transport_job *job=driver.jobs;
    while (job) {
        struct transport_job *next=job->next;
        if (job->context==identifier && !job->storage) release_job(job);
        job=next;
    }
    context->identifier=0;
    --driver.context_count;
    printf("VENUS TRANSPORT: context closed id=%u active=%u lost=%d\n",
           identifier, driver.context_count, driver.lost);
    unsigned remaining=driver.context_count;
    mtx_unlock(&driver.mutex);
    /* On loss, all unacknowledged DMA stays quarantined until every owner closes. */
    if (!remaining) shutdown_device();
}

static _Noreturn void finish(const char *reason)
{
    graphics_log(reason);
    abort();
}
static void number(const char *label, uint64_t value)
{
    printf("%s%llu\n", label, (unsigned long long)value);
}
#define text graphics_log
#include "../venus/present.inc"
#undef text

void firmware_venus_present_image(const uint8_t *pixels, uint32_t width)
{
    venus_display_initialize();
    venus_present(pixels, width);
}

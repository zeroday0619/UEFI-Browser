/* SPDX-License-Identifier: BSD-3-Clause */
#include <assert.h>
#include "../src/gpu/venus-runtime/transport.c"

/* Only descriptor memory is simulated. Privileged PCI helpers are never called. */
static BootServices services;
static SystemTable system_table;
static unsigned allocations, releases;
static uint32_t completed_commands[128];
static unsigned command_count;
static unsigned char registers[64], notifications[8];
static struct QueueDescriptor test_descriptors[QUEUE_SIZE];
static struct AvailableRing test_available;
static struct UsedRing test_used;
SystemTable *platform_firmware(void) { return &system_table; }
void graphics_log(const char *message) { fputs(message, stdout); }
static Status UEFI_API test_allocate(uint32_t type, uint32_t memory_type,
                                    size_t pages, uint64_t *result)
{
    assert(type == 0 && memory_type == 2 && pages);
    void *memory = aligned_alloc(4096, pages * 4096);
    assert(memory);
    *result = (uintptr_t)memory;
    ++allocations;
    return 0;
}
static Status UEFI_API test_release(uint64_t address_value, size_t pages)
{
    assert(address_value && pages);
    free((void *)(uintptr_t)address_value);
    ++releases;
    return 0;
}
static void complete(struct transport_job *job);
static Status UEFI_API test_stall(size_t microseconds)
{
    assert(microseconds == 100);
    for (unsigned index=0; index<QUEUE_SIZE/2; ++index)
        if (driver.slots[index]) complete(driver.slots[index]);
    return 0;
}
static void initialize(void)
{
    command_count=0;
    memset(&driver, 0, sizeof(driver));
    memset(registers, 0, sizeof(registers));
    memset(&test_available, 0, sizeof(test_available));
    memset(&test_used, 0, sizeof(test_used));
    assert(mtx_init(&driver.mutex, mtx_plain) == thrd_success);
    driver.initialized = 1;
    driver.contexts[0].identifier=1;
    driver.next_context=driver.context_count=1;
    common = registers;
    notification = notifications;
    notification_multiplier = 1;
    notification_length = sizeof(notifications);
    descriptors = test_descriptors;
    available = &test_available;
    used = &test_used;
    services.function[2] = test_allocate;
    services.function[3] = test_release;
    services.function[28] = test_stall;
    system_table.boot = &services;
    firmware = &system_table;
    claimed = 1;
}
static void complete(struct transport_job *job)
{
    unsigned slot = 0;
    while (slot < QUEUE_SIZE / 2 && driver.slots[slot] != job) ++slot;
    assert(slot < QUEUE_SIZE / 2);
    assert(command_count<128);
    completed_commands[command_count++]=((struct Command *)(uintptr_t)job->storage)->type;
    struct Command *response = (void *)(uintptr_t)(job->storage + (job->pages - 1) * 4096);
    *response = (struct Command){.type=0x1100, .flags=1, .fence=job->ticket,
                                 .context=job->context};
    test_used.entries[test_used.index++ % QUEUE_SIZE] = (struct UsedEntry){slot * 2, 24};
}
static void completion_cases(void)
{
    initialize();
    driver.submitted = driver.consumed = UINT16_MAX - 1;
    test_used.index = test_available.index = UINT16_MAX - 1;
    struct Command command = {.type=0x207, .context=1, .padding=1};
    struct transport_job *first = enqueue(&command, sizeof(command), 0x1100);
    struct transport_job *second = enqueue(&command, sizeof(command), 0x1100);
    assert(first && second && available->index == 0);
    command.context = 99;
    assert(((struct Command *)(uintptr_t)first->storage)->context == 1);
    complete(second);
    drain();
    assert(second->state == 1 && !second->storage && first->storage && !driver.lost);
    assert(firmware_venus_poll(&driver.contexts[0], first->ticket) == 0);
    firmware_venus_release_fence(&driver.contexts[0], second->ticket);
    complete(first);
    drain();
    assert(first->state == 1 && !first->storage && driver.consumed == 0);
    firmware_venus_release_fence(&driver.contexts[0], first->ticket);
    assert(!driver.jobs && driver.completed == 2);
    firmware_venus_close(&driver.contexts[0]);
    assert(releases == allocations);
}
static void malformed_cases(void)
{
    for (unsigned scenario = 0; scenario < 8; ++scenario) {
        initialize();
        const struct Command command = {.type=0x207, .context=1};
        struct transport_job *job = enqueue(&command, sizeof(command), 0x1100);
        assert(job);
        complete(job);
        struct Command *response = (void *)(uintptr_t)(job->storage + (job->pages - 1) * 4096);
        switch (scenario) {
        case 0: ++response->fence; break;
        case 1: ++response->context; break;
        case 2: response->flags = 0; break;
        case 3: test_used.entries[0].length = RESPONSE_SIZE + 1; break;
        case 4: test_used.entries[0].identifier = 1; break;
        case 5: test_used.index = QUEUE_SIZE / 2 + 1; break;
        case 6: registers[20] = 64; break;
        case 7: response->type = 0x1200; break;
        }
        unsigned before = releases;
        drain();
        assert(driver.lost && firmware_venus_poll(&driver.contexts[0], job->ticket) == -1);
        /* Malformed completions retain DMA until teardown; valid error replies retire it. */
        if (scenario != 7) assert(job->storage && releases == before);
        else assert(!job->storage && releases == before + 1);
        firmware_venus_close(&driver.contexts[0]);
        assert(releases == allocations && !driver.jobs && !claimed);
    }
}
static void aperture_cases(void)
{
    initialize();
    host_visible_size = 16384;
    struct transport_blob last = {.public={.mapping=(void *)1, .size=4096}, .offset=8192};
    struct transport_blob first = {.public={.mapping=(void *)1, .size=4096}, .offset=0, .next=&last};
    driver.blobs = &first;
    uint64_t offset;
    assert(!reserve_mapping(4096, &offset) && offset == 4096);
    assert(reserve_mapping(8192, &offset) == -1);
    assert(reserve_mapping(UINT64_MAX, &offset) == -1);
    first.public.mapping = NULL;
    assert(!reserve_mapping(8192, &offset) && offset == 0);
    driver.blobs = NULL;
    firmware_venus_close(&driver.contexts[0]);
}
static void multiple_context_cases(void)
{
    initialize();
    struct firmware_venus_context *first=&driver.contexts[0], *second=NULL;
    struct firmware_venus_capabilities caps;
    assert(!firmware_venus_open(&second, &caps));
    assert(second != first && second->identifier==2 && driver.context_count==2);
    uint32_t command=0;
    uint64_t first_ticket, second_ticket;
    assert(!firmware_venus_submit(first, &command, 4, 1, &first_ticket));
    assert(!firmware_venus_submit(second, &command, 4, 1, &second_ticket));
    assert(firmware_venus_poll(second, first_ticket)==-1);
    firmware_venus_release_fence(second, first_ticket);
    assert(!driver.lost && firmware_venus_poll(first, first_ticket)==0);
    struct transport_blob *blob=calloc(1, sizeof(*blob));
    blob->context=second->identifier;
    blob->public=(struct firmware_venus_blob){.resource_id=9, .size=4096};
    driver.blobs=blob;
    struct firmware_venus_blob foreign=blob->public;
    assert(firmware_venus_scanout(first,9,16,16,64,0,0)==-1);
    assert(firmware_venus_scanout(second,9,0,16,64,0,0)==-1);
    assert(firmware_venus_scanout(second,9,16,16,60,0,0)==-1);
    assert(firmware_venus_scanout(second,9,16,16,64,4096,0)==-1);
    assert(firmware_venus_scanout(second,9,16,16,64,0,1)==-1);
    driver.scanout_modifiers=1;
    assert(!firmware_venus_scanout(second,9,16,16,64,0,0x300000000606015ULL));
    assert(driver.scanout_resource==9);
    assert(firmware_venus_scanout(first,0,0,0,0,0,0)==-1);
    firmware_venus_destroy_blob(first, &foreign);
    assert(driver.scanout_resource==9);
    assert(!firmware_venus_scanout(second,0,0,0,0,0,0));
    assert(!driver.scanout_resource);
    assert(driver.blobs==blob && !driver.lost && foreign.resource_id==9);
    firmware_venus_close(first);
    assert(driver.initialized && driver.context_count==1 && !driver.lost);
    assert(driver.blobs==blob && firmware_venus_poll(second, second_ticket)==1);
    firmware_venus_release_fence(second, second_ticket);
    assert(!firmware_venus_submit(second, &command, 4, 2, &second_ticket));
    firmware_venus_close(second);
    assert(!driver.initialized && allocations==releases);
    assert(command_count>=2 && completed_commands[command_count-2]==0x201 &&
           completed_commands[command_count-1]==0x102);
    initialize();
    assert(!firmware_venus_open(&second, &caps));
    assert(!firmware_venus_submit(first, &command, 4, 1, &first_ticket));
    unsigned before=releases;
    driver.lost=1;
    firmware_venus_close(first);
    assert(releases==before && driver.jobs && driver.initialized);
    firmware_venus_close(second);
    assert(allocations==releases && !driver.initialized);
    puts("PASS: independent context IDs, fence and resource ownership, sibling survival and shared DMA quarantine");
}
static void shared_image_cases(void)
{
    initialize();
    struct firmware_venus_context *producer=&driver.contexts[0], *consumer=NULL;
    struct firmware_venus_capabilities capabilities;
    assert(!firmware_venus_open(&consumer,&capabilities));
    struct transport_blob *blob=calloc(1,sizeof(*blob));
    blob->context=producer->identifier;
    blob->public=(struct firmware_venus_blob){.resource_id=41,.size=4096};
    driver.blobs=blob;
    assert(firmware_venus_retain_blob(consumer,41)==-1);
    assert(firmware_venus_attach_blob(consumer,41)==-1);
    assert(!firmware_venus_retain_blob(producer,41));
    assert(!firmware_venus_attach_blob(consumer,41));
    assert(!firmware_venus_attach_blob(consumer,41));
    assert(blob->references==3);
    struct firmware_venus_blob value=blob->public;
    firmware_venus_destroy_blob(producer,&value);
    assert(!value.resource_id && !blob->context && blob->references==3);
    firmware_venus_close(producer);
    assert(driver.blobs==blob && !driver.lost);
    firmware_venus_release_blob(41);
    firmware_venus_detach_blob(consumer,41);
    assert(driver.blobs==blob && blob->references==1);
    firmware_venus_detach_blob(consumer,41);
    assert(!driver.blobs && !driver.lost);
    firmware_venus_release_blob(41);
    firmware_venus_close(consumer);
    assert(allocations==releases);
    puts("PASS: shared GPU allocation survives producer destruction and retires after its final consumer");
}
int main(void)
{
    shared_image_cases();
    completion_cases();
    malformed_cases();
    aperture_cases();
    multiple_context_cases();
    puts("PASS: Venus transport copied commands, out-of-order completion, index wrap, malformed replies, DMA quarantine and aperture bounds");
    return 0;
}

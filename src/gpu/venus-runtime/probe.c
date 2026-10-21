/* SPDX-License-Identifier: BSD-3-Clause */
#include <vulkan/vulkan.h>
#include <stdint.h>
#include <time.h>
#include <errno.h>
#include <sys/mman.h>
#include "renderer_runtime.h"
#include "renderer_sandbox.h"
#include "venus_broker.h"
#include <stdio.h>
#include <stdlib.h>

extern PFN_vkVoidFunction vk_icdGetInstanceProcAddr(VkInstance, const char *);
extern VkResult vk_icdNegotiateLoaderICDInterfaceVersion(uint32_t *);
extern void graphics_log(const char *);

#define REQUIRE(condition) do { if (!(condition)) { \
    printf("FAIL: Mesa runtime probe at line %d: %s\n", __LINE__, #condition); fflush(NULL); abort(); } } while (0)
#define INSTANCE(name) PFN_##name name = (PFN_##name)vk_icdGetInstanceProcAddr(instance, #name); REQUIRE(name)
#define DEVICE(name) PFN_##name name = (PFN_##name)vkGetDeviceProcAddr(device, #name); REQUIRE(name)

#ifdef ENABLE_VENUS_CPL3
extern uint64_t firmware_venus_client_session(void);
static void validate_gpu_boundaries(void *mapping)
{
    uint64_t session=firmware_venus_client_session();
    REQUIRE(session);
    REQUIRE((long)renderer_runtime_request(RENDERER_VENUS_REQUEST, VENUS_CLOSE, session+1, 0, 0, 0) == -EBADF);
    REQUIRE((long)renderer_runtime_request(RENDERER_VENUS_REQUEST, VENUS_DESTROY_BLOB, session, UINT32_MAX, 0, 0) == -EBADF);
    REQUIRE((long)renderer_runtime_request(RENDERER_VENUS_REQUEST, VENUS_SUBMIT, session, (uintptr_t)mapping, UINT64_MAX, 0) == -EINVAL);
    REQUIRE((long)renderer_runtime_request(RENDERER_VENUS_REQUEST, VENUS_SUBMIT, session, (uintptr_t)mapping, 4, 64) == -EINVAL);
    REQUIRE((long)renderer_runtime_request(RENDERER_VENUS_REQUEST, VENUS_CREATE_BLOB, session, 1, 0, 0) == -EFAULT);
    VenusBlobMessage message={.version=VENUS_BROKER_VERSION, .mappable=1, .size=VENUS_BROKER_MEMORY_LIMIT+4096};
    REQUIRE((long)renderer_runtime_request(RENDERER_VENUS_REQUEST, VENUS_CREATE_BLOB, session, (uintptr_t)&message, 0, 0) == -EINVAL);
    REQUIRE((long)renderer_runtime_request(RENDERER_VENUS_REQUEST, VENUS_RELEASE, session, UINT64_MAX, 0, 0) == -EINVAL);
    uint64_t permissions=renderer_system_call(RENDERER_SYSTEM_PERMISSIONS, (uintptr_t)mapping, 0, 0, 0, 0);
    REQUIRE((permissions & 7) == 7 && (permissions & (UINT64_C(1)<<63)));
    REQUIRE(renderer_system_call(RENDERER_SYSTEM_PROTECT, (uintptr_t)mapping, 4096, PROT_READ|PROT_EXEC, 0, 0) == UINT64_MAX);
    void *alias=mmap(NULL, 4096, PROT_NONE, MAP_PRIVATE|MAP_ANONYMOUS, -1, 0);
    REQUIRE(alias != MAP_FAILED);
    REQUIRE(renderer_system_call(RENDERER_SYSTEM_MAP, (uintptr_t)alias, (uintptr_t)mapping,
                                  (UINT64_C(1)<<63)|3, 0, 0) == UINT64_MAX);
    REQUIRE(!munmap(alias, 4096));
    puts("PASS: Venus CPL3 session, resource, range, quota, fence, NX and alias boundaries");
}
#endif

static void validate_clock_sleep(void)
{
    struct timespec before, after, delay = {0, 2000000}, remaining = {7, 11};
    REQUIRE(!clock_gettime(CLOCK_MONOTONIC, &before));
    REQUIRE(!clock_nanosleep(CLOCK_MONOTONIC, 0, &delay, &remaining));
    REQUIRE(!clock_gettime(CLOCK_MONOTONIC, &after));
    int64_t elapsed = (after.tv_sec - before.tv_sec) * INT64_C(1000000000) +
                      after.tv_nsec - before.tv_nsec;
    REQUIRE(elapsed >= 2000000);
    struct timespec deadline = after;
    deadline.tv_nsec += 2000000;
    if (deadline.tv_nsec >= 1000000000) { ++deadline.tv_sec; deadline.tv_nsec -= 1000000000; }
    REQUIRE(!clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &deadline, &remaining));
    REQUIRE(!clock_gettime(CLOCK_MONOTONIC, &after));
    REQUIRE(after.tv_sec > deadline.tv_sec ||
            (after.tv_sec == deadline.tv_sec && after.tv_nsec >= deadline.tv_nsec));
    REQUIRE(!clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &before, &remaining));
    REQUIRE(remaining.tv_sec == 7 && remaining.tv_nsec == 11);
    REQUIRE(clock_nanosleep(CLOCK_MONOTONIC, 2, &delay, NULL) == EINVAL);
    delay.tv_nsec = 1000000000;
    REQUIRE(clock_nanosleep(CLOCK_MONOTONIC, 0, &delay, NULL) == EINVAL);
    printf("PASS: Mesa monotonic relative and absolute waits elapsed_ns=%lld\n", (long long)elapsed);
}

extern void run_venus_runtime_render(VkInstance, VkPhysicalDevice, VkDevice, VkQueue, uint32_t);

static void cycle(void)
{
    validate_clock_sleep();
    VkInstance instance = VK_NULL_HANDLE;
    INSTANCE(vkCreateInstance);
    VkApplicationInfo application = {.sType=VK_STRUCTURE_TYPE_APPLICATION_INFO,
        .pApplicationName="UEFI BROWSER Mesa runtime", .apiVersion=VK_API_VERSION_1_3};
    VkInstanceCreateInfo instance_info = {.sType=VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
        .pApplicationInfo=&application};
    REQUIRE(vkCreateInstance(&instance_info, NULL, &instance) == VK_SUCCESS);
    INSTANCE(vkEnumeratePhysicalDevices);
    INSTANCE(vkGetPhysicalDeviceProperties);
    INSTANCE(vkGetPhysicalDeviceMemoryProperties);
    INSTANCE(vkGetPhysicalDeviceQueueFamilyProperties);
    INSTANCE(vkCreateDevice);
    INSTANCE(vkGetDeviceProcAddr);
    INSTANCE(vkDestroyInstance);
    uint32_t count = 8;
    VkPhysicalDevice physical[8];
    REQUIRE(vkEnumeratePhysicalDevices(instance, &count, physical) == VK_SUCCESS && count);
    VkPhysicalDeviceProperties properties;
    vkGetPhysicalDeviceProperties(physical[0], &properties);
    printf("MESA RUNTIME: device=%s type=%u vendor=%x\n",
           properties.deviceName, properties.deviceType, properties.vendorID);
    VkQueueFamilyProperties families[64];
    count = 64;
    vkGetPhysicalDeviceQueueFamilyProperties(physical[0], &count, families);
    REQUIRE(count <= 64);
    uint32_t family = 0;
    while (family < count && !(families[family].queueFlags & VK_QUEUE_GRAPHICS_BIT)) ++family;
    REQUIRE(family < count && families[family].timestampValidBits);
    float priority = 1;
    VkDeviceQueueCreateInfo queue_info = {.sType=VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
        .queueFamilyIndex=family, .queueCount=1, .pQueuePriorities=&priority};
    VkDeviceCreateInfo device_info = {.sType=VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
        .queueCreateInfoCount=1, .pQueueCreateInfos=&queue_info};
    VkDevice device;
    REQUIRE(vkCreateDevice(physical[0], &device_info, NULL, &device) == VK_SUCCESS);
    DEVICE(vkGetDeviceQueue); DEVICE(vkDestroyDevice); DEVICE(vkCreateBuffer);
    DEVICE(vkGetBufferMemoryRequirements); DEVICE(vkAllocateMemory); DEVICE(vkBindBufferMemory);
    DEVICE(vkMapMemory); DEVICE(vkUnmapMemory); DEVICE(vkFreeMemory); DEVICE(vkDestroyBuffer);
    DEVICE(vkCreateCommandPool); DEVICE(vkAllocateCommandBuffers); DEVICE(vkBeginCommandBuffer);
    DEVICE(vkEndCommandBuffer); DEVICE(vkCmdFillBuffer); DEVICE(vkCmdPipelineBarrier);
    DEVICE(vkCreateQueryPool); DEVICE(vkCmdResetQueryPool); DEVICE(vkCmdWriteTimestamp);
    DEVICE(vkGetQueryPoolResults); DEVICE(vkDestroyQueryPool); DEVICE(vkDestroyCommandPool);
    DEVICE(vkCreateFence); DEVICE(vkQueueSubmit); DEVICE(vkWaitForFences); DEVICE(vkDestroyFence);
    VkQueue queue;
    vkGetDeviceQueue(device, family, 0, &queue);
    REQUIRE(queue);
    VkBuffer buffer;
    VkBufferCreateInfo buffer_info = {.sType=VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
        .size=65536, .usage=VK_BUFFER_USAGE_TRANSFER_DST_BIT,
        .sharingMode=VK_SHARING_MODE_EXCLUSIVE};
    REQUIRE(vkCreateBuffer(device, &buffer_info, NULL, &buffer) == VK_SUCCESS);
    VkMemoryRequirements requirements;
    vkGetBufferMemoryRequirements(device, buffer, &requirements);
    VkPhysicalDeviceMemoryProperties memory_properties;
    vkGetPhysicalDeviceMemoryProperties(physical[0], &memory_properties);
    uint32_t memory_type = 0;
    const VkMemoryPropertyFlags flags = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                       VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
    while (memory_type < memory_properties.memoryTypeCount &&
        (!(requirements.memoryTypeBits & (1u << memory_type)) ||
         (memory_properties.memoryTypes[memory_type].propertyFlags & flags) != flags)) ++memory_type;
    REQUIRE(memory_type < memory_properties.memoryTypeCount);
    VkMemoryDedicatedAllocateInfo dedicated = {.sType=VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO,
        .buffer=buffer};
    VkMemoryAllocateInfo allocation = {.sType=VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .pNext=&dedicated, .allocationSize=requirements.size, .memoryTypeIndex=memory_type};
    VkDeviceMemory memory;
    REQUIRE(vkAllocateMemory(device, &allocation, NULL, &memory) == VK_SUCCESS);
    REQUIRE(vkBindBufferMemory(device, buffer, memory, 0) == VK_SUCCESS);
    VkCommandPoolCreateInfo pool_info = {.sType=VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
        .queueFamilyIndex=family};
    VkCommandPool pool;
    REQUIRE(vkCreateCommandPool(device, &pool_info, NULL, &pool) == VK_SUCCESS);
    VkCommandBufferAllocateInfo command_info = {.sType=VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
        .commandPool=pool, .level=VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount=1};
    VkCommandBuffer command;
    REQUIRE(vkAllocateCommandBuffers(device, &command_info, &command) == VK_SUCCESS);
    VkQueryPoolCreateInfo query_info = {.sType=VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO,
        .queryType=VK_QUERY_TYPE_TIMESTAMP, .queryCount=2};
    VkQueryPool query;
    REQUIRE(vkCreateQueryPool(device, &query_info, NULL, &query) == VK_SUCCESS);
    VkCommandBufferBeginInfo begin = {.sType=VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    REQUIRE(vkBeginCommandBuffer(command, &begin) == VK_SUCCESS);
    vkCmdResetQueryPool(command, query, 0, 2);
    vkCmdWriteTimestamp(command, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, query, 0);
    vkCmdFillBuffer(command, buffer, 0, 65536, 0x13579bdf);
    VkBufferMemoryBarrier barrier = {.sType=VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER,
        .srcAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT, .dstAccessMask=VK_ACCESS_HOST_READ_BIT,
        .srcQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED, .dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED,
        .buffer=buffer, .size=VK_WHOLE_SIZE};
    vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT,
                         0, 0, NULL, 1, &barrier, 0, NULL);
    vkCmdWriteTimestamp(command, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, query, 1);
    REQUIRE(vkEndCommandBuffer(command) == VK_SUCCESS);
    VkFenceCreateInfo fence_info = {.sType=VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    VkFence fence;
    REQUIRE(vkCreateFence(device, &fence_info, NULL, &fence) == VK_SUCCESS);
    VkSubmitInfo submit = {.sType=VK_STRUCTURE_TYPE_SUBMIT_INFO,
        .commandBufferCount=1, .pCommandBuffers=&command};
    REQUIRE(vkQueueSubmit(queue, 1, &submit, fence) == VK_SUCCESS);
    REQUIRE(vkWaitForFences(device, 1, &fence, VK_TRUE, UINT64_C(10000000000)) == VK_SUCCESS);
    uint64_t timestamps[2];
    REQUIRE(vkGetQueryPoolResults(device, query, 0, 2, sizeof(timestamps), timestamps,
        sizeof(uint64_t), VK_QUERY_RESULT_64_BIT) == VK_SUCCESS);
    REQUIRE(timestamps[0] && timestamps[1] > timestamps[0]);
    uint32_t *mapping = NULL;
    REQUIRE(vkMapMemory(device, memory, 0, 65536, 0, (void **)&mapping) == VK_SUCCESS);
#ifdef ENABLE_VENUS_CPL3
    validate_gpu_boundaries(mapping);
#endif
    for (unsigned index = 0; index < 65536 / sizeof(*mapping); ++index)
        REQUIRE(mapping[index] == 0x13579bdf);
    printf("MESA RUNTIME: timestamps=%llu,%llu validated_bytes=65536\n",
           (unsigned long long)timestamps[0], (unsigned long long)timestamps[1]);
    vkUnmapMemory(device, memory);
    vkDestroyFence(device, fence, NULL);
    vkDestroyQueryPool(device, query, NULL);
    vkDestroyCommandPool(device, pool, NULL);
    vkDestroyBuffer(device, buffer, NULL);
    vkFreeMemory(device, memory, NULL);
    run_venus_runtime_render(instance, physical[0], device, queue, family);
    vkDestroyDevice(device, NULL);
    vkDestroyInstance(instance, NULL);
    graphics_log("PASS: Mesa runtime Vulkan queue, GPU buffer readback and teardown\n");
}
int run_venus_runtime_probe(void)
{
    setenv("VN_DEBUG", "init,result", 1);
    setenv("MESA_LOG_LEVEL", "debug", 1);
    graphics_log("MESA RUNTIME: beginning ICD validation\n");
    uint32_t version = 7;
    REQUIRE(vk_icdNegotiateLoaderICDInterfaceVersion(&version) == VK_SUCCESS && version >= 5);
    for (unsigned index = 0; index < 3; ++index) cycle();
    graphics_log("PASS: UEFI Mesa Venus runtime Vulkan execution\n");
    return 0;
}

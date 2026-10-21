// Copyright 2026 zeroday0619
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
#include "blink/gpu_canvas.h"
#include <cstdio>
#include <map>
#include <algorithm>
#include "venus_image.h"
#if defined(UEFI_RUNTIME)
#include <vulkan/vulkan.h>
#endif
extern "C" {
#if defined(UEFI_RUNTIME)
PFN_vkVoidFunction firmware_venus_get_proc(VkInstance,const char*) __attribute__((weak));
uint64_t firmware_venus_export_image(VkDevice,VkDeviceMemory,const firmware_venus_image*) __attribute__((weak));
#endif
void firmware_venus_release_image(uint64_t) __attribute__((weak));
int firmware_venus_acquire_image(uint64_t,firmware_venus_image*) __attribute__((weak));
int firmware_gpu_canvas_enabled(void) __attribute__((weak));
}
namespace uefi_browser {
namespace {
std::map<int,std::shared_ptr<GpuCanvasFrame>> frames;
}
GpuCanvasFrame::~GpuCanvasFrame() {
  image.reset();
  if (firmware_venus_release_image) firmware_venus_release_image(token);
}
void SetGpuCanvasFrame(int layer,uint64_t token) {
  if (token) frames[layer]=std::make_shared<GpuCanvasFrame>(token);
  else frames.erase(layer);
}
std::shared_ptr<GpuCanvasFrame> GetGpuCanvasFrame(int layer) {
  auto found=frames.find(layer);
  return found==frames.end() ? nullptr : found->second;
}
std::shared_ptr<GpuCanvasFrame> AcquireGpuCanvasFrame(uint64_t token) {
  // Repeated scene publications may reference the same immutable canvas frame.
  // Sharing its lease also avoids repeating Vulkan imports after pointer updates.
  static std::map<uint64_t,std::weak_ptr<GpuCanvasFrame>> imported;
  if (auto found=imported.find(token); found!=imported.end())
    if (auto frame=found->second.lock()) return frame;
  firmware_venus_image metadata{};
  if (!firmware_venus_acquire_image || firmware_venus_acquire_image(token,&metadata)) return {};
  auto frame=std::make_shared<GpuCanvasFrame>(token);
  std::erase_if(imported,[](const auto& entry){return entry.second.expired();});
  imported[token]=frame;
  return frame;
}
}

#if defined(UEFI_RUNTIME)
// The copy publishes immutable storage. The producer can immediately reuse its
// canvas after completion while scene references retain the exported allocation.
extern "C" uint64_t firmware_gpu_copy_canvas(VkInstance instance,VkPhysicalDevice physical,
    VkDevice device,VkQueue queue,uint32_t family,VkImage source,VkImageLayout source_layout,
    uint32_t width,uint32_t height,VkFormat format,uint32_t alpha) {
  if (!firmware_gpu_canvas_enabled || !firmware_gpu_canvas_enabled() ||
      !firmware_venus_export_image || !width || !height || width>4096 || height>4096 ||
      (format!=VK_FORMAT_R8G8B8A8_UNORM && format!=VK_FORMAT_B8G8R8A8_UNORM && format!=VK_FORMAT_R16G16B16A16_SFLOAT)) return 0;
  auto get=reinterpret_cast<PFN_vkGetDeviceProcAddr>(firmware_venus_get_proc(instance,"vkGetDeviceProcAddr"));
  auto properties=reinterpret_cast<PFN_vkGetPhysicalDeviceMemoryProperties>(firmware_venus_get_proc(instance,"vkGetPhysicalDeviceMemoryProperties"));
  if (!get || !properties) return 0;
#define LOAD(name) auto name=reinterpret_cast<PFN_vk##name>(get(device,"vk" #name)); if (!name) return 0
  LOAD(CreateImage); LOAD(DestroyImage); LOAD(GetImageMemoryRequirements); LOAD(AllocateMemory); LOAD(FreeMemory); LOAD(BindImageMemory);
  LOAD(CreateCommandPool); LOAD(DestroyCommandPool); LOAD(AllocateCommandBuffers); LOAD(BeginCommandBuffer); LOAD(EndCommandBuffer);
  LOAD(CmdPipelineBarrier); LOAD(CmdCopyImage); LOAD(CreateFence); LOAD(DestroyFence); LOAD(QueueSubmit); LOAD(WaitForFences); LOAD(DeviceWaitIdle);
#undef LOAD
  VkImage image=VK_NULL_HANDLE; VkDeviceMemory memory=VK_NULL_HANDLE;
  VkCommandPool pool=VK_NULL_HANDLE; VkFence fence=VK_NULL_HANDLE;
  uint64_t token=0;
  auto cleanup=[&] {
    if (fence) DestroyFence(device,fence,nullptr);
    if (pool) DestroyCommandPool(device,pool,nullptr);
    if (image) DestroyImage(device,image,nullptr);
    if (memory) FreeMemory(device,memory,nullptr);
    return token;
  };
  VkExternalMemoryImageCreateInfo external{VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO};
  external.handleTypes=VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT;
  VkImageCreateInfo info{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO}; info.pNext=&external;
  info.imageType=VK_IMAGE_TYPE_2D; info.format=format; info.extent={width,height,1};
  info.mipLevels=1; info.arrayLayers=1; info.samples=VK_SAMPLE_COUNT_1_BIT;
  info.tiling=VK_IMAGE_TILING_OPTIMAL; info.usage=VK_IMAGE_USAGE_TRANSFER_SRC_BIT|VK_IMAGE_USAGE_TRANSFER_DST_BIT|VK_IMAGE_USAGE_SAMPLED_BIT;
  if (CreateImage(device,&info,nullptr,&image)!=VK_SUCCESS) return cleanup();
  VkMemoryRequirements requirements{}; GetImageMemoryRequirements(device,image,&requirements);
  VkPhysicalDeviceMemoryProperties types{}; properties(physical,&types);
  uint32_t type=0;
  while (type<types.memoryTypeCount && (!(requirements.memoryTypeBits&(1u<<type)) || !(types.memoryTypes[type].propertyFlags&VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT))) ++type;
  if (type==types.memoryTypeCount) return cleanup();
  VkMemoryDedicatedAllocateInfo dedicated{VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO}; dedicated.image=image;
  VkExportMemoryAllocateInfo export_info{VK_STRUCTURE_TYPE_EXPORT_MEMORY_ALLOCATE_INFO}; export_info.pNext=&dedicated; export_info.handleTypes=external.handleTypes;
  VkMemoryAllocateInfo allocation{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO}; allocation.pNext=&export_info; allocation.allocationSize=requirements.size; allocation.memoryTypeIndex=type;
  if (AllocateMemory(device,&allocation,nullptr,&memory)!=VK_SUCCESS || BindImageMemory(device,image,memory,0)!=VK_SUCCESS) return cleanup();
  VkCommandPoolCreateInfo pool_info{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO}; pool_info.queueFamilyIndex=family;
  if (CreateCommandPool(device,&pool_info,nullptr,&pool)!=VK_SUCCESS) return cleanup();
  VkCommandBufferAllocateInfo commands{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO}; commands.commandPool=pool; commands.level=VK_COMMAND_BUFFER_LEVEL_PRIMARY; commands.commandBufferCount=1;
  VkCommandBuffer command;
  if (AllocateCommandBuffers(device,&commands,&command)!=VK_SUCCESS) return cleanup();
  VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO}; begin.flags=VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
  if (BeginCommandBuffer(command,&begin)!=VK_SUCCESS) return cleanup();
  VkImageMemoryBarrier barriers[2]{};
  for (auto& barrier:barriers) {
    barrier.sType=VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    barrier.srcQueueFamilyIndex=barrier.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED;
    barrier.subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1};
  }
  barriers[0].image=source; barriers[0].oldLayout=source_layout; barriers[0].newLayout=VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
  barriers[0].srcAccessMask=VK_ACCESS_MEMORY_WRITE_BIT|VK_ACCESS_MEMORY_READ_BIT; barriers[0].dstAccessMask=VK_ACCESS_TRANSFER_READ_BIT;
  barriers[1].image=image; barriers[1].oldLayout=VK_IMAGE_LAYOUT_UNDEFINED; barriers[1].newLayout=VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL; barriers[1].dstAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT;
  CmdPipelineBarrier(command,VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT,0,0,nullptr,0,nullptr,2,barriers);
  VkImageCopy copy{}; copy.srcSubresource=copy.dstSubresource={VK_IMAGE_ASPECT_COLOR_BIT,0,0,1}; copy.extent={width,height,1};
  CmdCopyImage(command,source,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,image,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,1,&copy);
  barriers[0].oldLayout=VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL; barriers[0].newLayout=source_layout;
  barriers[0].srcAccessMask=VK_ACCESS_TRANSFER_READ_BIT; barriers[0].dstAccessMask=VK_ACCESS_MEMORY_READ_BIT|VK_ACCESS_MEMORY_WRITE_BIT;
  barriers[1].oldLayout=VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL; barriers[1].newLayout=VK_IMAGE_LAYOUT_GENERAL;
  barriers[1].srcAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT; barriers[1].dstAccessMask=0;
  barriers[1].srcQueueFamilyIndex=family; barriers[1].dstQueueFamilyIndex=VK_QUEUE_FAMILY_EXTERNAL;
  CmdPipelineBarrier(command,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,0,0,nullptr,0,nullptr,2,barriers);
  if (EndCommandBuffer(command)!=VK_SUCCESS) return cleanup();
  VkFenceCreateInfo fence_info{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
  if (CreateFence(device,&fence_info,nullptr,&fence)!=VK_SUCCESS) return cleanup();
  VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO}; submit.commandBufferCount=1; submit.pCommandBuffers=&command;
  if (QueueSubmit(queue,1,&submit,fence)!=VK_SUCCESS) return cleanup();
  if (WaitForFences(device,1,&fence,VK_TRUE,5000000000ULL)!=VK_SUCCESS) {
    // Pending commands must stop using their storage before it can be released.
    if (DeviceWaitIdle(device)!=VK_SUCCESS) return 0;
    return cleanup();
  }
  firmware_venus_image metadata{width,height,static_cast<uint32_t>(format),alpha,requirements.size,type,0};
  token=firmware_venus_export_image(device,memory,&metadata);
  if (token) std::printf("GPU-CANVAS: exported token=%llu size=%ux%u cpu_readback_bytes=0\n",static_cast<unsigned long long>(token),width,height);
  return cleanup();
}

#endif

// Copyright 2026 zeroday0619
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
#include "blink/gpu_compositor.h"
#include "venus_image.h"
#include "third_party/blink/renderer/platform/graphics/predefined_color_space.h"
#include <cstdio>
#include <vector>
#include <string_view>
#include "base/time/time.h"
#include "third_party/skia/include/core/SkCanvas.h"
#include "third_party/skia/include/gpu/ganesh/GrBackendSurface.h"
#include "third_party/skia/include/gpu/ganesh/GrDirectContext.h"
#include "third_party/skia/include/gpu/ganesh/SkSurfaceGanesh.h"
#include "third_party/skia/include/gpu/ganesh/SkImageGanesh.h"
#include "third_party/skia/include/gpu/ganesh/vk/GrVkBackendSurface.h"
#include "third_party/skia/include/gpu/ganesh/vk/GrVkDirectContext.h"
#include "third_party/skia/include/gpu/ganesh/vk/GrVkTypes.h"
#include "third_party/skia/include/gpu/vk/VulkanBackendContext.h"
#include "third_party/skia/include/gpu/vk/VulkanMemoryAllocator.h"
extern "C" int firmware_gpu_composition_enabled(void) __attribute__((weak));
extern "C" PFN_vkVoidFunction firmware_venus_get_proc(VkInstance, const char*) __attribute__((weak));
extern "C" int firmware_venus_scanout_memory(VkDevice, VkDeviceMemory, uint32_t, uint32_t, uint32_t, uint32_t, uint64_t) __attribute__((weak));
extern "C" {
int firmware_venus_acquire_image(uint64_t,firmware_venus_image*) __attribute__((weak));
void firmware_venus_release_image(uint64_t) __attribute__((weak));
VkResult firmware_venus_import_memory(VkDevice,const VkMemoryAllocateInfo*,uint32_t,VkDeviceMemory*) __attribute__((weak));
}
namespace uefi_browser {
namespace {
GpuCompositor* active_compositor=nullptr;
#define DEVICE_FUNCTIONS(F) \
 F(DestroyDevice) F(GetDeviceQueue) F(AllocateMemory) F(FreeMemory) F(MapMemory) F(UnmapMemory) \
 F(GetImageMemoryRequirements) F(GetBufferMemoryRequirements) F(CreateImage) F(DestroyImage) \
 F(BindImageMemory) F(GetImageSubresourceLayout) F(CreateCommandPool) F(DestroyCommandPool) \
 F(AllocateCommandBuffers) F(ResetCommandBuffer) F(BeginCommandBuffer) F(EndCommandBuffer) \
 F(CmdPipelineBarrier) F(CmdCopyImage) F(CreateFence) F(DestroyFence) F(ResetFences) \
 F(GetFenceStatus) F(QueueSubmit) F(DeviceWaitIdle)
struct Dispatch {
#define DECLARE(name) PFN_vk##name name = nullptr;
 DEVICE_FUNCTIONS(DECLARE)
#undef DECLARE
 VkDevice device = VK_NULL_HANDLE;
 VkPhysicalDeviceMemoryProperties properties{};
 uint32_t MemoryType(uint32_t bits, VkMemoryPropertyFlags required) const {
   for (uint32_t index=0; index<properties.memoryTypeCount; ++index)
     if ((bits & (1u<<index)) && (properties.memoryTypes[index].propertyFlags & required)==required) return index;
   return UINT32_MAX;
 }
};
class Allocator final : public skgpu::VulkanMemoryAllocator {
 public:
  explicit Allocator(Dispatch* dispatch) : dispatch_(dispatch) {}
  VkResult allocateImageMemory(VkImage image, uint32_t flags, skgpu::VulkanBackendMemory* output) override {
    if (flags & kProtected_AllocationPropertyFlag) return VK_ERROR_FEATURE_NOT_PRESENT;
    VkMemoryRequirements requirements; dispatch_->GetImageMemoryRequirements(dispatch_->device,image,&requirements);
    return Allocate(requirements,VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,image,VK_NULL_HANDLE,output);
  }
  VkResult allocateBufferMemory(VkBuffer buffer, BufferUsage usage, uint32_t flags, skgpu::VulkanBackendMemory* output) override {
    if (flags & kProtected_AllocationPropertyFlag) return VK_ERROR_FEATURE_NOT_PRESENT;
    VkMemoryRequirements requirements; dispatch_->GetBufferMemoryRequirements(dispatch_->device,buffer,&requirements);
    return Allocate(requirements,usage==BufferUsage::kGpuOnly ? VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT :
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT|VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,VK_NULL_HANDLE,buffer,output);
  }
  void getAllocInfo(const skgpu::VulkanBackendMemory& memory, skgpu::VulkanAlloc* info) const override {
    auto* allocation=reinterpret_cast<Allocation*>(memory);
    *info={}; info->fMemory=allocation->memory; info->fSize=allocation->size; info->fBackendMemory=memory;
    if (allocation->mappable) info->fFlags=skgpu::VulkanAlloc::kMappable_Flag;
  }
  VkResult mapMemory(const skgpu::VulkanBackendMemory& memory, void** pointer) override {
    auto* allocation=reinterpret_cast<Allocation*>(memory);
    if (!allocation->mapping) {
      VkResult result=dispatch_->MapMemory(dispatch_->device,allocation->memory,0,VK_WHOLE_SIZE,0,&allocation->mapping);
      if (result!=VK_SUCCESS) return result;
    }
    *pointer=allocation->mapping; return VK_SUCCESS;
  }
  void unmapMemory(const skgpu::VulkanBackendMemory&) override {
    // Persistent mappings avoid repeated Virtio aperture updates for upload buffers.
  }
  void freeMemory(const skgpu::VulkanBackendMemory& memory) override {
    auto* allocation=reinterpret_cast<Allocation*>(memory);
    if (allocation->mapping) dispatch_->UnmapMemory(dispatch_->device,allocation->memory);
    dispatch_->FreeMemory(dispatch_->device,allocation->memory,nullptr);
    bytes_-=allocation->size; delete allocation;
  }
  std::pair<uint64_t,uint64_t> totalAllocatedAndUsedMemory() const override { return {bytes_,bytes_}; }
 private:
  struct Allocation { VkDeviceMemory memory; VkDeviceSize size; bool mappable; void* mapping=nullptr; };
  VkResult Allocate(const VkMemoryRequirements& requirements, VkMemoryPropertyFlags flags,
                    VkImage image, VkBuffer buffer, skgpu::VulkanBackendMemory* output) {
    uint32_t type=dispatch_->MemoryType(requirements.memoryTypeBits,flags);
    if (type==UINT32_MAX) return VK_ERROR_OUT_OF_DEVICE_MEMORY;
    VkMemoryDedicatedAllocateInfo dedicated{VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO};
    dedicated.image=image; dedicated.buffer=buffer;
    VkMemoryAllocateInfo info{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    info.pNext=&dedicated; info.allocationSize=requirements.size; info.memoryTypeIndex=type;
    auto allocation=std::make_unique<Allocation>();
    VkResult result=dispatch_->AllocateMemory(dispatch_->device,&info,nullptr,&allocation->memory);
    if (result!=VK_SUCCESS) return result;
    allocation->size=requirements.size; allocation->mappable=(flags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT)!=0;
    bytes_+=requirements.size; *output=reinterpret_cast<intptr_t>(allocation.release()); return VK_SUCCESS;
  }
  Dispatch* dispatch_;
  uint64_t bytes_=0;
};
}
struct GpuCompositor::State {
  struct Imported {
    VkDevice device=VK_NULL_HANDLE;
    VkImage image=VK_NULL_HANDLE;
    VkDeviceMemory memory=VK_NULL_HANDLE;
    PFN_vkDestroyImage destroy=nullptr;
    PFN_vkFreeMemory free=nullptr;
    void Release() {
      if (!device) return;
      destroy(device,image,nullptr); free(device,memory,nullptr); device=VK_NULL_HANDLE;
    }
    ~Imported() { Release(); }
  };
  std::vector<std::weak_ptr<Imported>> imported;
  Dispatch functions;
  VkInstance instance=VK_NULL_HANDLE;
  PFN_vkDestroyInstance destroy_instance=nullptr;
  VkQueue queue=VK_NULL_HANDLE;
  VkCommandPool pool=VK_NULL_HANDLE;
  VkCommandBuffer command=VK_NULL_HANDLE;
  VkFence fence=VK_NULL_HANDLE;
  sk_sp<GrDirectContext> context;
  struct Output { VkImage image=VK_NULL_HANDLE; VkDeviceMemory memory=VK_NULL_HANDLE; VkSubresourceLayout layout{}; bool used=false, mapped=false; } output[2];
  uint32_t queue_family=0;
  uint64_t scanout_modifier=0;
  int width=0,height=0,next=0;
  bool pending=false,failed=false;
  size_t submitted=0,presented=0,coalesced=0;
  int64_t completion_microseconds=0;
  base::TimeTicks submitted_at;
  ~State() {
    auto& f=functions;
    if (f.device && f.DeviceWaitIdle) f.DeviceWaitIdle(f.device);
    if (context) context->releaseResourcesAndAbandonContext();
    context.reset();
    for (auto& weak:imported) if (auto image=weak.lock()) image->Release();
    for (auto& buffer:output) {
      if (buffer.image) f.DestroyImage(f.device,buffer.image,nullptr);
      if (buffer.memory) { if (buffer.mapped) f.UnmapMemory(f.device,buffer.memory); f.FreeMemory(f.device,buffer.memory,nullptr); }
    }
    if (fence) f.DestroyFence(f.device,fence,nullptr);
    if (pool) f.DestroyCommandPool(f.device,pool,nullptr);
    if (f.device && f.DestroyDevice) f.DestroyDevice(f.device,nullptr);
    if (instance && destroy_instance) destroy_instance(instance,nullptr);
    std::printf("GPU-COMPOSITOR: submitted=%zu presented=%zu coalesced=%zu completion_us=%lld cpu_readback_bytes=0\n",
        submitted,presented,coalesced,static_cast<long long>(completion_microseconds));
  }
  bool InitializationFailed(int line) { std::printf("GPU-COMPOSITOR: initialization failed at line=%d\n",line); return false; }
  bool Initialize(int requested_width,int requested_height) {
    width=requested_width; height=requested_height;
    auto create=reinterpret_cast<PFN_vkCreateInstance>(firmware_venus_get_proc(nullptr,"vkCreateInstance"));
    VkApplicationInfo application{VK_STRUCTURE_TYPE_APPLICATION_INFO};
    application.pApplicationName="UEFI BROWSER compositor"; application.apiVersion=VK_API_VERSION_1_3;
    VkInstanceCreateInfo instance_info{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO}; instance_info.pApplicationInfo=&application;
    if (!create || create(&instance_info,nullptr,&instance)!=VK_SUCCESS) return InitializationFailed(__LINE__);
#define INSTANCE(name) auto name=reinterpret_cast<PFN_vk##name>(firmware_venus_get_proc(instance,"vk" #name)); if (!name) return false
    INSTANCE(DestroyInstance); destroy_instance=DestroyInstance;
    INSTANCE(EnumeratePhysicalDevices); INSTANCE(GetPhysicalDeviceQueueFamilyProperties);
    INSTANCE(GetPhysicalDeviceFormatProperties2); INSTANCE(EnumerateDeviceExtensionProperties); INSTANCE(GetPhysicalDeviceMemoryProperties); INSTANCE(GetPhysicalDeviceFeatures); INSTANCE(CreateDevice); INSTANCE(GetDeviceProcAddr);
    uint32_t count=0;
    if (EnumeratePhysicalDevices(instance,&count,nullptr)!=VK_SUCCESS || !count || count>16) return InitializationFailed(__LINE__);
    std::vector<VkPhysicalDevice> physical(count);
    if (EnumeratePhysicalDevices(instance,&count,physical.data())!=VK_SUCCESS) return InitializationFailed(__LINE__);
    VkPhysicalDevice device=physical[0];
    GetPhysicalDeviceQueueFamilyProperties(device,&count,nullptr);
    std::vector<VkQueueFamilyProperties> families(count); GetPhysicalDeviceQueueFamilyProperties(device,&count,families.data());
    uint32_t family=0; while (family<count && !(families[family].queueFlags&VK_QUEUE_GRAPHICS_BIT)) ++family;
    if (family==count) return InitializationFailed(__LINE__);
    queue_family=family;
    GetPhysicalDeviceMemoryProperties(device,&functions.properties);
    VkPhysicalDeviceFeatures features{}; GetPhysicalDeviceFeatures(device,&features);
    uint32_t extension_count=0;
    if (EnumerateDeviceExtensionProperties(device,nullptr,&extension_count,nullptr)!=VK_SUCCESS) return InitializationFailed(__LINE__);
    std::vector<VkExtensionProperties> extensions(extension_count);
    if (EnumerateDeviceExtensionProperties(device,nullptr,&extension_count,extensions.data())!=VK_SUCCESS) return InitializationFailed(__LINE__);
    bool modifiers_available=false;
    for (const auto& extension:extensions) if (std::string_view(extension.extensionName)==VK_EXT_IMAGE_DRM_FORMAT_MODIFIER_EXTENSION_NAME) modifiers_available=true;
    uint64_t modifier=0;
    if (modifiers_available) {
      VkDrmFormatModifierPropertiesListEXT list{VK_STRUCTURE_TYPE_DRM_FORMAT_MODIFIER_PROPERTIES_LIST_EXT};
      VkFormatProperties2 properties{VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2}; properties.pNext=&list;
      GetPhysicalDeviceFormatProperties2(device,VK_FORMAT_B8G8R8A8_UNORM,&properties);
      std::vector<VkDrmFormatModifierPropertiesEXT> modifiers(list.drmFormatModifierCount);
      list.pDrmFormatModifierProperties=modifiers.data();
      GetPhysicalDeviceFormatProperties2(device,VK_FORMAT_B8G8R8A8_UNORM,&properties);
      for (const auto& entry:modifiers) if (entry.drmFormatModifier && entry.drmFormatModifierPlaneCount==1 && (entry.drmFormatModifierTilingFeatures&VK_FORMAT_FEATURE_TRANSFER_DST_BIT)) { modifier=entry.drmFormatModifier; break; }
    }
    scanout_modifier=modifier;
    float priority=1;
    VkDeviceQueueCreateInfo queue_info{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
    queue_info.queueFamilyIndex=family; queue_info.queueCount=1; queue_info.pQueuePriorities=&priority;
    VkDeviceCreateInfo device_info{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
    device_info.queueCreateInfoCount=1; device_info.pQueueCreateInfos=&queue_info; device_info.pEnabledFeatures=&features;
    const char* extension_names[]={VK_EXT_IMAGE_DRM_FORMAT_MODIFIER_EXTENSION_NAME,VK_EXT_EXTERNAL_MEMORY_DMA_BUF_EXTENSION_NAME,VK_KHR_EXTERNAL_MEMORY_FD_EXTENSION_NAME};
    if (modifier) { device_info.enabledExtensionCount=3; device_info.ppEnabledExtensionNames=extension_names; }
    std::printf("GPU-COMPOSITOR: scanout modifier=%llx\n",static_cast<unsigned long long>(modifier));
    if (CreateDevice(device,&device_info,nullptr,&functions.device)!=VK_SUCCESS) return InitializationFailed(__LINE__);
#define LOAD(name) functions.name=reinterpret_cast<PFN_vk##name>(GetDeviceProcAddr(functions.device,"vk" #name)); if (!functions.name) return InitializationFailed(__LINE__);
    DEVICE_FUNCTIONS(LOAD)
#undef LOAD
    auto& f=functions; f.GetDeviceQueue(f.device,family,0,&queue);
    skgpu::VulkanBackendContext backend;
    backend.fInstance=instance; backend.fPhysicalDevice=device; backend.fDevice=f.device; backend.fQueue=queue;
    backend.fGraphicsQueueIndex=family; backend.fMaxAPIVersion=VK_API_VERSION_1_3; backend.fDeviceFeatures=&features;
    backend.fMemoryAllocator=sk_make_sp<Allocator>(&f);
    backend.fGetProc=[GetDeviceProcAddr](const char* name,VkInstance instance,VkDevice device) {
      return device ? GetDeviceProcAddr(device,name) : firmware_venus_get_proc(instance,name);
    };
    context=GrDirectContexts::MakeVulkan(backend);
    if (!context) return InitializationFailed(__LINE__);
    context->setResourceCacheLimit(64*1024*1024);
    VkCommandPoolCreateInfo pool_info{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    pool_info.flags=VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT; pool_info.queueFamilyIndex=family;
    if (f.CreateCommandPool(f.device,&pool_info,nullptr,&pool)!=VK_SUCCESS) return InitializationFailed(__LINE__);
    VkCommandBufferAllocateInfo command_info{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    command_info.commandPool=pool; command_info.level=VK_COMMAND_BUFFER_LEVEL_PRIMARY; command_info.commandBufferCount=1;
    if (f.AllocateCommandBuffers(f.device,&command_info,&command)!=VK_SUCCESS) return InitializationFailed(__LINE__);
    VkFenceCreateInfo fence_info{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    if (f.CreateFence(f.device,&fence_info,nullptr,&fence)!=VK_SUCCESS) return InitializationFailed(__LINE__);
    for (auto& buffer:output) {
      VkImageCreateInfo image{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
      image.imageType=VK_IMAGE_TYPE_2D; image.format=VK_FORMAT_B8G8R8A8_UNORM;
      image.extent={static_cast<uint32_t>(width),static_cast<uint32_t>(height),1}; image.mipLevels=1; image.arrayLayers=1;
      image.samples=VK_SAMPLE_COUNT_1_BIT; image.tiling=VK_IMAGE_TILING_LINEAR; image.usage=VK_IMAGE_USAGE_TRANSFER_DST_BIT;
      VkImageDrmFormatModifierListCreateInfoEXT modifier_info{VK_STRUCTURE_TYPE_IMAGE_DRM_FORMAT_MODIFIER_LIST_CREATE_INFO_EXT};
      if (modifier) { modifier_info.drmFormatModifierCount=1; modifier_info.pDrmFormatModifiers=&modifier; image.pNext=&modifier_info; image.tiling=VK_IMAGE_TILING_DRM_FORMAT_MODIFIER_EXT; }
      VkExternalMemoryImageCreateInfo external_image{VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO};
      if (modifier) { external_image.handleTypes=VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT; external_image.pNext=image.pNext; image.pNext=&external_image; }
      if (f.CreateImage(f.device,&image,nullptr,&buffer.image)!=VK_SUCCESS) return InitializationFailed(__LINE__);
      VkMemoryRequirements requirements; f.GetImageMemoryRequirements(f.device,buffer.image,&requirements);
      uint32_t type=f.MemoryType(requirements.memoryTypeBits,modifier ? VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT : VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT|VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
      if (type==UINT32_MAX) return InitializationFailed(__LINE__);
      VkMemoryDedicatedAllocateInfo dedicated{VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO}; dedicated.image=buffer.image;
      VkMemoryAllocateInfo memory{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO}; memory.pNext=&dedicated;
      memory.allocationSize=requirements.size; memory.memoryTypeIndex=type;
      VkExportMemoryAllocateInfo export_info{VK_STRUCTURE_TYPE_EXPORT_MEMORY_ALLOCATE_INFO};
      if (modifier) { export_info.handleTypes=VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT; export_info.pNext=memory.pNext; memory.pNext=&export_info; }
      if (f.AllocateMemory(f.device,&memory,nullptr,&buffer.memory)!=VK_SUCCESS ||
          f.BindImageMemory(f.device,buffer.image,buffer.memory,0)!=VK_SUCCESS) return InitializationFailed(__LINE__);
      // Mapping exports the allocation as a Venus blob. The CPU never reads its pixels.
      void* mapping=nullptr;
      if (!modifier) {
        if (f.MapMemory(f.device,buffer.memory,0,VK_WHOLE_SIZE,0,&mapping)!=VK_SUCCESS) return InitializationFailed(__LINE__);
        buffer.mapped=true;
      }
      VkImageSubresource subresource{modifier ? VK_IMAGE_ASPECT_MEMORY_PLANE_0_BIT_EXT : VK_IMAGE_ASPECT_COLOR_BIT,0,0};
      f.GetImageSubresourceLayout(f.device,buffer.image,&subresource,&buffer.layout);
      if (buffer.layout.rowPitch>UINT32_MAX || buffer.layout.offset>UINT32_MAX) return InitializationFailed(__LINE__);
    }
    std::puts("GPU-COMPOSITOR: Skia Vulkan initialized; asynchronous double-buffered Virtio scanout");
    return true;
  }
};
GpuCompositor::GpuCompositor(std::unique_ptr<State> state): state_(std::move(state)) { active_compositor=this; }
GpuCompositor::~GpuCompositor() { if (active_compositor==this) active_compositor=nullptr; }
sk_sp<SkImage> GpuCompositor::CanvasImage(uint64_t token) {
  auto fail=[&](int line) -> sk_sp<SkImage> { std::printf("GPU-CANVAS: import failed token=%llu line=%d\n",static_cast<unsigned long long>(token),line); return {}; };
  if (!active_compositor || !firmware_venus_import_memory || !firmware_venus_acquire_image) return fail(__LINE__);
  auto& state=*active_compositor->state_;
  auto& f=state.functions;
  firmware_venus_image metadata{};
  if (firmware_venus_acquire_image(token,&metadata)) return fail(__LINE__);
  struct Lease { uint64_t token; ~Lease() { firmware_venus_release_image(token); } } lease{token};
  auto image=std::make_shared<State::Imported>();
  image->device=f.device; image->destroy=f.DestroyImage; image->free=f.FreeMemory;
  VkExternalMemoryImageCreateInfo external{VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO}; external.handleTypes=VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT;
  VkImageCreateInfo info{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO}; info.pNext=&external;
  info.imageType=VK_IMAGE_TYPE_2D; info.format=static_cast<VkFormat>(metadata.format); info.extent={metadata.width,metadata.height,1};
  info.mipLevels=1; info.arrayLayers=1; info.samples=VK_SAMPLE_COUNT_1_BIT; info.tiling=VK_IMAGE_TILING_OPTIMAL;
  info.usage=VK_IMAGE_USAGE_TRANSFER_SRC_BIT|VK_IMAGE_USAGE_TRANSFER_DST_BIT|VK_IMAGE_USAGE_SAMPLED_BIT;
  if (f.CreateImage(f.device,&info,nullptr,&image->image)!=VK_SUCCESS) return fail(__LINE__);
  VkMemoryRequirements requirements{}; f.GetImageMemoryRequirements(f.device,image->image,&requirements);
  if (metadata.memory_type>=f.properties.memoryTypeCount || !(requirements.memoryTypeBits&(1u<<metadata.memory_type)) || requirements.size>metadata.size) return fail(__LINE__);
  VkMemoryDedicatedAllocateInfo dedicated{VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO}; dedicated.image=image->image;
  VkMemoryAllocateInfo memory{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO}; memory.pNext=&dedicated;
  memory.allocationSize=metadata.size; memory.memoryTypeIndex=metadata.memory_type;
  if (firmware_venus_import_memory(f.device,&memory,metadata.resource,&image->memory)!=VK_SUCCESS || f.BindImageMemory(f.device,image->image,image->memory,0)!=VK_SUCCESS) return fail(__LINE__);
  GrVkImageInfo texture{}; texture.fImage=image->image; texture.fImageTiling=VK_IMAGE_TILING_OPTIMAL;
  texture.fImageLayout=VK_IMAGE_LAYOUT_GENERAL; texture.fFormat=info.format; texture.fImageUsageFlags=info.usage;
  texture.fSampleCount=1; texture.fLevelCount=1; texture.fCurrentQueueFamily=VK_QUEUE_FAMILY_EXTERNAL;
  auto backend=GrBackendTextures::MakeVk(metadata.width,metadata.height,texture);
  auto* lifetime=new std::shared_ptr<State::Imported>(image);
  auto result=SkImages::BorrowTextureFrom(state.context.get(),backend,
      (metadata.alpha&2)!=0 ? kBottomLeft_GrSurfaceOrigin : kTopLeft_GrSurfaceOrigin,
      metadata.format==97 ? kRGBA_F16_SkColorType : metadata.format==37 ? kRGBA_8888_SkColorType : kBGRA_8888_SkColorType,
      (metadata.alpha&1)==0 ? kOpaque_SkAlphaType : (metadata.alpha&4) ? kUnpremul_SkAlphaType : kPremul_SkAlphaType,
      blink::PredefinedColorSpaceToSkColorSpace(static_cast<blink::PredefinedColorSpace>(metadata.alpha>>3)),
      [](void* value) { delete static_cast<std::shared_ptr<State::Imported>*>(value); },lifetime);
  if (!result) return fail(__LINE__);
  std::erase_if(state.imported,[](const auto& weak){return weak.expired();});
  state.imported.push_back(image);
  std::printf("GPU-CANVAS: imported token=%llu size=%ux%u cpu_readback_bytes=0\n",static_cast<unsigned long long>(token),metadata.width,metadata.height);
  return result;
}
bool GpuCompositor::Requested() { return firmware_gpu_composition_enabled && firmware_gpu_composition_enabled(); }
std::unique_ptr<GpuCompositor> GpuCompositor::Create(int width,int height) {
  if (!Requested() || !firmware_venus_get_proc || !firmware_venus_scanout_memory) return nullptr;
  auto state=std::make_unique<State>();
  if (!state->Initialize(width,height)) { std::puts("GPU-COMPOSITOR: initialization failed"); return nullptr; }
  return std::unique_ptr<GpuCompositor>(new GpuCompositor(std::move(state)));
}
sk_sp<SkSurface> GpuCompositor::MakeSurface(int width,int height) {
  return SkSurfaces::RenderTarget(state_->context.get(),skgpu::Budgeted::kYes,
      SkImageInfo::Make(width,height,kBGRA_8888_SkColorType,kPremul_SkAlphaType),0,kTopLeft_GrSurfaceOrigin,nullptr);
}
void GpuCompositor::Poll() {
  auto& state=*state_; auto& f=state.functions;
  if (!state.pending || state.failed) return;
  VkResult result=f.GetFenceStatus(f.device,state.fence);
  if (result==VK_NOT_READY) return;
  auto& buffer=state.output[state.next];
  if (result!=VK_SUCCESS || firmware_venus_scanout_memory(f.device,buffer.memory,state.width,state.height,static_cast<uint32_t>(buffer.layout.rowPitch),static_cast<uint32_t>(buffer.layout.offset),state.scanout_modifier)) {
    state.failed=true; std::puts("FAIL: GPU-COMPOSITOR scanout failed"); return;
  }
  state.completion_microseconds+=(base::TimeTicks::Now()-state.submitted_at).InMicroseconds();
  state.pending=false; state.next^=1;
  if (!state.presented++) std::puts("GPU-COMPOSITOR: first frame presented; cpu_readback_bytes=0");
}
bool GpuCompositor::CanPresent() {
  if (state_->pending) ++state_->coalesced;
  return !state_->pending && !state_->failed;
}
bool GpuCompositor::Present(SkSurface& surface) {
  Poll();
  auto& state=*state_; auto& f=state.functions;
  if (state.failed) return false;
  if (state.pending) { ++state.coalesced; return false; }
  auto target=SkSurfaces::GetBackendRenderTarget(&surface,SkSurface::kFlushRead_BackendHandleAccess);
  GrVkImageInfo source;
  if (!GrBackendRenderTargets::GetVkImageInfo(target,&source)) return false;
  state.context->flushAndSubmit(&surface,GrSyncCpu::kNo);
  auto& buffer=state.output[state.next];
  if (f.ResetCommandBuffer(state.command,0)!=VK_SUCCESS || f.ResetFences(f.device,1,&state.fence)!=VK_SUCCESS) return false;
  VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO}; begin.flags=VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
  if (f.BeginCommandBuffer(state.command,&begin)!=VK_SUCCESS) return false;
  VkImageMemoryBarrier barriers[2]{};
  for (auto& barrier:barriers) {
    barrier.sType=VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    barrier.srcQueueFamilyIndex=barrier.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED;
    barrier.subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1};
  }
  barriers[0].image=source.fImage; barriers[0].oldLayout=source.fImageLayout; barriers[0].newLayout=VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
  barriers[0].srcAccessMask=VK_ACCESS_MEMORY_WRITE_BIT; barriers[0].dstAccessMask=VK_ACCESS_TRANSFER_READ_BIT;
  barriers[1].image=buffer.image; barriers[1].oldLayout=buffer.used ? VK_IMAGE_LAYOUT_GENERAL : VK_IMAGE_LAYOUT_UNDEFINED;
  barriers[1].newLayout=VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL; barriers[1].dstAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT;
  if (buffer.used) {
    barriers[1].srcQueueFamilyIndex=VK_QUEUE_FAMILY_EXTERNAL;
    barriers[1].dstQueueFamilyIndex=state.queue_family;
  }
  f.CmdPipelineBarrier(state.command,VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT,0,0,nullptr,0,nullptr,2,barriers);
  VkImageCopy copy{}; copy.srcSubresource=copy.dstSubresource={VK_IMAGE_ASPECT_COLOR_BIT,0,0,1};
  copy.extent={static_cast<uint32_t>(state.width),static_cast<uint32_t>(state.height),1};
  f.CmdCopyImage(state.command,source.fImage,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,buffer.image,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,1,&copy);
  barriers[0].oldLayout=VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL; barriers[0].newLayout=source.fImageLayout;
  barriers[0].srcAccessMask=VK_ACCESS_TRANSFER_READ_BIT; barriers[0].dstAccessMask=VK_ACCESS_MEMORY_READ_BIT|VK_ACCESS_MEMORY_WRITE_BIT;
  barriers[1].oldLayout=VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL; barriers[1].newLayout=VK_IMAGE_LAYOUT_GENERAL;
  barriers[1].srcAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT; barriers[1].dstAccessMask=0;
  barriers[1].srcQueueFamilyIndex=state.queue_family;
  barriers[1].dstQueueFamilyIndex=VK_QUEUE_FAMILY_EXTERNAL;
  f.CmdPipelineBarrier(state.command,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,0,0,nullptr,0,nullptr,2,barriers);
  if (f.EndCommandBuffer(state.command)!=VK_SUCCESS) return false;
  VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO}; submit.commandBufferCount=1; submit.pCommandBuffers=&state.command;
  if (f.QueueSubmit(state.queue,1,&submit,state.fence)!=VK_SUCCESS) return false;
  state.pending=true; buffer.used=true; ++state.submitted; state.submitted_at=base::TimeTicks::Now(); return true;
}
}

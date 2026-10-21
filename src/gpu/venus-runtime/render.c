/* SPDX-License-Identifier: BSD-3-Clause */
#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
extern PFN_vkVoidFunction vk_icdGetInstanceProcAddr(VkInstance, const char *);
extern void graphics_log(const char *);
extern void firmware_venus_present_image(const uint8_t *, uint32_t);
static VkDevice render_device;
#define REQUIRE(condition) do { if (!(condition)) { \
    printf("FAIL: Mesa rendering at line %d: %s\n", __LINE__, #condition); abort(); } } while (0)
static _Noreturn void venus_stream_failure(const char *reason) {
    printf("FAIL: Mesa rendering: %s\n", reason);
    abort();
}
static void render_number(const char *name, uint32_t value) {
    printf("%s%u\n", name, value);
}
static PFN_vkAllocateCommandBuffers vkAllocateCommandBuffers;
static PFN_vkAllocateMemory vkAllocateMemory;
static PFN_vkBeginCommandBuffer vkBeginCommandBuffer;
static PFN_vkBindBufferMemory vkBindBufferMemory;
static PFN_vkBindImageMemory vkBindImageMemory;
static PFN_vkCmdBeginRenderPass vkCmdBeginRenderPass;
static PFN_vkCmdBindPipeline vkCmdBindPipeline;
static PFN_vkCmdCopyImageToBuffer vkCmdCopyImageToBuffer;
static PFN_vkCmdDraw vkCmdDraw;
static PFN_vkCmdEndRenderPass vkCmdEndRenderPass;
static PFN_vkCmdPipelineBarrier vkCmdPipelineBarrier;
static PFN_vkCreateBuffer vkCreateBuffer;
static PFN_vkCreateCommandPool vkCreateCommandPool;
static PFN_vkCreateFence vkCreateFence;
static PFN_vkCreateFramebuffer vkCreateFramebuffer;
static PFN_vkCreateGraphicsPipelines vkCreateGraphicsPipelines;
static PFN_vkCreateImage vkCreateImage;
static PFN_vkCreateImageView vkCreateImageView;
static PFN_vkCreatePipelineLayout vkCreatePipelineLayout;
static PFN_vkCreateRenderPass vkCreateRenderPass;
static PFN_vkCreateShaderModule vkCreateShaderModule;
static PFN_vkDestroyBuffer vkDestroyBuffer;
static PFN_vkDestroyCommandPool vkDestroyCommandPool;
static PFN_vkDestroyFence vkDestroyFence;
static PFN_vkDestroyFramebuffer vkDestroyFramebuffer;
static PFN_vkDestroyImage vkDestroyImage;
static PFN_vkDestroyImageView vkDestroyImageView;
static PFN_vkDestroyPipeline vkDestroyPipeline;
static PFN_vkDestroyPipelineLayout vkDestroyPipelineLayout;
static PFN_vkDestroyRenderPass vkDestroyRenderPass;
static PFN_vkDestroyShaderModule vkDestroyShaderModule;
static PFN_vkEndCommandBuffer vkEndCommandBuffer;
static PFN_vkFreeMemory vkFreeMemory;
static PFN_vkGetBufferMemoryRequirements vkGetBufferMemoryRequirements;
static PFN_vkGetImageMemoryRequirements vkGetImageMemoryRequirements;
static PFN_vkGetPhysicalDeviceFormatProperties vkGetPhysicalDeviceFormatProperties;
static PFN_vkGetPhysicalDeviceMemoryProperties vkGetPhysicalDeviceMemoryProperties;
static PFN_vkMapMemory vkMapMemory;
static PFN_vkQueueSubmit vkQueueSubmit;
static PFN_vkUnmapMemory vkUnmapMemory;
static PFN_vkWaitForFences vkWaitForFences;
#include "venus-shaders.h"
#define VENUS_RENDER_WIDTH 128u
#define VENUS_RENDER_BYTES (VENUS_RENDER_WIDTH*VENUS_RENDER_WIDTH*4u)
static struct {
  VkImage image;
  VkDeviceMemory image_memory, readback_memory;
  VkImageView view;
  VkBuffer buffer;
  VkRenderPass render_pass;
  VkFramebuffer framebuffer;
  VkPipelineLayout layout;
  VkPipeline pipeline;
  VkShaderModule vertex, fragment;
  uint64_t readback_allocation;
} render_state;
static uint8_t render_pixels[VENUS_RENDER_BYTES];
static uint32_t venus_memory_type(const VkPhysicalDeviceMemoryProperties *properties,
                                 uint32_t allowed, VkMemoryPropertyFlags required) {
  if(properties->memoryTypeCount>VK_MAX_MEMORY_TYPES || properties->memoryHeapCount>VK_MAX_MEMORY_HEAPS)
    venus_stream_failure("memory property count");
  for(uint32_t index=0;index<properties->memoryTypeCount;++index)
    if((allowed&(1u<<index)) && (properties->memoryTypes[index].propertyFlags&required)==required)
      return index;
  venus_stream_failure("compatible Vulkan memory type");
}
static void venus_render_create(VkPhysicalDevice physical_device,VkDevice logical_device) {
  memset(&render_state,0,sizeof(render_state));
  VkPhysicalDeviceMemoryProperties memory_properties={0};
  vkGetPhysicalDeviceMemoryProperties(physical_device,&memory_properties);
  VkFormatProperties format_properties={0};
  vkGetPhysicalDeviceFormatProperties(physical_device,VK_FORMAT_R8G8B8A8_UNORM,&format_properties);
  const VkFormatFeatureFlags required=VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT|VK_FORMAT_FEATURE_TRANSFER_SRC_BIT;
  if((format_properties.optimalTilingFeatures&required)!=required) venus_stream_failure("RGBA render target support");
  const VkImageCreateInfo image_info={.sType=VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,.imageType=VK_IMAGE_TYPE_2D,
    .format=VK_FORMAT_R8G8B8A8_UNORM,.extent={VENUS_RENDER_WIDTH,VENUS_RENDER_WIDTH,1},.mipLevels=1,.arrayLayers=1,
    .samples=VK_SAMPLE_COUNT_1_BIT,.tiling=VK_IMAGE_TILING_OPTIMAL,
    .usage=VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT|VK_IMAGE_USAGE_TRANSFER_SRC_BIT,.sharingMode=VK_SHARING_MODE_EXCLUSIVE};
  REQUIRE(vkCreateImage(logical_device,&image_info,NULL,&render_state.image) == VK_SUCCESS);
  VkMemoryRequirements image_requirements={0};
  vkGetImageMemoryRequirements(logical_device,render_state.image,&image_requirements);
  if(!image_requirements.size || image_requirements.size>32*1024*1024) venus_stream_failure("render target allocation size");
  const VkMemoryDedicatedAllocateInfo image_dedicated={.sType=VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO,.image=render_state.image};
  const VkMemoryAllocateInfo image_allocate={.sType=VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,.pNext=&image_dedicated,
    .allocationSize=image_requirements.size,.memoryTypeIndex=venus_memory_type(&memory_properties,image_requirements.memoryTypeBits,VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)};
  REQUIRE(vkAllocateMemory(logical_device,&image_allocate,NULL,&render_state.image_memory) == VK_SUCCESS);
  REQUIRE(vkBindImageMemory(logical_device,render_state.image,render_state.image_memory,0) == VK_SUCCESS);
  const VkImageViewCreateInfo view_info={.sType=VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,.image=render_state.image,
    .viewType=VK_IMAGE_VIEW_TYPE_2D,.format=VK_FORMAT_R8G8B8A8_UNORM,.subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1}};
  REQUIRE(vkCreateImageView(logical_device,&view_info,NULL,&render_state.view) == VK_SUCCESS);
  const VkBufferCreateInfo buffer_info={.sType=VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,.size=VENUS_RENDER_BYTES,
    .usage=VK_BUFFER_USAGE_TRANSFER_DST_BIT,.sharingMode=VK_SHARING_MODE_EXCLUSIVE};
  REQUIRE(vkCreateBuffer(logical_device,&buffer_info,NULL,&render_state.buffer) == VK_SUCCESS);
  VkMemoryRequirements buffer_requirements={0};
  vkGetBufferMemoryRequirements(logical_device,render_state.buffer,&buffer_requirements);
  if(buffer_requirements.size<VENUS_RENDER_BYTES || buffer_requirements.size>32*1024*1024)
    venus_stream_failure("readback allocation size");
  render_state.readback_allocation=(buffer_requirements.size+4095)&~4095ull;
  const VkMemoryDedicatedAllocateInfo buffer_dedicated={.sType=VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO,.buffer=render_state.buffer};
  const VkMemoryAllocateInfo buffer_allocate={.sType=VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,.pNext=&buffer_dedicated,
    .allocationSize=render_state.readback_allocation,
    .memoryTypeIndex=venus_memory_type(&memory_properties,buffer_requirements.memoryTypeBits,VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT|VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)};
  REQUIRE(vkAllocateMemory(logical_device,&buffer_allocate,NULL,&render_state.readback_memory) == VK_SUCCESS);
  REQUIRE(vkBindBufferMemory(logical_device,render_state.buffer,render_state.readback_memory,0) == VK_SUCCESS);
  const VkAttachmentDescription attachment={.format=VK_FORMAT_R8G8B8A8_UNORM,.samples=VK_SAMPLE_COUNT_1_BIT,
    .loadOp=VK_ATTACHMENT_LOAD_OP_CLEAR,.storeOp=VK_ATTACHMENT_STORE_OP_STORE,
    .stencilLoadOp=VK_ATTACHMENT_LOAD_OP_DONT_CARE,.stencilStoreOp=VK_ATTACHMENT_STORE_OP_DONT_CARE,
    .initialLayout=VK_IMAGE_LAYOUT_UNDEFINED,.finalLayout=VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
  const VkAttachmentReference color_reference={0,VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
  const VkSubpassDescription subpass={.pipelineBindPoint=VK_PIPELINE_BIND_POINT_GRAPHICS,.colorAttachmentCount=1,.pColorAttachments=&color_reference};
  const VkSubpassDependency dependency={.srcSubpass=VK_SUBPASS_EXTERNAL,.dstSubpass=0,
    .srcStageMask=VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,.dstStageMask=VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
    .dstAccessMask=VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT};
  const VkRenderPassCreateInfo pass_info={.sType=VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO,.attachmentCount=1,.pAttachments=&attachment,
    .subpassCount=1,.pSubpasses=&subpass,.dependencyCount=1,.pDependencies=&dependency};
  REQUIRE(vkCreateRenderPass(logical_device,&pass_info,NULL,&render_state.render_pass) == VK_SUCCESS);
  const VkFramebufferCreateInfo framebuffer_info={.sType=VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO,
    .renderPass=render_state.render_pass,.attachmentCount=1,.pAttachments=&render_state.view,
    .width=VENUS_RENDER_WIDTH,.height=VENUS_RENDER_WIDTH,.layers=1};
  REQUIRE(vkCreateFramebuffer(logical_device,&framebuffer_info,NULL,&render_state.framebuffer) == VK_SUCCESS);
  const VkShaderModuleCreateInfo vertex_info={.sType=VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
    .codeSize=sizeof(venus_vertex_shader),.pCode=venus_vertex_shader};
  const VkShaderModuleCreateInfo fragment_info={.sType=VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
    .codeSize=sizeof(venus_fragment_shader),.pCode=venus_fragment_shader};
  REQUIRE(vkCreateShaderModule(logical_device,&vertex_info,NULL,&render_state.vertex) == VK_SUCCESS);
  REQUIRE(vkCreateShaderModule(logical_device,&fragment_info,NULL,&render_state.fragment) == VK_SUCCESS);
  const VkPipelineLayoutCreateInfo layout_info={.sType=VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
  REQUIRE(vkCreatePipelineLayout(logical_device,&layout_info,NULL,&render_state.layout) == VK_SUCCESS);
  const VkPipelineShaderStageCreateInfo stages[2]={
    {.sType=VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,.stage=VK_SHADER_STAGE_VERTEX_BIT,.module=render_state.vertex,.pName="main"},
    {.sType=VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,.stage=VK_SHADER_STAGE_FRAGMENT_BIT,.module=render_state.fragment,.pName="main"}};
  const VkPipelineVertexInputStateCreateInfo vertex_input={.sType=VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
  const VkPipelineInputAssemblyStateCreateInfo assembly={.sType=VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,.topology=VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST};
  const VkViewport viewport={0,0,VENUS_RENDER_WIDTH,VENUS_RENDER_WIDTH,0,1};
  const VkRect2D scissor={{0,0},{VENUS_RENDER_WIDTH,VENUS_RENDER_WIDTH}};
  const VkPipelineViewportStateCreateInfo viewport_info={.sType=VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,
    .viewportCount=1,.pViewports=&viewport,.scissorCount=1,.pScissors=&scissor};
  const VkPipelineRasterizationStateCreateInfo raster={.sType=VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
    .polygonMode=VK_POLYGON_MODE_FILL,.cullMode=VK_CULL_MODE_NONE,.frontFace=VK_FRONT_FACE_COUNTER_CLOCKWISE,.lineWidth=1.0f};
  const VkPipelineMultisampleStateCreateInfo samples={.sType=VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,.rasterizationSamples=VK_SAMPLE_COUNT_1_BIT};
  const VkPipelineColorBlendAttachmentState blend_attachment={.colorWriteMask=15};
  const VkPipelineColorBlendStateCreateInfo blend={.sType=VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,.attachmentCount=1,.pAttachments=&blend_attachment};
  const VkGraphicsPipelineCreateInfo pipeline_info={.sType=VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,.stageCount=2,.pStages=stages,
    .pVertexInputState=&vertex_input,.pInputAssemblyState=&assembly,.pViewportState=&viewport_info,.pRasterizationState=&raster,
    .pMultisampleState=&samples,.pColorBlendState=&blend,.layout=render_state.layout,.renderPass=render_state.render_pass,.basePipelineIndex=-1};
  REQUIRE(vkCreateGraphicsPipelines(logical_device,VK_NULL_HANDLE,1,&pipeline_info,NULL,&render_state.pipeline) == VK_SUCCESS);
  graphics_log("PASS: Venus image, memory and graphics pipeline created\n");
}
static void venus_render_record(VkCommandBuffer command_buffer) {
  const VkClearValue clear={.color={.float32={16.0f/255.0f,32.0f/255.0f,48.0f/255.0f,1.0f}}};
  const VkRenderPassBeginInfo begin={.sType=VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,.renderPass=render_state.render_pass,
    .framebuffer=render_state.framebuffer,.renderArea={{0,0},{VENUS_RENDER_WIDTH,VENUS_RENDER_WIDTH}},.clearValueCount=1,.pClearValues=&clear};
  vkCmdBeginRenderPass(command_buffer,&begin,VK_SUBPASS_CONTENTS_INLINE);
  vkCmdBindPipeline(command_buffer,VK_PIPELINE_BIND_POINT_GRAPHICS,render_state.pipeline);
  vkCmdDraw(command_buffer,3,1,0,0);
  vkCmdEndRenderPass(command_buffer);
  const VkImageMemoryBarrier image_barrier={.sType=VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
    .srcAccessMask=VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,.dstAccessMask=VK_ACCESS_TRANSFER_READ_BIT,
    .oldLayout=VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,.newLayout=VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
    .srcQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED,.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED,
    .image=render_state.image,.subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1}};
  vkCmdPipelineBarrier(command_buffer,VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT,0,0,NULL,0,NULL,1,&image_barrier);
  const VkBufferImageCopy region={.imageSubresource={VK_IMAGE_ASPECT_COLOR_BIT,0,0,1},.imageExtent={VENUS_RENDER_WIDTH,VENUS_RENDER_WIDTH,1}};
  vkCmdCopyImageToBuffer(command_buffer,render_state.image,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,render_state.buffer,1,&region);
  const VkBufferMemoryBarrier buffer_barrier={.sType=VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER,
    .srcAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT,.dstAccessMask=VK_ACCESS_HOST_READ_BIT,
    .srcQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED,.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED,
    .buffer=render_state.buffer,.size=VENUS_RENDER_BYTES};
  vkCmdPipelineBarrier(command_buffer,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_HOST_BIT,0,0,NULL,1,&buffer_barrier,0,NULL);
}
static void venus_render_readback(void) {
  void *mapping = NULL;
  REQUIRE(vkMapMemory(render_device, render_state.readback_memory, 0,
                      VENUS_RENDER_BYTES, 0, &mapping) == VK_SUCCESS);
  memcpy(render_pixels, mapping, sizeof(render_pixels));
  vkUnmapMemory(render_device, render_state.readback_memory);
  uint32_t interior=0,background=0,checksum=2166136261u;
  for(uint32_t y=0;y<VENUS_RENDER_WIDTH;++y) for(uint32_t x=0;x<VENUS_RENDER_WIDTH;++x) {
    const uint8_t *pixel=render_pixels+4*(y*VENUS_RENDER_WIDTH+x);
    const float blue=((float)y+0.5f-16.0f)/96.0f;
    const float green=(((float)x+0.5f-16.0f)-48.0f*blue)/96.0f;
    const float red=1.0f-green-blue;
    if(red>0.02f && green>0.02f && blue>0.02f) {
      const float expected[3]={red*255.0f,green*255.0f,blue*255.0f};
      for(unsigned channel=0;channel<3;++channel)
        if((float)pixel[channel]<expected[channel]-2.0f || (float)pixel[channel]>expected[channel]+2.0f)
          venus_stream_failure("GPU interpolated triangle pixel");
      ++interior;
    } else if(red< -0.02f || green< -0.02f || blue< -0.02f) {
      if(pixel[0]!=16 || pixel[1]!=32 || pixel[2]!=48) venus_stream_failure("GPU background pixel");
      ++background;
    }
    if(pixel[3]!=255) venus_stream_failure("GPU image alpha");
    for(unsigned channel=0;channel<4;++channel) checksum=(checksum^pixel[channel])*16777619u;
  }
  if(interior<3500 || background<10000) venus_stream_failure("GPU image coverage");
  render_number("VENUS: triangle_interior_pixels=",interior); render_number("VENUS: background_pixels=",background);
  render_number("VENUS: rgba_fnv1a=",checksum);
  graphics_log("PASS: Venus GPU triangle pixels validated\n");
  firmware_venus_present_image(render_pixels, VENUS_RENDER_WIDTH);

}
static void venus_render_destroy(VkDevice logical_device) {
  vkDestroyPipeline(logical_device,render_state.pipeline,NULL);
  vkDestroyPipelineLayout(logical_device,render_state.layout,NULL);
  vkDestroyShaderModule(logical_device,render_state.fragment,NULL);
  vkDestroyShaderModule(logical_device,render_state.vertex,NULL);
  vkDestroyFramebuffer(logical_device,render_state.framebuffer,NULL);
  vkDestroyRenderPass(logical_device,render_state.render_pass,NULL);
  vkDestroyImageView(logical_device,render_state.view,NULL);
  vkDestroyImage(logical_device,render_state.image,NULL);
  vkDestroyBuffer(logical_device,render_state.buffer,NULL);
  vkFreeMemory(logical_device,render_state.readback_memory,NULL);
  vkFreeMemory(logical_device,render_state.image_memory,NULL);
  graphics_log("PASS: Venus render resources destroyed\n");
}

void run_venus_runtime_render(VkInstance instance, VkPhysicalDevice physical,
                              VkDevice device, VkQueue queue, uint32_t family)
{
    render_device = device;
    PFN_vkGetDeviceProcAddr get_device = (PFN_vkGetDeviceProcAddr)
        vk_icdGetInstanceProcAddr(instance, "vkGetDeviceProcAddr");
    REQUIRE(get_device);
    vkAllocateCommandBuffers = (PFN_vkAllocateCommandBuffers)get_device(device, "vkAllocateCommandBuffers"); REQUIRE(vkAllocateCommandBuffers);
    vkAllocateMemory = (PFN_vkAllocateMemory)get_device(device, "vkAllocateMemory"); REQUIRE(vkAllocateMemory);
    vkBeginCommandBuffer = (PFN_vkBeginCommandBuffer)get_device(device, "vkBeginCommandBuffer"); REQUIRE(vkBeginCommandBuffer);
    vkBindBufferMemory = (PFN_vkBindBufferMemory)get_device(device, "vkBindBufferMemory"); REQUIRE(vkBindBufferMemory);
    vkBindImageMemory = (PFN_vkBindImageMemory)get_device(device, "vkBindImageMemory"); REQUIRE(vkBindImageMemory);
    vkCmdBeginRenderPass = (PFN_vkCmdBeginRenderPass)get_device(device, "vkCmdBeginRenderPass"); REQUIRE(vkCmdBeginRenderPass);
    vkCmdBindPipeline = (PFN_vkCmdBindPipeline)get_device(device, "vkCmdBindPipeline"); REQUIRE(vkCmdBindPipeline);
    vkCmdCopyImageToBuffer = (PFN_vkCmdCopyImageToBuffer)get_device(device, "vkCmdCopyImageToBuffer"); REQUIRE(vkCmdCopyImageToBuffer);
    vkCmdDraw = (PFN_vkCmdDraw)get_device(device, "vkCmdDraw"); REQUIRE(vkCmdDraw);
    vkCmdEndRenderPass = (PFN_vkCmdEndRenderPass)get_device(device, "vkCmdEndRenderPass"); REQUIRE(vkCmdEndRenderPass);
    vkCmdPipelineBarrier = (PFN_vkCmdPipelineBarrier)get_device(device, "vkCmdPipelineBarrier"); REQUIRE(vkCmdPipelineBarrier);
    vkCreateBuffer = (PFN_vkCreateBuffer)get_device(device, "vkCreateBuffer"); REQUIRE(vkCreateBuffer);
    vkCreateCommandPool = (PFN_vkCreateCommandPool)get_device(device, "vkCreateCommandPool"); REQUIRE(vkCreateCommandPool);
    vkCreateFence = (PFN_vkCreateFence)get_device(device, "vkCreateFence"); REQUIRE(vkCreateFence);
    vkCreateFramebuffer = (PFN_vkCreateFramebuffer)get_device(device, "vkCreateFramebuffer"); REQUIRE(vkCreateFramebuffer);
    vkCreateGraphicsPipelines = (PFN_vkCreateGraphicsPipelines)get_device(device, "vkCreateGraphicsPipelines"); REQUIRE(vkCreateGraphicsPipelines);
    vkCreateImage = (PFN_vkCreateImage)get_device(device, "vkCreateImage"); REQUIRE(vkCreateImage);
    vkCreateImageView = (PFN_vkCreateImageView)get_device(device, "vkCreateImageView"); REQUIRE(vkCreateImageView);
    vkCreatePipelineLayout = (PFN_vkCreatePipelineLayout)get_device(device, "vkCreatePipelineLayout"); REQUIRE(vkCreatePipelineLayout);
    vkCreateRenderPass = (PFN_vkCreateRenderPass)get_device(device, "vkCreateRenderPass"); REQUIRE(vkCreateRenderPass);
    vkCreateShaderModule = (PFN_vkCreateShaderModule)get_device(device, "vkCreateShaderModule"); REQUIRE(vkCreateShaderModule);
    vkDestroyBuffer = (PFN_vkDestroyBuffer)get_device(device, "vkDestroyBuffer"); REQUIRE(vkDestroyBuffer);
    vkDestroyCommandPool = (PFN_vkDestroyCommandPool)get_device(device, "vkDestroyCommandPool"); REQUIRE(vkDestroyCommandPool);
    vkDestroyFence = (PFN_vkDestroyFence)get_device(device, "vkDestroyFence"); REQUIRE(vkDestroyFence);
    vkDestroyFramebuffer = (PFN_vkDestroyFramebuffer)get_device(device, "vkDestroyFramebuffer"); REQUIRE(vkDestroyFramebuffer);
    vkDestroyImage = (PFN_vkDestroyImage)get_device(device, "vkDestroyImage"); REQUIRE(vkDestroyImage);
    vkDestroyImageView = (PFN_vkDestroyImageView)get_device(device, "vkDestroyImageView"); REQUIRE(vkDestroyImageView);
    vkDestroyPipeline = (PFN_vkDestroyPipeline)get_device(device, "vkDestroyPipeline"); REQUIRE(vkDestroyPipeline);
    vkDestroyPipelineLayout = (PFN_vkDestroyPipelineLayout)get_device(device, "vkDestroyPipelineLayout"); REQUIRE(vkDestroyPipelineLayout);
    vkDestroyRenderPass = (PFN_vkDestroyRenderPass)get_device(device, "vkDestroyRenderPass"); REQUIRE(vkDestroyRenderPass);
    vkDestroyShaderModule = (PFN_vkDestroyShaderModule)get_device(device, "vkDestroyShaderModule"); REQUIRE(vkDestroyShaderModule);
    vkEndCommandBuffer = (PFN_vkEndCommandBuffer)get_device(device, "vkEndCommandBuffer"); REQUIRE(vkEndCommandBuffer);
    vkFreeMemory = (PFN_vkFreeMemory)get_device(device, "vkFreeMemory"); REQUIRE(vkFreeMemory);
    vkGetBufferMemoryRequirements = (PFN_vkGetBufferMemoryRequirements)get_device(device, "vkGetBufferMemoryRequirements"); REQUIRE(vkGetBufferMemoryRequirements);
    vkGetImageMemoryRequirements = (PFN_vkGetImageMemoryRequirements)get_device(device, "vkGetImageMemoryRequirements"); REQUIRE(vkGetImageMemoryRequirements);
    vkGetPhysicalDeviceFormatProperties = (PFN_vkGetPhysicalDeviceFormatProperties)vk_icdGetInstanceProcAddr(instance, "vkGetPhysicalDeviceFormatProperties"); REQUIRE(vkGetPhysicalDeviceFormatProperties);
    vkGetPhysicalDeviceMemoryProperties = (PFN_vkGetPhysicalDeviceMemoryProperties)vk_icdGetInstanceProcAddr(instance, "vkGetPhysicalDeviceMemoryProperties"); REQUIRE(vkGetPhysicalDeviceMemoryProperties);
    vkMapMemory = (PFN_vkMapMemory)get_device(device, "vkMapMemory"); REQUIRE(vkMapMemory);
    vkQueueSubmit = (PFN_vkQueueSubmit)get_device(device, "vkQueueSubmit"); REQUIRE(vkQueueSubmit);
    vkUnmapMemory = (PFN_vkUnmapMemory)get_device(device, "vkUnmapMemory"); REQUIRE(vkUnmapMemory);
    vkWaitForFences = (PFN_vkWaitForFences)get_device(device, "vkWaitForFences"); REQUIRE(vkWaitForFences);
    venus_render_create(physical, device);
    VkCommandPoolCreateInfo pool_info = {.sType=VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
        .queueFamilyIndex=family};
    VkCommandPool pool;
    REQUIRE(vkCreateCommandPool(device, &pool_info, NULL, &pool) == VK_SUCCESS);
    VkCommandBufferAllocateInfo allocation = {.sType=VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
        .commandPool=pool, .level=VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount=1};
    VkCommandBuffer command;
    REQUIRE(vkAllocateCommandBuffers(device, &allocation, &command) == VK_SUCCESS);
    VkCommandBufferBeginInfo begin = {.sType=VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    REQUIRE(vkBeginCommandBuffer(command, &begin) == VK_SUCCESS);
    venus_render_record(command);
    REQUIRE(vkEndCommandBuffer(command) == VK_SUCCESS);
    VkFenceCreateInfo fence_info = {.sType=VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    VkFence fence;
    REQUIRE(vkCreateFence(device, &fence_info, NULL, &fence) == VK_SUCCESS);
    VkSubmitInfo submission = {.sType=VK_STRUCTURE_TYPE_SUBMIT_INFO,
        .commandBufferCount=1, .pCommandBuffers=&command};
    REQUIRE(vkQueueSubmit(queue, 1, &submission, fence) == VK_SUCCESS);
    REQUIRE(vkWaitForFences(device, 1, &fence, VK_TRUE, UINT64_C(10000000000)) == VK_SUCCESS);
    venus_render_readback();
    vkDestroyFence(device, fence, NULL);
    vkDestroyCommandPool(device, pool, NULL);
    venus_render_destroy(device);
    graphics_log("PASS: Mesa runtime shader rendering and presentation\n");
}

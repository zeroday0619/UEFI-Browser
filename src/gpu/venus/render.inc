#include "venus-shaders.h"
#define VENUS_RENDER_WIDTH 128u
#define VENUS_RENDER_BYTES (VENUS_RENDER_WIDTH*VENUS_RENDER_WIDTH*4u)
#define VENUS_READBACK_OFFSET 65536u
static struct {
  struct VenusHandle objects[11];
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
  const VkCommandFlagsEXT reply=VK_COMMAND_GENERATE_REPLY_BIT_EXT;
  struct vn_cs_encoder encoder; struct vn_cs_decoder decoder;
  memset(&render_state,0,sizeof(render_state));
#define RENDER_OBJECT(member,type,object_type,index) \
  render_state.objects[index]=(struct VenusHandle){200+index,object_type}; \
  render_state.member=(type)&render_state.objects[index]
  RENDER_OBJECT(image,VkImage,VK_OBJECT_TYPE_IMAGE,0);
  RENDER_OBJECT(image_memory,VkDeviceMemory,VK_OBJECT_TYPE_DEVICE_MEMORY,1);
  RENDER_OBJECT(readback_memory,VkDeviceMemory,VK_OBJECT_TYPE_DEVICE_MEMORY,2);
  RENDER_OBJECT(view,VkImageView,VK_OBJECT_TYPE_IMAGE_VIEW,3);
  RENDER_OBJECT(buffer,VkBuffer,VK_OBJECT_TYPE_BUFFER,4);
  RENDER_OBJECT(render_pass,VkRenderPass,VK_OBJECT_TYPE_RENDER_PASS,5);
  RENDER_OBJECT(framebuffer,VkFramebuffer,VK_OBJECT_TYPE_FRAMEBUFFER,6);
  RENDER_OBJECT(layout,VkPipelineLayout,VK_OBJECT_TYPE_PIPELINE_LAYOUT,7);
  RENDER_OBJECT(pipeline,VkPipeline,VK_OBJECT_TYPE_PIPELINE,8);
  RENDER_OBJECT(vertex,VkShaderModule,VK_OBJECT_TYPE_SHADER_MODULE,9);
  RENDER_OBJECT(fragment,VkShaderModule,VK_OBJECT_TYPE_SHADER_MODULE,10);
#undef RENDER_OBJECT
  VkPhysicalDeviceMemoryProperties memory_properties={0};
  VENUS_VOID(vkGetPhysicalDeviceMemoryProperties,physical_device,&memory_properties);
  VkFormatProperties format_properties={0};
  VENUS_VOID(vkGetPhysicalDeviceFormatProperties,physical_device,VK_FORMAT_R8G8B8A8_UNORM,&format_properties);
  const VkFormatFeatureFlags required=VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT|VK_FORMAT_FEATURE_TRANSFER_SRC_BIT;
  if((format_properties.optimalTilingFeatures&required)!=required) venus_stream_failure("RGBA render target support");
  const VkImageCreateInfo image_info={.sType=VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,.imageType=VK_IMAGE_TYPE_2D,
    .format=VK_FORMAT_R8G8B8A8_UNORM,.extent={VENUS_RENDER_WIDTH,VENUS_RENDER_WIDTH,1},.mipLevels=1,.arrayLayers=1,
    .samples=VK_SAMPLE_COUNT_1_BIT,.tiling=VK_IMAGE_TILING_OPTIMAL,
    .usage=VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT|VK_IMAGE_USAGE_TRANSFER_SRC_BIT,.sharingMode=VK_SHARING_MODE_EXCLUSIVE};
  VENUS_RESULT(vkCreateImage,logical_device,&image_info,NULL,&render_state.image);
  VkMemoryRequirements image_requirements={0};
  VENUS_VOID(vkGetImageMemoryRequirements,logical_device,render_state.image,&image_requirements);
  if(!image_requirements.size || image_requirements.size>32*1024*1024) venus_stream_failure("render target allocation size");
  const VkMemoryDedicatedAllocateInfo image_dedicated={.sType=VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO,.image=render_state.image};
  const VkMemoryAllocateInfo image_allocate={.sType=VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,.pNext=&image_dedicated,
    .allocationSize=image_requirements.size,.memoryTypeIndex=venus_memory_type(&memory_properties,image_requirements.memoryTypeBits,VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)};
  VENUS_RESULT(vkAllocateMemory,logical_device,&image_allocate,NULL,&render_state.image_memory);
  VENUS_RESULT(vkBindImageMemory,logical_device,render_state.image,render_state.image_memory,0);
  const VkImageViewCreateInfo view_info={.sType=VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,.image=render_state.image,
    .viewType=VK_IMAGE_VIEW_TYPE_2D,.format=VK_FORMAT_R8G8B8A8_UNORM,.subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1}};
  VENUS_RESULT(vkCreateImageView,logical_device,&view_info,NULL,&render_state.view);
  const VkBufferCreateInfo buffer_info={.sType=VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,.size=VENUS_RENDER_BYTES,
    .usage=VK_BUFFER_USAGE_TRANSFER_DST_BIT,.sharingMode=VK_SHARING_MODE_EXCLUSIVE};
  VENUS_RESULT(vkCreateBuffer,logical_device,&buffer_info,NULL,&render_state.buffer);
  VkMemoryRequirements buffer_requirements={0};
  VENUS_VOID(vkGetBufferMemoryRequirements,logical_device,render_state.buffer,&buffer_requirements);
  if(buffer_requirements.size<VENUS_RENDER_BYTES || buffer_requirements.size>32*1024*1024)
    venus_stream_failure("readback allocation size");
  render_state.readback_allocation=(buffer_requirements.size+4095)&~4095ull;
  if(host_visible_size<VENUS_READBACK_OFFSET || render_state.readback_allocation>host_visible_size-VENUS_READBACK_OFFSET)
    venus_stream_failure("readback aperture size");
  const VkMemoryDedicatedAllocateInfo buffer_dedicated={.sType=VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO,.buffer=render_state.buffer};
  const VkMemoryAllocateInfo buffer_allocate={.sType=VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,.pNext=&buffer_dedicated,
    .allocationSize=render_state.readback_allocation,
    .memoryTypeIndex=venus_memory_type(&memory_properties,buffer_requirements.memoryTypeBits,VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT|VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)};
  VENUS_RESULT(vkAllocateMemory,logical_device,&buffer_allocate,NULL,&render_state.readback_memory);
  VENUS_RESULT(vkBindBufferMemory,logical_device,render_state.buffer,render_state.readback_memory,0);
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
  VENUS_RESULT(vkCreateRenderPass,logical_device,&pass_info,NULL,&render_state.render_pass);
  const VkFramebufferCreateInfo framebuffer_info={.sType=VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO,
    .renderPass=render_state.render_pass,.attachmentCount=1,.pAttachments=&render_state.view,
    .width=VENUS_RENDER_WIDTH,.height=VENUS_RENDER_WIDTH,.layers=1};
  VENUS_RESULT(vkCreateFramebuffer,logical_device,&framebuffer_info,NULL,&render_state.framebuffer);
  const VkShaderModuleCreateInfo vertex_info={.sType=VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
    .codeSize=sizeof(venus_vertex_shader),.pCode=venus_vertex_shader};
  const VkShaderModuleCreateInfo fragment_info={.sType=VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
    .codeSize=sizeof(venus_fragment_shader),.pCode=venus_fragment_shader};
  VENUS_RESULT(vkCreateShaderModule,logical_device,&vertex_info,NULL,&render_state.vertex);
  VENUS_RESULT(vkCreateShaderModule,logical_device,&fragment_info,NULL,&render_state.fragment);
  const VkPipelineLayoutCreateInfo layout_info={.sType=VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
  VENUS_RESULT(vkCreatePipelineLayout,logical_device,&layout_info,NULL,&render_state.layout);
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
  VENUS_RESULT(vkCreateGraphicsPipelines,logical_device,VK_NULL_HANDLE,1,&pipeline_info,NULL,&render_state.pipeline);
  text("PASS: Venus image, memory and graphics pipeline created\n");
}
static void venus_render_record(VkCommandBuffer command_buffer) {
  const VkCommandFlagsEXT reply=VK_COMMAND_GENERATE_REPLY_BIT_EXT;
  struct vn_cs_encoder encoder; struct vn_cs_decoder decoder;
  const VkClearValue clear={.color={.float32={16.0f/255.0f,32.0f/255.0f,48.0f/255.0f,1.0f}}};
  const VkRenderPassBeginInfo begin={.sType=VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,.renderPass=render_state.render_pass,
    .framebuffer=render_state.framebuffer,.renderArea={{0,0},{VENUS_RENDER_WIDTH,VENUS_RENDER_WIDTH}},.clearValueCount=1,.pClearValues=&clear};
  VENUS_VOID(vkCmdBeginRenderPass,command_buffer,&begin,VK_SUBPASS_CONTENTS_INLINE);
  VENUS_VOID(vkCmdBindPipeline,command_buffer,VK_PIPELINE_BIND_POINT_GRAPHICS,render_state.pipeline);
  VENUS_VOID(vkCmdDraw,command_buffer,3,1,0,0);
  VENUS_VOID(vkCmdEndRenderPass,command_buffer);
  const VkImageMemoryBarrier image_barrier={.sType=VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
    .srcAccessMask=VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,.dstAccessMask=VK_ACCESS_TRANSFER_READ_BIT,
    .oldLayout=VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,.newLayout=VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
    .srcQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED,.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED,
    .image=render_state.image,.subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1}};
  VENUS_VOID(vkCmdPipelineBarrier,command_buffer,VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT,0,0,NULL,0,NULL,1,&image_barrier);
  const VkBufferImageCopy region={.imageSubresource={VK_IMAGE_ASPECT_COLOR_BIT,0,0,1},.imageExtent={VENUS_RENDER_WIDTH,VENUS_RENDER_WIDTH,1}};
  VENUS_VOID(vkCmdCopyImageToBuffer,command_buffer,render_state.image,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,render_state.buffer,1,&region);
  const VkBufferMemoryBarrier buffer_barrier={.sType=VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER,
    .srcAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT,.dstAccessMask=VK_ACCESS_HOST_READ_BIT,
    .srcQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED,.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED,
    .buffer=render_state.buffer,.size=VENUS_RENDER_BYTES};
  VENUS_VOID(vkCmdPipelineBarrier,command_buffer,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_HOST_BIT,0,0,NULL,1,&buffer_barrier,0,NULL);
}
static void venus_render_readback(void) {
  struct ExportBlob { struct Command header; uint32_t flags,entries; uint64_t identifier,size; };
  const struct ExportBlob blob={.header={.type=0x10c,.context=1,.first=0x102,.second=2},
    .flags=1,.identifier=render_state.objects[2].identifier,.size=render_state.readback_allocation};
  if(submit(&blob,sizeof(blob),0x1100)!=24) venus_stream_failure("GPU memory export");
  const struct Command attach={.type=0x202,.context=1,.first=0x102};
  if(submit(&attach,sizeof(attach),0x1100)!=24) venus_stream_failure("readback blob attachment");
  struct ReadbackMap { struct Command header; uint64_t offset; };
  const struct ReadbackMap map={.header={.type=0x208,.first=0x102},.offset=VENUS_READBACK_OFFSET};
  if(submit(&map,sizeof(map),0x1106)!=32) venus_stream_failure("GPU memory mapping");
  memcpy(render_pixels,(const void*)(uintptr_t)(host_visible_base+VENUS_READBACK_OFFSET),sizeof(render_pixels));
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
  number("VENUS: triangle_interior_pixels=",interior); number("VENUS: background_pixels=",background);
  number("VENUS: rgba_fnv1a=",checksum);
  text("PASS: Venus GPU triangle pixels validated\n");
  venus_present(render_pixels,VENUS_RENDER_WIDTH);
  const struct Command unmap={.type=0x209,.first=0x102};
  if(submit(&unmap,sizeof(unmap),0x1100)!=24) venus_stream_failure("readback blob unmap");
  const struct Command detach={.type=0x203,.context=1,.first=0x102};
  if(submit(&detach,sizeof(detach),0x1100)!=24) venus_stream_failure("readback blob detach");
  const struct Command release={.type=0x102,.first=0x102};
  if(submit(&release,sizeof(release),0x1100)!=24) venus_stream_failure("readback blob release");
}
static void venus_render_destroy(VkDevice logical_device) {
  const VkCommandFlagsEXT reply=VK_COMMAND_GENERATE_REPLY_BIT_EXT;
  struct vn_cs_encoder encoder; struct vn_cs_decoder decoder;
  VENUS_VOID(vkDestroyPipeline,logical_device,render_state.pipeline,NULL);
  VENUS_VOID(vkDestroyPipelineLayout,logical_device,render_state.layout,NULL);
  VENUS_VOID(vkDestroyShaderModule,logical_device,render_state.fragment,NULL);
  VENUS_VOID(vkDestroyShaderModule,logical_device,render_state.vertex,NULL);
  VENUS_VOID(vkDestroyFramebuffer,logical_device,render_state.framebuffer,NULL);
  VENUS_VOID(vkDestroyRenderPass,logical_device,render_state.render_pass,NULL);
  VENUS_VOID(vkDestroyImageView,logical_device,render_state.view,NULL);
  VENUS_VOID(vkDestroyImage,logical_device,render_state.image,NULL);
  VENUS_VOID(vkDestroyBuffer,logical_device,render_state.buffer,NULL);
  VENUS_VOID(vkFreeMemory,logical_device,render_state.readback_memory,NULL);
  VENUS_VOID(vkFreeMemory,logical_device,render_state.image_memory,NULL);
  text("PASS: Venus render resources destroyed\n");
}

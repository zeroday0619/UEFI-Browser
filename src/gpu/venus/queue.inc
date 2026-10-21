/* Every synchronous command waits for its context's CPU timeline before decoding. */
#define VENUS_RESULT(function, ...) do { \
  encoder=venus_begin(); \
  vn_encode_##function(&encoder,reply,__VA_ARGS__); \
  decoder=venus_execute(&encoder,vn_sizeof_##function##_reply(__VA_ARGS__)); \
  VkResult result=vn_decode_##function##_reply(&decoder,__VA_ARGS__); \
  if(result!=VK_SUCCESS) { number("VENUS: result=",(uint32_t)result); venus_stream_failure(#function); } \
} while(0)
#define VENUS_VOID(function, ...) do { \
  encoder=venus_begin(); \
  vn_encode_##function(&encoder,reply,__VA_ARGS__); \
  decoder=venus_execute(&encoder,vn_sizeof_##function##_reply(__VA_ARGS__)); \
  vn_decode_##function##_reply(&decoder,__VA_ARGS__); \
} while(0)
#ifdef ENABLE_VENUS_RENDER
#include "render.inc"
#endif
static void venus_queue_commands(VkPhysicalDevice physical_device) {
  const VkCommandFlagsEXT reply=VK_COMMAND_GENERATE_REPLY_BIT_EXT;
  struct vn_cs_encoder encoder;
  struct vn_cs_decoder decoder;
  uint32_t family_count=16;
  VkQueueFamilyProperties families[16]={0};
  encoder=venus_begin();
  vn_encode_vkGetPhysicalDeviceQueueFamilyProperties(&encoder,reply,physical_device,&family_count,families);
  decoder=venus_execute(&encoder,vn_sizeof_vkGetPhysicalDeviceQueueFamilyProperties_reply(physical_device,&family_count,families));
  venus_validate_count_reply(&decoder,false,16);
  vn_decode_vkGetPhysicalDeviceQueueFamilyProperties_reply(&decoder,physical_device,&family_count,families);
  uint32_t family_index=0;
  while(family_index<family_count && (!families[family_index].queueCount ||
       !(families[family_index].queueFlags&VK_QUEUE_GRAPHICS_BIT) ||
       !families[family_index].timestampValidBits)) ++family_index;
  if(family_index==family_count) venus_stream_failure("timestamp-capable graphics queue");
  number("VENUS: graphics_queue_family=",family_index);
  float priority=1.0f;
  const VkDeviceQueueCreateInfo queue_create={.sType=VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
    .queueFamilyIndex=family_index,.queueCount=1,.pQueuePriorities=&priority};
  const VkDeviceCreateInfo device_create={.sType=VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
    .queueCreateInfoCount=1,.pQueueCreateInfos=&queue_create};
  struct VenusHandle device_object={100,VK_OBJECT_TYPE_DEVICE};
  VkDevice logical_device=(VkDevice)&device_object;
  VENUS_RESULT(vkCreateDevice,physical_device,&device_create,NULL,&logical_device);
  struct VenusHandle queue_object={101,VK_OBJECT_TYPE_QUEUE};
  VkQueue queue=(VkQueue)&queue_object;
  const VkDeviceQueueTimelineInfoMESA timeline={.sType=VK_STRUCTURE_TYPE_DEVICE_QUEUE_TIMELINE_INFO_MESA,.ringIdx=1};
  const VkDeviceQueueInfo2 queue_info={.sType=VK_STRUCTURE_TYPE_DEVICE_QUEUE_INFO_2,.pNext=&timeline,
    .queueFamilyIndex=family_index,.queueIndex=0};
  VENUS_VOID(vkGetDeviceQueue2,logical_device,&queue_info,&queue);
  const VkQueryPoolCreateInfo query_create={.sType=VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO,
    .queryType=VK_QUERY_TYPE_TIMESTAMP,.queryCount=2};
  struct VenusHandle query_object={102,VK_OBJECT_TYPE_QUERY_POOL};
  VkQueryPool query=(VkQueryPool)&query_object;
  VENUS_RESULT(vkCreateQueryPool,logical_device,&query_create,NULL,&query);
  const VkCommandPoolCreateInfo pool_create={.sType=VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
    .queueFamilyIndex=family_index};
  struct VenusHandle pool_object={103,VK_OBJECT_TYPE_COMMAND_POOL};
  VkCommandPool pool=(VkCommandPool)&pool_object;
  VENUS_RESULT(vkCreateCommandPool,logical_device,&pool_create,NULL,&pool);
  const VkCommandBufferAllocateInfo allocate={.sType=VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
    .commandPool=pool,.level=VK_COMMAND_BUFFER_LEVEL_PRIMARY,.commandBufferCount=1};
  struct VenusHandle command_object={104,VK_OBJECT_TYPE_COMMAND_BUFFER};
  VkCommandBuffer command_buffer=(VkCommandBuffer)&command_object;
  VENUS_RESULT(vkAllocateCommandBuffers,logical_device,&allocate,&command_buffer);
#ifdef ENABLE_VENUS_RENDER
  venus_render_create(physical_device,logical_device);
#endif
  const VkCommandBufferBeginInfo begin={.sType=VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
    .flags=VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT};
  VENUS_RESULT(vkBeginCommandBuffer,command_buffer,&begin);
  VENUS_VOID(vkCmdResetQueryPool,command_buffer,query,0,2);
  VENUS_VOID(vkCmdWriteTimestamp,command_buffer,VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,query,0);
#ifdef ENABLE_VENUS_RENDER
  venus_render_record(command_buffer);
#endif
  VENUS_VOID(vkCmdWriteTimestamp,command_buffer,VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,query,1);
  VENUS_RESULT(vkEndCommandBuffer,command_buffer);
  const VkSubmitInfo submit_info={.sType=VK_STRUCTURE_TYPE_SUBMIT_INFO,
    .commandBufferCount=1,.pCommandBuffers=&command_buffer};
  encoder=venus_begin();
  vn_encode_vkQueueSubmit(&encoder,reply,queue,1,&submit_info,VK_NULL_HANDLE);
  /* Timeline one fences GPU completion; timeline zero only fences command decoding. */
  venus_request.header.padding=1;
  decoder=venus_execute(&encoder,vn_sizeof_vkQueueSubmit_reply(queue,1,&submit_info,VK_NULL_HANDLE));
  if(vn_decode_vkQueueSubmit_reply(&decoder,queue,1,&submit_info,VK_NULL_HANDLE)!=VK_SUCCESS)
    venus_stream_failure("GPU queue submission");
  uint64_t timestamps[2]={0};
  VENUS_RESULT(vkGetQueryPoolResults,logical_device,query,0,2,sizeof(timestamps),timestamps,
    sizeof(uint64_t),VK_QUERY_RESULT_64_BIT);
  if(!timestamps[0] || timestamps[1]<=timestamps[0]) venus_stream_failure("GPU timestamp results");
  number("VENUS: gpu_timestamp_begin=",timestamps[0]); number("VENUS: gpu_timestamp_end=",timestamps[1]);
#ifdef ENABLE_VENUS_RENDER
  venus_render_readback();
  venus_render_destroy(logical_device);
#endif
  VENUS_VOID(vkFreeCommandBuffers,logical_device,pool,1,&command_buffer);
  VENUS_VOID(vkDestroyCommandPool,logical_device,pool,NULL);
  VENUS_VOID(vkDestroyQueryPool,logical_device,query,NULL);
  VENUS_VOID(vkDestroyDevice,logical_device,NULL);
  text("PASS: Venus graphics queue executed GPU timestamp commands\n");
}
#undef VENUS_VOID
#undef VENUS_RESULT

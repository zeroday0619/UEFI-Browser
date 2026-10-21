#include "vn_protocol_driver.h"
#include "venus_hw.h"
static struct virgl_renderer_capset_venus venus_capabilities;
bool vn_cs_renderer_protocol_has_api_version(uint32_t version) { return version<=venus_capabilities.vk_xml_version; }
bool vn_cs_renderer_protocol_has_extension(uint32_t extension) {
  return extension<1024 && (!(venus_capabilities.vk_extension_mask1[0]&1) ||
    (venus_capabilities.vk_extension_mask1[extension/32]&(1u<<(extension%32))));
}
static void venus_check_capabilities(const void *data, size_t size) {
  if(size<sizeof(venus_capabilities)) venus_stream_failure("Venus capability size");
  memcpy(&venus_capabilities,data,sizeof(venus_capabilities));
  if(venus_capabilities.wire_format_version!=vn_info_wire_format_version() ||
     venus_capabilities.vk_xml_version<VK_API_VERSION_1_1 || !venus_capabilities.supports_blob_id_0 ||
     !venus_capabilities.supports_multiple_timelines || !venus_capabilities.allow_vk_wait_syncs)
    venus_stream_failure("Venus protocol capabilities");
}

_Noreturn void venus_stream_failure(const char *message) {
  text("VENUS: "); text(message); text("\n"); finish("FAIL: Venus command stream");
}
void platform_assert(const char *condition, const char *file, int line) {
  text(file); number(" line=", (unsigned)line); venus_stream_failure(condition);
}
size_t strlen(const char *string) { size_t size=0; while(string[size]) ++size; return size; }
static struct { struct Command header; uint8_t data[4064]; } venus_request;
static uint8_t venus_reply[4096];
static struct vn_cs_encoder venus_begin(void) {
  memset(&venus_request,0,sizeof(venus_request));
  memset((void*)(uintptr_t)host_visible_base,0xa5,4096);
  venus_request.header.type=0x207;
  venus_request.header.context=1;
  struct vn_cs_encoder encoder={venus_request.data,sizeof(venus_request.data),0};
  const VkCommandStreamDescriptionMESA stream={.resourceId=0x101,.offset=0,.size=4096};
  vn_encode_vkSetReplyCommandStreamMESA(&encoder,0,&stream);
  return encoder;
}
static struct vn_cs_decoder venus_execute(struct vn_cs_encoder *encoder, size_t reply_size) {
  if (reply_size>sizeof(venus_reply)) venus_stream_failure("reply capacity");
  venus_request.header.first=(uint32_t)encoder->length;
  if(submit(&venus_request,32+(uint32_t)encoder->length,0x1100)!=24)
    venus_stream_failure("SUBMIT_3D completion");
  /* The fence precedes a private snapshot so decoding never follows changing DMA bytes. */
  memcpy(venus_reply,(const void*)(uintptr_t)host_visible_base,reply_size);
  return (struct vn_cs_decoder){venus_reply,reply_size,0};
}
#include "queue.inc"
static void venus_commands(void) {
  if (!host_visible_base || host_visible_size<4096) venus_stream_failure("host-visible PCI region");
  struct HostBlob { struct Command header; uint32_t flags, entries; uint64_t identifier, size; };
  _Static_assert(sizeof(struct HostBlob)==56,"host blob layout");
  const struct HostBlob blob={.header={.type=0x10c,.context=1,.first=0x101,.second=2},.flags=1,.size=4096};
  if(submit(&blob,sizeof(blob),0x1100)!=24) venus_stream_failure("host shared blob creation");
  const struct Command attach={.type=0x202,.context=1,.first=0x101};
  if(submit(&attach,sizeof(attach),0x1100)!=24) venus_stream_failure("host blob attachment");
  struct MapBlob { struct Command header; uint64_t offset; };
  const struct MapBlob map={.header={.type=0x208,.first=0x101}};
  if(submit(&map,sizeof(map),0x1106)!=32) venus_stream_failure("host blob mapping");
  number("VENUS: host_visible_base=",host_visible_base);
  text("PASS: Venus host shared blob mapped\n");
  const VkCommandFlagsEXT reply=VK_COMMAND_GENERATE_REPLY_BIT_EXT;
  uint32_t version=0;
  struct vn_cs_encoder encoder=venus_begin();
  vn_encode_vkEnumerateInstanceVersion(&encoder,reply,&version);
  struct vn_cs_decoder decoder=venus_execute(&encoder,vn_sizeof_vkEnumerateInstanceVersion_reply(&version));
  if(vn_decode_vkEnumerateInstanceVersion_reply(&decoder,&version)!=VK_SUCCESS || version<VK_API_VERSION_1_1)
    venus_stream_failure("Vulkan instance version");
  number("VENUS: host_api_version=",version);
  struct VenusHandle instance_object={1,VK_OBJECT_TYPE_INSTANCE};
  VkInstance instance=(VkInstance)&instance_object;
  const VkApplicationInfo application={.sType=VK_STRUCTURE_TYPE_APPLICATION_INFO,.pApplicationName="UEFI BROWSER",.apiVersion=VK_API_VERSION_1_1};
  const VkInstanceCreateInfo create={.sType=VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,.pApplicationInfo=&application};
  encoder=venus_begin();
  vn_encode_vkCreateInstance(&encoder,reply,&create,NULL,&instance);
  decoder=venus_execute(&encoder,vn_sizeof_vkCreateInstance_reply(&create,NULL,&instance));
  if(vn_decode_vkCreateInstance_reply(&decoder,&create,NULL,&instance)!=VK_SUCCESS || instance_object.identifier!=1)
    venus_stream_failure("Vulkan instance creation");
  text("PASS: Venus Vulkan instance created\n");
  struct VenusHandle device_objects[8]; VkPhysicalDevice physical_devices[8];
  for(unsigned index=0;index<8;++index) {
    device_objects[index]=(struct VenusHandle){2+index,VK_OBJECT_TYPE_PHYSICAL_DEVICE};
    physical_devices[index]=(VkPhysicalDevice)&device_objects[index];
  }
  uint32_t count=8;
  encoder=venus_begin();
  vn_encode_vkEnumeratePhysicalDevices(&encoder,reply,instance,&count,physical_devices);
  decoder=venus_execute(&encoder,vn_sizeof_vkEnumeratePhysicalDevices_reply(instance,&count,physical_devices));
  /* The generated decoder uses the returned count as its array limit. Validate it first. */
  venus_validate_count_reply(&decoder,true,8);
  if(vn_decode_vkEnumeratePhysicalDevices_reply(&decoder,instance,&count,physical_devices)!=VK_SUCCESS || !count)
    venus_stream_failure("physical device enumeration");
  number("VENUS: physical_device_count=",count);
  for(uint32_t index=0;index<count;++index) {
    VkPhysicalDeviceProperties properties={0};
    encoder=venus_begin();
    vn_encode_vkGetPhysicalDeviceProperties(&encoder,reply,physical_devices[index],&properties);
    decoder=venus_execute(&encoder,vn_sizeof_vkGetPhysicalDeviceProperties_reply(physical_devices[index],&properties));
    vn_decode_vkGetPhysicalDeviceProperties_reply(&decoder,physical_devices[index],&properties);
    properties.deviceName[VK_MAX_PHYSICAL_DEVICE_NAME_SIZE-1]=0;
    text("VENUS: physical_device_name="); text(properties.deviceName); text("\n");
    number("VENUS: vendor_id=",properties.vendorID); number("VENUS: device_id=",properties.deviceID);
    number("VENUS: device_type=",properties.deviceType);
    if(!properties.deviceName[0] || !properties.vendorID) venus_stream_failure("physical device properties");
  }
  venus_queue_commands(physical_devices[0]);
  encoder=venus_begin(); vn_encode_vkDestroyInstance(&encoder,reply,instance,NULL);
  decoder=venus_execute(&encoder,vn_sizeof_vkDestroyInstance_reply(instance,NULL));
  vn_decode_vkDestroyInstance_reply(&decoder,instance,NULL);
  const struct Command unmap={.type=0x209,.first=0x101};
  if(submit(&unmap,sizeof(unmap),0x1100)!=24) venus_stream_failure("host blob unmap");
  const struct Command detach={.type=0x203,.context=1,.first=0x101};
  if(submit(&detach,sizeof(detach),0x1100)!=24) venus_stream_failure("host blob detach");
  const struct Command release={.type=0x102,.first=0x101};
  if(submit(&release,sizeof(release),0x1100)!=24) venus_stream_failure("host blob release");
  text("PASS: Venus Vulkan physical devices queried and instance destroyed\n");
}

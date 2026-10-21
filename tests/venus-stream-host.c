#include <assert.h>
#include <setjmp.h>
#include <stdio.h>
#include "../src/gpu/venus/vn_cs.h"
static jmp_buf failure;
static unsigned rejected;
_Noreturn void venus_stream_failure(const char *message) {
  assert(message && *message); ++rejected; longjmp(failure,1);
}
#define REJECT(expression) do { if(!setjmp(failure)) { expression; assert(!"expected rejection"); } } while(0)
int main(void) {
  uint8_t bytes[32]; memset(bytes,0xff,sizeof(bytes));
  struct vn_cs_encoder encoder={bytes,16,0};
  const uint32_t input=0x12345678;
  vn_cs_encoder_write(&encoder,8,&input,4);
  assert(encoder.length==8 && bytes[4]==0 && bytes[7]==0 && bytes[8]==255);
  struct vn_cs_decoder decoder={bytes,8,0}; uint32_t output=0;
  vn_cs_decoder_read(&decoder,8,&output,4); assert(output==input && decoder.offset==8);
  REJECT(vn_cs_encoder_reserve(&encoder,SIZE_MAX));
  REJECT(vn_cs_encoder_write(&encoder,4,&input,8));
  REJECT(vn_cs_encoder_write(&encoder,4,NULL,4));
  REJECT(vn_cs_decoder_read(&decoder,1,&output,1));
  REJECT(vn_cs_decoder_read(&decoder,SIZE_MAX,&output,4));
  struct vn_cs_decoder short_reply={bytes,4,0};
  REJECT(venus_validate_count_reply(&short_reply,true,8));
  encoder=(struct vn_cs_encoder){bytes,sizeof(bytes),0};
  uint32_t zero=0,count=8; uint64_t present=1,array_count=8;
  vn_cs_encoder_write(&encoder,4,&zero,4); vn_cs_encoder_write(&encoder,4,&zero,4);
  vn_cs_encoder_write(&encoder,8,&present,8); vn_cs_encoder_write(&encoder,4,&count,4);
  vn_cs_encoder_write(&encoder,8,&array_count,8);
  struct vn_cs_decoder valid={bytes,encoder.length,0};
  venus_validate_count_reply(&valid,true,8); assert(valid.offset==0);
  REJECT(venus_validate_count_reply(&valid,true,7));
  count=UINT32_MAX; memcpy(bytes+16,&count,4);
  REJECT(venus_validate_count_reply(&valid,true,8));
  count=8; memcpy(bytes+16,&count,4); array_count=UINT64_MAX; memcpy(bytes+20,&array_count,8);
  REJECT(venus_validate_count_reply(&valid,true,8));
  array_count=8; memcpy(bytes+20,&array_count,8); present=0; memcpy(bytes+8,&present,8);
  REJECT(venus_validate_count_reply(&valid,true,8));
  struct VenusHandle object={99,VK_OBJECT_TYPE_DEVICE}; void *handle=&object;
  assert(vn_cs_handle_load_id((const void **)&handle,VK_OBJECT_TYPE_DEVICE)==99);
  vn_cs_handle_store_id(&handle,100,VK_OBJECT_TYPE_DEVICE); assert(object.identifier==100);
  REJECT(vn_cs_handle_load_id((const void **)&handle,VK_OBJECT_TYPE_INSTANCE));
  REJECT(vn_cs_handle_store_id(&handle,100,VK_OBJECT_TYPE_INSTANCE));
  void *null_handle=NULL;
  assert(vn_cs_handle_load_id((const void **)&null_handle,VK_OBJECT_TYPE_DEVICE)==0);
  REJECT(vn_cs_handle_store_id(&null_handle,1,VK_OBJECT_TYPE_DEVICE));
  assert(rejected==13);
  printf("PASS: Venus stream and host reply boundaries (%u rejected cases)\n",rejected);
}

#ifndef UEFI_VENUS_COMMAND_STREAM_H
#define UEFI_VENUS_COMMAND_STREAM_H
#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <vulkan/vulkan.h>

_Noreturn void venus_stream_failure(const char *message);
struct vn_cs_encoder { uint8_t *data; size_t capacity, length; };
struct vn_cs_decoder { const uint8_t *data; size_t capacity, offset; };
bool vn_cs_renderer_protocol_has_api_version(uint32_t version);
bool vn_cs_renderer_protocol_has_extension(uint32_t extension);
struct VenusHandle { uint64_t identifier; VkObjectType type; };
static inline size_t vn_cs_encoder_get_len(const struct vn_cs_encoder *encoder) { return encoder->length; }
static inline bool vn_cs_encoder_reserve(struct vn_cs_encoder *encoder, size_t size) {
  if (encoder->length > encoder->capacity || size > encoder->capacity - encoder->length)
    venus_stream_failure("command stream capacity");
  return true;
}
static inline void vn_cs_encoder_write(struct vn_cs_encoder *encoder, size_t size, const void *data, size_t data_size) {
  vn_cs_encoder_reserve(encoder, size);
  if (data_size > size || (data_size && !data)) venus_stream_failure("command stream input");
  memset(encoder->data + encoder->length, 0, size);
  if (data_size) memcpy(encoder->data + encoder->length, data, data_size);
  encoder->length += size;
}
static inline void vn_cs_decoder_set_fatal(const struct vn_cs_decoder *decoder) {
  (void)decoder; venus_stream_failure("invalid host reply");
}
static inline void vn_cs_decoder_peek(const struct vn_cs_decoder *decoder, size_t size, void *data, size_t data_size) {
  if (decoder->offset > decoder->capacity || size > decoder->capacity - decoder->offset ||
      data_size > size || (data_size && !data)) vn_cs_decoder_set_fatal(decoder);
  if (data_size) memcpy(data, decoder->data + decoder->offset, data_size);
}
static inline void vn_cs_decoder_read(struct vn_cs_decoder *decoder, size_t size, void *data, size_t data_size) {
  vn_cs_decoder_peek(decoder, size, data, data_size); decoder->offset += size;
}
/* Output counts must be bounded before generated decoders use them as array limits. */
static inline void venus_validate_count_reply(const struct vn_cs_decoder *decoder, bool has_result, uint32_t capacity) {
  struct vn_cs_decoder cursor=*decoder;
  uint32_t command, result, count; uint64_t present, array_count;
  vn_cs_decoder_read(&cursor,4,&command,4);
  if(has_result) vn_cs_decoder_read(&cursor,4,&result,4);
  vn_cs_decoder_read(&cursor,8,&present,8);
  vn_cs_decoder_read(&cursor,4,&count,4);
  vn_cs_decoder_read(&cursor,8,&array_count,8);
  if(present!=1 || count>capacity || array_count!=count)
    venus_stream_failure("host output count");
}
static inline uint64_t vn_cs_handle_load_id(const void **handle, VkObjectType type) {
  const struct VenusHandle *object = *handle;
  if (!object) return 0;
  if (object->type != type) venus_stream_failure("handle type mismatch");
  return object->identifier;
}
static inline void vn_cs_handle_store_id(void **handle, uint64_t identifier, VkObjectType type) {
  struct VenusHandle *object = *handle;
  if (!object || object->type != type) venus_stream_failure("reply handle type mismatch");
  object->identifier = identifier;
}
#endif

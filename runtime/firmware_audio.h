#ifndef UEFI_FIRMWARE_AUDIO_H
#define UEFI_FIRMWARE_AUDIO_H
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
#define RENDERER_AUDIO_REQUEST 131
#define FIRMWARE_AUDIO_LIMIT (32u * 131068u)
#define FIRMWARE_AUDIO_STREAM_SLOTS 128u
enum {
  AUDIO_CREATE = 1,
  AUDIO_UPLOAD,
  AUDIO_START,
  AUDIO_STOP,
  AUDIO_POSITION,
  AUDIO_VOLUME,
  AUDIO_CLOSE,
  AUDIO_STREAM_CREATE,
  AUDIO_STREAM_WRITE,
  AUDIO_STREAM_RESET
};
long firmware_audio_request(unsigned operation,
                            long handle,
                            uint64_t first,
                            uint64_t second,
                            uint64_t third);
long firmware_audio_service(unsigned owner,
                            unsigned operation,
                            long handle,
                            uint64_t first,
                            uint64_t second,
                            uint64_t third);
long firmware_audio_stream_submitted(unsigned owner, long handle);
void firmware_audio_reset(unsigned owner);
void firmware_audio_poll(void);
#ifdef __cplusplus
}
#endif
#endif

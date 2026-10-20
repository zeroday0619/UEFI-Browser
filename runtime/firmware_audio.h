#ifndef UEFI_FIRMWARE_AUDIO_H
#define UEFI_FIRMWARE_AUDIO_H
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
#define RENDERER_AUDIO_REQUEST 131
#define FIRMWARE_AUDIO_LIMIT (32u * 131068u)
enum {
  AUDIO_CREATE = 1,
  AUDIO_UPLOAD,
  AUDIO_START,
  AUDIO_STOP,
  AUDIO_POSITION,
  AUDIO_VOLUME,
  AUDIO_CLOSE
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
void firmware_audio_reset(unsigned owner);
#ifdef __cplusplus
}
#endif
#endif

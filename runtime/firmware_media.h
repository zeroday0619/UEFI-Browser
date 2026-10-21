#ifndef UEFI_FIRMWARE_MEDIA_H
#define UEFI_FIRMWARE_MEDIA_H
#include "renderer_runtime.h"
#ifdef __cplusplus
extern "C" {
#endif
#define RENDERER_MEDIA_REQUEST 132
#define FIRMWARE_OPUS_PACKET_LIMIT 65536u
#define FIRMWARE_OPUS_FRAME_LIMIT 5760u
enum { FIRMWARE_OPUS_CREATE = 10, FIRMWARE_OPUS_DECODE, FIRMWARE_OPUS_RESET,
       FIRMWARE_OPUS_CLOSE };
typedef struct {
  uint32_t operation, channels, streams, coupled, bytes, phase_disabled;
  int32_t gain;
  uint32_t mapping[2];
  uint64_t handle;
} FirmwareOpusCommand;
enum { FIRMWARE_OPUS_QUEUE_LIMIT = 512, FIRMWARE_OPUS_QUEUE_BYTES = 4 * 1024 * 1024 };
typedef struct {
  int64_t timestamp, duration, discard_front, discard_back;
  uint32_t bytes, padding, discard_all, end;
} FirmwareOpusPacket;
typedef struct {
  uint64_t submitted, queued, bytes, decoded, pending_frames;
  int32_t error;
  uint32_t ended;
} FirmwareOpusStreamStatus;
long firmware_opus_stream_attach(long handle, long audio, unsigned discard, int64_t origin);
long firmware_opus_stream_enqueue(long handle, const FirmwareOpusPacket *packet, const unsigned char *data);
long firmware_opus_stream_status(long handle, FirmwareOpusStreamStatus *status);
void *firmware_audio_timeline_create(unsigned channels, unsigned discard, int64_t origin);
void firmware_audio_timeline_destroy(void *context);
int firmware_audio_timeline_process(void *context, const FirmwareOpusPacket *packet,
    const float *input, unsigned frames, int16_t *output, int64_t *first);
long firmware_opus_create(unsigned channels, unsigned streams, unsigned coupled,
                         const unsigned char *mapping, int gain, unsigned phase_disabled);
long firmware_opus_decode(long handle, const unsigned char *packet, size_t bytes,
                         float *output, size_t samples);
long firmware_opus_reset(long handle);
void firmware_opus_close(long handle);
long firmware_media_broker(const uint64_t arguments[5]);
void firmware_media_reset(void);
void firmware_media_poll(void);
void firmware_media_shutdown(void);
int firmware_media_probe(void);
void firmware_media_child(RendererBootstrap *bootstrap);
#ifdef __cplusplus
}
#endif
#endif

#include "firmware_media.h"
#include "media_probe_packet.h"
static int run_media_opus_probe(unsigned *total) {
  unsigned checks = 0;
  const long available = (long)renderer_runtime_request(RENDERER_MEDIA_REQUEST, 0, 0, 0, 0, 0);
  if (available == -ENOSYS) return 0;
  VERIFY(available == -EINVAL);
  VERIFY((long)renderer_runtime_request(RENDERER_MEDIA_REQUEST, 1, 1, 0, 0, 0) == -EFAULT);
  VERIFY((long)renderer_runtime_request(RENDERER_MEDIA_REQUEST, (1ULL << 32) | 1, 1, 0, 0, 0) == -EINVAL);
  VERIFY((long)renderer_runtime_request(RENDERER_MEDIA_REQUEST, 2, 1, 0, 0, 0) == -ESTALE);
  unsigned char mapping[2] = {0, 1};
  VERIFY(firmware_opus_create(2, 1, 1, mapping, 32768, 0) == -EINVAL);
  VERIFY(firmware_opus_create(2, 1, 1, mapping, 0, 2) == -EINVAL);
  mapping[1] = 2;
  VERIFY(firmware_opus_create(2, 1, 1, mapping, 0, 0) == -EINVAL);
  mapping[1] = 1;
  long handles[8];
  for (unsigned index = 0; index < 8; ++index) {
    handles[index] = firmware_opus_create(2, 1, 1, mapping, 0, 0);
    VERIFY(handles[index] > 0 && (!index || handles[index] > handles[index - 1]));
  }
  VERIFY(firmware_opus_create(2, 1, 1, mapping, 0, 0) == -ENOSPC);
  unsigned char packet[sizeof(media_probe_packet)];
  memcpy(packet, media_probe_packet, sizeof(packet));
  FirmwareOpusCommand command = {.operation = FIRMWARE_OPUS_DECODE,
      .handle = handles[0], .bytes = sizeof(packet)};
  VERIFY((long)renderer_runtime_request(RENDERER_MEDIA_REQUEST, 1,
      (uintptr_t)&command, 1, 0, 0) == -EFAULT);
  command.bytes = FIRMWARE_OPUS_PACKET_LIMIT + 1;
  VERIFY((long)renderer_runtime_request(RENDERER_MEDIA_REQUEST, 1,
      (uintptr_t)&command, (uintptr_t)packet, 0, 0) == -EINVAL);
  command.bytes = sizeof(packet);
  long ticket = (long)renderer_runtime_request(RENDERER_MEDIA_REQUEST, 1,
      (uintptr_t)&command, (uintptr_t)packet, 0, 0);
  VERIFY(ticket > 0);
  float output[FIRMWARE_OPUS_FRAME_LIMIT * 2] = {0};
  long result;
  do {
    result = (long)renderer_runtime_request(RENDERER_MEDIA_REQUEST, 2, ticket,
        1, FIRMWARE_OPUS_FRAME_LIMIT * 2, 0);
  } while (result == -EAGAIN);
  VERIFY(result == -EFAULT);
  VERIFY((long)renderer_runtime_request(RENDERER_MEDIA_REQUEST, 2, ticket,
      (uintptr_t)output, 1, 0) == -EINVAL);
  VERIFY((long)renderer_runtime_request(RENDERER_MEDIA_REQUEST, 2, ticket,
      (uintptr_t)output, FIRMWARE_OPUS_FRAME_LIMIT * 2 + 1, 0) == -EINVAL);
  VERIFY((long)renderer_runtime_request(RENDERER_MEDIA_REQUEST, 2, ticket,
      (uintptr_t)output, FIRMWARE_OPUS_FRAME_LIMIT * 2, 0) == 960);
  double energy = 0;
  for (unsigned index = 0; index < 1920; ++index) energy += (double)output[index] * output[index];
  VERIFY(energy > 1 && energy < 1920);
  VERIFY((long)renderer_runtime_request(RENDERER_MEDIA_REQUEST, 2, ticket,
      (uintptr_t)output, FIRMWARE_OPUS_FRAME_LIMIT * 2, 0) == -ESTALE);
  VERIFY(!firmware_opus_reset(handles[0]));
  const unsigned char corrupt[] = {0xff};
  VERIFY(firmware_opus_decode(handles[0], corrupt, sizeof(corrupt), output,
      FIRMWARE_OPUS_FRAME_LIMIT * 2) < 0);
  VERIFY(!firmware_opus_reset(handles[0]));
  VERIFY(firmware_opus_decode(handles[0], packet, sizeof(packet), output,
      FIRMWARE_OPUS_FRAME_LIMIT * 2) == 960);
  firmware_opus_close(handles[0]);
  VERIFY(firmware_opus_reset(handles[0]) == -EBADF);
  long replacement = firmware_opus_create(1, 1, 0, mapping, -256, 1);
  VERIFY(replacement > handles[7]);
  VERIFY(firmware_opus_decode(replacement, packet, sizeof(packet), output,
      FIRMWARE_OPUS_FRAME_LIMIT) == 960);
  firmware_opus_close(replacement);
  for (unsigned index = 1; index < 8; ++index) firmware_opus_close(handles[index]);
  printf("MEDIA BROKER PROBE: passed checks=%u streams=8 stale_handles=blocked\n", checks);
  *total += checks;
  return 0;
}

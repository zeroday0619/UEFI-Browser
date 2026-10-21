/* The fixture fills a paused stream to exercise both independent queue quotas. */
extern int opus_packet_pad(unsigned char *data, int length, int new_length);
static int run_media_queue_probe(void) {
  if ((long)renderer_runtime_request(RENDERER_MEDIA_REQUEST, 0, 0, 0, 0, 0) == -ENOSYS) return 0;
  unsigned checks = 0;
  const unsigned char mapping[2] = {0, 1};
  long decoder = firmware_opus_create(2, 1, 1, mapping, 0, 0);
  VERIFY(decoder > 0);
  long audio = firmware_audio_request(AUDIO_STREAM_CREATE, 0, 0, 48000, 0);
  VERIFY(audio > 0);
  VERIFY(firmware_opus_stream_attach(decoder, audio, 65536, 0) == -EINVAL);
  VERIFY(firmware_opus_stream_attach(decoder, audio, 0, -1) == -EINVAL);
  long clip = firmware_audio_request(AUDIO_CREATE, 0, 64, 48000, 0);
  VERIFY(clip > 0);
  VERIFY(firmware_opus_stream_attach(decoder, clip, 0, 0) == -EBADF);
  VERIFY(!firmware_audio_request(AUDIO_CLOSE, clip, 0, 0, 0));
  VERIFY(!firmware_opus_stream_attach(decoder, audio, 0, 0));
  VERIFY(firmware_opus_stream_attach(decoder, audio, 0, 0) == -EEXIST);
  long second = firmware_opus_create(2, 1, 1, mapping, 0, 0);
  VERIFY(second > 0 && firmware_opus_stream_attach(second, audio, 0, 0) == -EBUSY);
  firmware_opus_close(second);
  VERIFY((long)renderer_runtime_request(RENDERER_MEDIA_REQUEST, 4, decoder, 1, 0, 0) == -EFAULT);
  VERIFY((long)renderer_runtime_request(RENDERER_MEDIA_REQUEST, 5, decoder, 1, 0, 0) == -EFAULT);
  FirmwareOpusPacket packet = {.timestamp = 0, .duration = 20000, .bytes = sizeof(media_probe_packet)};
  VERIFY((long)renderer_runtime_request(RENDERER_MEDIA_REQUEST, 4, decoder, (uintptr_t)&packet, 1, 0) == -EFAULT);
  packet.bytes = FIRMWARE_OPUS_PACKET_LIMIT + 1;
  VERIFY((long)renderer_runtime_request(RENDERER_MEDIA_REQUEST, 4, decoder, (uintptr_t)&packet, 0, 0) == -EINVAL);
  packet.bytes = 0; packet.timestamp = INT64_MIN;
  VERIFY(firmware_opus_stream_enqueue(decoder, &packet, NULL) == -EINVAL);
  packet.timestamp = 0; packet.discard_all = 1;
  VERIFY(firmware_opus_stream_enqueue(decoder, &packet, NULL) == -EINVAL);
  packet.padding = 1; packet.discard_back = 1;
  VERIFY(firmware_opus_stream_enqueue(decoder, &packet, NULL) == -EINVAL);
  packet.discard_all = packet.padding = 0; packet.discard_back = -1;
  VERIFY(firmware_opus_stream_enqueue(decoder, &packet, NULL) == -EINVAL);
  packet.discard_back = 0; packet.duration = 1200001;
  VERIFY(firmware_opus_stream_enqueue(decoder, &packet, NULL) == -EINVAL);
  packet.duration = 20000;
  unsigned char padded[FIRMWARE_OPUS_PACKET_LIMIT];
  memcpy(padded, media_probe_packet, sizeof(media_probe_packet));
  VERIFY(!opus_packet_pad(padded, sizeof(media_probe_packet), sizeof(padded)));
  for (unsigned phase = 0; phase < 2; ++phase) {
    packet.bytes = phase ? sizeof(padded) : sizeof(media_probe_packet);
    long result = 0;
    unsigned accepted = 0;
    while (accepted < 900) {
      packet.timestamp = (int64_t)accepted * 20000;
      result = firmware_opus_stream_enqueue(decoder, &packet, phase ? padded : media_probe_packet);
      if (result) break;
      ++accepted;
    }
    VERIFY(result == -EAGAIN);
    FirmwareOpusStreamStatus status;
    VERIFY(!firmware_opus_stream_status(decoder, &status) && !status.error);
    if (!phase) VERIFY(status.queued >= FIRMWARE_OPUS_QUEUE_LIMIT && status.queued <= FIRMWARE_OPUS_QUEUE_LIMIT + 1);
    else VERIFY(status.bytes >= FIRMWARE_OPUS_QUEUE_BYTES - sizeof(padded) && status.bytes <= FIRMWARE_OPUS_QUEUE_BYTES);
    firmware_opus_close(decoder);
    VERIFY(firmware_opus_stream_status(decoder, &status) == -EBADF);
    VERIFY(!firmware_audio_request(AUDIO_STREAM_RESET, audio, 0, 0, 0));
    long previous = decoder;
    decoder = firmware_opus_create(2, 1, 1, mapping, 0, 0);
    VERIFY(decoder > previous);
    VERIFY(!firmware_opus_stream_attach(decoder, audio, 0, 0));
    VERIFY(firmware_audio_request(AUDIO_START, audio, 0, 48000, 0) == -EAGAIN);
  }
  firmware_opus_close(decoder);
  memset(padded, 0, 16384);
  uint64_t filled = 0;
  for (unsigned index = 0; index < FIRMWARE_AUDIO_STREAM_SLOTS; ++index) {
    VERIFY(firmware_audio_request(AUDIO_STREAM_WRITE, audio, filled,
        (uintptr_t)padded, 16384) == 16384);
    filled += 4096;
  }
  VERIFY(firmware_audio_request(AUDIO_STREAM_WRITE, audio, filled,
      (uintptr_t)padded, 16384) == -EAGAIN);
  decoder = firmware_opus_create(2, 1, 1, mapping, 0, 0);
  VERIFY(decoder > 0 && !firmware_opus_stream_attach(decoder, audio, 0, 0));
  packet = (FirmwareOpusPacket){.timestamp = filled * 1000000 / 48000,
      .duration = 20000, .bytes = sizeof(media_probe_packet)};
  VERIFY(!firmware_opus_stream_enqueue(decoder, &packet, media_probe_packet));
  FirmwareOpusStreamStatus blocked = {0};
  time_t deadline = time(NULL) + 10;
  do { VERIFY(!firmware_opus_stream_status(decoder, &blocked)); }
  while (blocked.queued && !blocked.error && time(NULL) < deadline);
  // An empty compressed queue can still own PCM blocked by a full device.
  VERIFY(!blocked.error && !blocked.queued && blocked.pending_frames == 960 &&
      blocked.submitted == filled);
  firmware_opus_close(decoder);
  VERIFY(!firmware_audio_request(AUDIO_STREAM_RESET, audio, 0, 0, 0));
  decoder = firmware_opus_create(2, 1, 1, mapping, 0, 0);
  VERIFY(decoder > 0 && !firmware_opus_stream_attach(decoder, audio, 0, 0));
  packet = (FirmwareOpusPacket){.duration = 20000, .bytes = sizeof(media_probe_packet)};
  VERIFY(!firmware_opus_stream_enqueue(decoder, &packet, media_probe_packet));
  packet = (FirmwareOpusPacket){.end = 1};
  VERIFY(!firmware_opus_stream_enqueue(decoder, &packet, NULL));
  FirmwareOpusStreamStatus status = {0};
  time_t until = time(NULL) + 10;
  do { VERIFY(!firmware_opus_stream_status(decoder, &status)); }
  while (!status.ended && !status.error && time(NULL) < until);
  VERIFY(!status.error && status.ended && status.submitted == 960 && !status.queued && !status.pending_frames);
  firmware_opus_close(decoder);
  VERIFY(!firmware_audio_request(AUDIO_CLOSE, audio, 0, 0, 0));
  printf("MEDIA QUEUE PROBE: passed checks=%u packet_limit=512 byte_limit=4194304 cancellation=verified\n", checks);
  return 0;
}

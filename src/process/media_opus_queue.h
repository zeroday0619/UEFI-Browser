/* Compressed bytes and timeline state remain owned by the BSP after submission. */
typedef struct { FirmwareOpusPacket timing; unsigned char *data; } QueuedPacket;
static struct {
  void *timeline;
  long audio;
  QueuedPacket packets[FIRMWARE_OPUS_QUEUE_LIMIT];
  unsigned head, count, bytes, frames, offset, ended;
  int error;
  int64_t first;
  uint64_t submitted, decoded;
  int16_t pcm[MEDIA_FRAME_LIMIT * 2];
} queued_streams[MEDIA_DECODER_SLOTS];
static unsigned queued_inflight, queue_cursor;
static uint64_t synchronous_wait_deadline;
static uint64_t queued_handle;
static FirmwareOpusPacket queued_timing;
static void queue_clear(unsigned slot) {
  for (unsigned index = 0; index < queued_streams[slot].count; ++index)
    free(queued_streams[slot].packets[(queued_streams[slot].head + index) % FIRMWARE_OPUS_QUEUE_LIMIT].data);
  firmware_audio_timeline_destroy(queued_streams[slot].timeline);
  memset(&queued_streams[slot], 0, sizeof(queued_streams[slot]));
}
static void queue_clear_all(void) {
  unsigned attached = 0;
  uint64_t bytes = 0;
  for (unsigned slot = 0; slot < MEDIA_DECODER_SLOTS; ++slot) {
    attached += queued_streams[slot].timeline != NULL;
    bytes += queued_streams[slot].bytes;
    queue_clear(slot);
  }
  if (attached) printf("MEDIA QUEUE: released streams=%u queued_bytes=%llu\n", attached, (unsigned long long)bytes);
  queued_inflight = 0;
  synchronous_wait_deadline = 0;
}
static void queue_output(unsigned slot) {
  if (!queued_streams[slot].frames || queued_streams[slot].error) return;
  const int64_t first = queued_streams[slot].first + queued_streams[slot].offset;
  const int64_t submitted = queued_streams[slot].submitted;
  unsigned remaining = queued_streams[slot].frames - queued_streams[slot].offset;
  if (first < submitted) {
    const uint64_t skip = (uint64_t)(submitted - first);
    queued_streams[slot].offset += skip < remaining ? skip : remaining;
    if (queued_streams[slot].offset == queued_streams[slot].frames)
      queued_streams[slot].frames = queued_streams[slot].offset = 0;
    return;
  }
  int16_t silence[8192] = {0};
  const int silent = first > submitted;
  uint64_t frames = silent ? (uint64_t)(first - submitted) : remaining;
  if (frames > 4096) frames = 4096;
  const int16_t *samples = silent ? silence : queued_streams[slot].pcm + queued_streams[slot].offset * 2;
  long result = firmware_audio_service(1, AUDIO_STREAM_WRITE, queued_streams[slot].audio,
      queued_streams[slot].submitted, (uintptr_t)samples, frames * 4);
  if (result == -EAGAIN) return;
  if (result != (long)(frames * 4)) { queued_streams[slot].error = -EIO; return; }
  queued_streams[slot].submitted += frames;
  if (!silent) {
    queued_streams[slot].offset += frames;
    if (queued_streams[slot].offset == queued_streams[slot].frames)
      queued_streams[slot].frames = queued_streams[slot].offset = 0;
  }
}
static void queue_poll(void) {
  if (queued_inflight && transaction_ready) {
    unsigned slot = queued_inflight - 1;
    if (queued_streams[slot].timeline && streams[slot].handle == queued_handle) {
      if (transaction_result < 0 || transaction_result > MEDIA_FRAME_LIMIT) queued_streams[slot].error = -EIO;
      else {
        int frames = firmware_audio_timeline_process(queued_streams[slot].timeline,
            &queued_timing, transaction_pcm, transaction_result,
            queued_streams[slot].pcm, &queued_streams[slot].first);
        if (frames < 0 || frames > MEDIA_FRAME_LIMIT) queued_streams[slot].error = -EIO;
        else queued_streams[slot].frames = frames;
        ++queued_streams[slot].decoded;
      }
    }
    queued_inflight = 0;
    transaction_ticket = transaction_ready = transaction_samples = 0;
  }
  for (unsigned slot = 0; slot < MEDIA_DECODER_SLOTS; ++slot) queue_output(slot);
  if (transaction_ticket || probe_stage || milliseconds() < synchronous_wait_deadline || !channel) return;
  for (unsigned attempt = 0; attempt < MEDIA_DECODER_SLOTS; ++attempt) {
    unsigned slot = queue_cursor++ % MEDIA_DECODER_SLOTS;
    if (!queued_streams[slot].timeline || queued_streams[slot].error ||
        queued_streams[slot].frames || !queued_streams[slot].count) continue;
    QueuedPacket packet = queued_streams[slot].packets[queued_streams[slot].head];
    queued_streams[slot].head = (queued_streams[slot].head + 1) % FIRMWARE_OPUS_QUEUE_LIMIT;
    --queued_streams[slot].count;
    queued_streams[slot].bytes -= packet.timing.bytes;
    if (packet.timing.end) {
      queued_streams[slot].ended = !queued_streams[slot].count;
      free(packet.data);
      continue;
    }
    if (!packet.timing.bytes) {
      firmware_audio_timeline_process(queued_streams[slot].timeline, &packet.timing,
          NULL, 0, queued_streams[slot].pcm, &queued_streams[slot].first);
      free(packet.data);
      continue;
    }
    if (next_ticket == INT64_MAX) { free(packet.data); queued_streams[slot].error = -EOVERFLOW; return; }
    transaction_command = (FirmwareOpusCommand){.operation = FIRMWARE_OPUS_DECODE,
        .handle = streams[slot].handle, .bytes = packet.timing.bytes};
    transaction_slot = slot;
    transaction_ticket = ++next_ticket;
    transaction_deadline = milliseconds() + 10000;
    transaction_ready = transaction_samples = 0;
    queued_inflight = slot + 1;
    queued_handle = streams[slot].handle;
    queued_timing = packet.timing;
    channel->version = 1;
    channel->operation = FIRMWARE_OPUS_DECODE;
    channel->command = transaction_command;
    channel->slot = slot;
    channel->bytes = packet.timing.bytes;
    memcpy(channel->packet, packet.data, packet.timing.bytes);
    free(packet.data);
    __atomic_store_n(&channel->pending, 1, __ATOMIC_RELEASE);
    return;
  }
}
static long queue_broker(const uint64_t arguments[5]) {
  unsigned slot = 0;
  while (slot < MEDIA_DECODER_SLOTS && (!arguments[1] || streams[slot].handle != arguments[1])) ++slot;
  if (slot == MEDIA_DECODER_SLOTS) return -EBADF;
  if (arguments[0] == 3) {
    if (arguments[3] > 65535 || arguments[4] > 86400000000ULL || !arguments[2] || arguments[2] > LONG_MAX)
      return -EINVAL;
    if (queued_streams[slot].timeline) return -EEXIST;
    for (unsigned index = 0; index < MEDIA_DECODER_SLOTS; ++index)
      if (queued_streams[index].timeline && queued_streams[index].audio == (long)arguments[2]) return -EBUSY;
    const long submitted = firmware_audio_stream_submitted(1, arguments[2]);
    if (submitted < 0) return -EBADF;
    if (transaction_ticket) { synchronous_wait_deadline = milliseconds() + 4; return -EAGAIN; }
    queued_streams[slot].timeline = firmware_audio_timeline_create(streams[slot].channels, arguments[3], arguments[4]);
    synchronous_wait_deadline = 0;
    if (!queued_streams[slot].timeline) return -ENOMEM;
    queued_streams[slot].audio = arguments[2];
    queued_streams[slot].submitted = submitted;
    printf("MEDIA QUEUE: attached stream=%llu audio=%llu capacity=%u\n",
        (unsigned long long)arguments[1], (unsigned long long)arguments[2], FIRMWARE_OPUS_QUEUE_LIMIT);
    return 0;
  }
  if (!queued_streams[slot].timeline) return -EBADF;
  if (arguments[0] == 5) {
    if (arguments[3] || arguments[4]) return -EINVAL;
    if (!renderer_runtime_buffer(arguments[2], sizeof(FirmwareOpusStreamStatus), 1)) return -EFAULT;
    FirmwareOpusStreamStatus status = {queued_streams[slot].submitted,
        queued_streams[slot].count + (queued_inflight == slot + 1), queued_streams[slot].bytes,
        queued_streams[slot].decoded, queued_streams[slot].frames - queued_streams[slot].offset,
        queued_streams[slot].error, queued_streams[slot].ended};
    memcpy((void *)(uintptr_t)arguments[2], &status, sizeof(status));
    return 0;
  }
  if (arguments[0] != 4 || arguments[4]) return -EINVAL;
  FirmwareOpusPacket packet;
  if (!renderer_runtime_buffer(arguments[2], sizeof(packet), 0)) return -EFAULT;
  memcpy(&packet, (const void *)(uintptr_t)arguments[2], sizeof(packet));
  if (packet.bytes > MEDIA_PACKET_LIMIT || packet.padding > 1 || packet.discard_all > 1 || packet.end > 1 ||
      packet.timestamp < -86400000000LL || packet.timestamp > 86400000000LL ||
      packet.duration < 0 || packet.duration > 1200000 || packet.discard_front < 0 ||
      packet.discard_front > 86400000000LL || packet.discard_back < 0 || packet.discard_back > 86400000000LL ||
      (packet.discard_all && (!packet.padding || packet.discard_front || packet.discard_back)) ||
      (packet.end && (packet.bytes || packet.padding || packet.discard_all))) return -EINVAL;
  if (packet.bytes && !renderer_runtime_buffer(arguments[3], packet.bytes, 0)) return -EFAULT;
  if (!packet.bytes && arguments[3]) return -EINVAL;
  if (queued_streams[slot].error) return queued_streams[slot].error;
  if (queued_streams[slot].count == FIRMWARE_OPUS_QUEUE_LIMIT ||
      packet.bytes > FIRMWARE_OPUS_QUEUE_BYTES - queued_streams[slot].bytes) return -EAGAIN;
  unsigned char *data = packet.bytes ? malloc(packet.bytes) : NULL;
  if (packet.bytes && !data) return -ENOMEM;
  if (packet.bytes) memcpy(data, (const void *)(uintptr_t)arguments[3], packet.bytes);
  unsigned tail = (queued_streams[slot].head + queued_streams[slot].count) % FIRMWARE_OPUS_QUEUE_LIMIT;
  queued_streams[slot].packets[tail] = (QueuedPacket){packet, data};
  ++queued_streams[slot].count;
  queued_streams[slot].bytes += packet.bytes;
  queued_streams[slot].ended = 0;
  return 0;
}

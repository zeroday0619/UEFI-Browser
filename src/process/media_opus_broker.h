/* Handles and completion metadata are owned by the BSP, never by the decoder. */
static struct { uint64_t handle; unsigned channels; } streams[MEDIA_DECODER_SLOTS];
static uint64_t next_handle, next_ticket, transaction_ticket, transaction_deadline;
static FirmwareOpusCommand transaction_command;
static unsigned transaction_slot, transaction_ready;
static long transaction_result;
static float transaction_pcm[MEDIA_FRAME_LIMIT * 2];
static size_t transaction_samples;
static unsigned decoded_packets;
#include "media_opus_queue.h"

static void production_poll(void) {
  if (!transaction_ticket || transaction_ready) return;
  if (media_join() != ERROR(6) || milliseconds() >= transaction_deadline) {
    const unsigned was_queued = queued_inflight;
    firmware_media_shutdown();
    queue_clear_all();
    memset(streams, 0, sizeof(streams));
    transaction_result = -EIO;
    transaction_ready = 1;
    if (was_queued) transaction_ticket = transaction_ready = transaction_samples = 0;
    return;
  }
  if (__atomic_load_n(&channel->pending, __ATOMIC_ACQUIRE)) return;
  /* A compromised decoder can forge results but cannot enlarge the copy. */
  const int result = channel->result;
  const unsigned frames = channel->frames;
  const unsigned channels = streams[transaction_slot].channels;
  transaction_result = result <= 0 ? result : -EIO;
  if (transaction_command.operation == FIRMWARE_OPUS_CREATE) {
    if (!result && !frames) {
      streams[transaction_slot].handle = transaction_command.handle;
      streams[transaction_slot].channels = transaction_command.channels;
      transaction_result = transaction_command.handle;
      printf("MEDIA SERVICE: Opus stream=%llu channels=%u cpl=3\n",
          (unsigned long long)transaction_command.handle, transaction_command.channels);
    } else transaction_result = result < 0 ? result : -EIO;
  } else if (transaction_command.operation == FIRMWARE_OPUS_DECODE) {
    if (!result && frames && frames <= MEDIA_FRAME_LIMIT && channels >= 1 && channels <= 2) {
      transaction_samples = (size_t)frames * channels;
      memcpy(transaction_pcm, channel->pcm, transaction_samples * sizeof(float));
      transaction_result = frames;
      if (++decoded_packets == 1 || decoded_packets % 100 == 0)
        printf("MEDIA SERVICE: production decoded_packets=%u frames=%u cpl=3\n", decoded_packets, frames);
    } else transaction_result = result < 0 ? result : -EIO;
  } else {
    if (frames) transaction_result = -EIO;
    if (transaction_command.operation == FIRMWARE_OPUS_CLOSE)
      memset(&streams[transaction_slot], 0, sizeof(streams[transaction_slot]));
  }
  transaction_ready = 1;
}

long firmware_media_broker(const uint64_t arguments[5]) {
  if (arguments[0] >= 3 && arguments[0] <= 5) return queue_broker(arguments);
  if (arguments[4]) return -EINVAL;
  if (arguments[0] == 2) {
    if (queued_inflight || !transaction_ticket || arguments[1] != transaction_ticket) return -ESTALE;
    production_poll();
    if (!transaction_ready) return -EAGAIN;
    if (transaction_samples) {
      if (arguments[3] < transaction_samples || arguments[3] > MEDIA_FRAME_LIMIT * 2)
        return -EINVAL;
      if (!renderer_runtime_buffer(arguments[2], transaction_samples * sizeof(float), 1))
        return -EFAULT;
      memcpy((void *)(uintptr_t)arguments[2], transaction_pcm, transaction_samples * sizeof(float));
    } else if (transaction_command.operation != FIRMWARE_OPUS_DECODE &&
               (arguments[2] || arguments[3])) return -EINVAL;
    const long result = transaction_result;
    transaction_ticket = transaction_ready = transaction_samples = 0;
    return result;
  }
  if (arguments[0] != 1 || arguments[3]) return -EINVAL;
  FirmwareOpusCommand command;
  if (!renderer_runtime_buffer(arguments[1], sizeof(command), 0)) return -EFAULT;
  memcpy(&command, (const void *)(uintptr_t)arguments[1], sizeof(command));
  if (command.operation < FIRMWARE_OPUS_CREATE || command.operation > FIRMWARE_OPUS_CLOSE ||
      command.bytes > MEDIA_PACKET_LIMIT) return -EINVAL;
  unsigned slot = MEDIA_DECODER_SLOTS;
  if (command.operation == FIRMWARE_OPUS_CREATE) {
    if (command.handle || command.bytes || arguments[2] || !valid_configuration(&command)) return -EINVAL;
    for (unsigned index = 0; index < MEDIA_DECODER_SLOTS; ++index)
      if (!streams[index].handle) { slot = index; break; }
    if (slot == MEDIA_DECODER_SLOTS) return -ENOSPC;
  } else {
    for (unsigned index = 0; index < MEDIA_DECODER_SLOTS; ++index)
      if (command.handle && streams[index].handle == command.handle) { slot = index; break; }
    if (slot == MEDIA_DECODER_SLOTS) return -EBADF;
    if (command.operation == FIRMWARE_OPUS_DECODE) {
      if (!command.bytes) return -EINVAL;
      if (!renderer_runtime_buffer(arguments[2], command.bytes, 0)) return -EFAULT;
    } else if (command.bytes || arguments[2]) return -EINVAL;
  }
  if (command.operation == FIRMWARE_OPUS_RESET || command.operation == FIRMWARE_OPUS_CLOSE) queue_clear(slot);
  if (command.operation == FIRMWARE_OPUS_DECODE && queued_streams[slot].timeline) return -EBUSY;
  if (transaction_ticket || probe_stage) { synchronous_wait_deadline = milliseconds() + 4; return -EAGAIN; }
  synchronous_wait_deadline = 0;
  if (!initialize()) return -ENODEV;
  if (media_join() != ERROR(6)) {
    firmware_media_reset();
    return -EIO;
  }
  if (!__atomic_load_n(&channel->initialized, __ATOMIC_ACQUIRE)) {
    if (milliseconds() < startup_deadline) return -EAGAIN;
    firmware_media_reset();
    return -ETIMEDOUT;
  }
  if (channel->cpl != 3 || channel->denied_capabilities != 5) return -EIO;
  if (next_ticket == INT64_MAX || next_handle == INT64_MAX) return -EOVERFLOW;
  if (command.operation == FIRMWARE_OPUS_CREATE) command.handle = ++next_handle;
  transaction_command = command;
  transaction_slot = slot;
  transaction_ticket = ++next_ticket;
  transaction_deadline = milliseconds() + 10000;
  transaction_ready = transaction_samples = 0;
  channel->version = 1;
  channel->operation = command.operation;
  channel->command = command;
  channel->slot = slot;
  channel->bytes = command.bytes;
  if (command.bytes) memcpy(channel->packet, (const void *)(uintptr_t)arguments[2], command.bytes);
  __atomic_store_n(&channel->pending, 1, __ATOMIC_RELEASE);
  return transaction_ticket;
}
void firmware_media_reset(void) {
  firmware_media_shutdown();
  queue_clear_all();
  memset(streams, 0, sizeof(streams));
  transaction_ticket = transaction_ready = transaction_samples = 0;
}

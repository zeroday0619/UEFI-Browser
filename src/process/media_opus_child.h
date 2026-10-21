/* Each decoder lives only in the media process's private data mapping. */
typedef struct OpusMSDecoder OpusMSDecoder;
extern int opus_multistream_decoder_get_size(int streams, int coupled);
extern int opus_multistream_decoder_init(OpusMSDecoder *, int rate, int channels,
    int streams, int coupled, const unsigned char *mapping);
extern int opus_multistream_decoder_ctl(OpusMSDecoder *, int request, ...);
extern int opus_multistream_decode_float(OpusMSDecoder *, const unsigned char *,
    int bytes, float *, int frames, int fec);
enum { MEDIA_DECODER_SLOTS = 8 };
static struct {
  unsigned channels;
  unsigned char storage[131072] __attribute__((aligned(64)));
} decoders[MEDIA_DECODER_SLOTS];
static int valid_configuration(const FirmwareOpusCommand *command) {
  if (command->channels < 1 || command->channels > 2 || !command->streams ||
      command->streams > 2 || command->coupled > command->streams ||
      command->streams + command->coupled > 2 || command->phase_disabled > 1 ||
      command->gain < -32768 || command->gain > 32767) return 0;
  for (unsigned index = 0; index < command->channels; ++index)
    if (command->mapping[index] >= command->streams + command->coupled &&
        command->mapping[index] != 255) return 0;
  return 1;
}
static int decode_command(MediaChannel *message) {
  const FirmwareOpusCommand command = message->command;
  const unsigned slot = message->slot;
  if (slot >= MEDIA_DECODER_SLOTS || command.operation != message->operation ||
      command.bytes != message->bytes) return -EINVAL;
  OpusMSDecoder *decoder = (OpusMSDecoder *)decoders[slot].storage;
  if (command.operation == FIRMWARE_OPUS_CREATE) {
    if (!valid_configuration(&command) || command.bytes) return -EINVAL;
    const int size = opus_multistream_decoder_get_size(command.streams, command.coupled);
    if (size <= 0 || (size_t)size > sizeof(decoders[slot].storage)) return -EINVAL;
    const unsigned char mapping[2] = {command.mapping[0], command.mapping[1]};
    decoders[slot].channels = 0;
    int result = opus_multistream_decoder_init(decoder, 48000, command.channels,
        command.streams, command.coupled, mapping);
    /* These request numbers are part of the pinned Opus public control ABI. */
    if (!result) result = opus_multistream_decoder_ctl(decoder, 4034, command.gain);
    if (!result) result = opus_multistream_decoder_ctl(decoder, 4046, command.phase_disabled);
    if (!result) decoders[slot].channels = command.channels;
    return result;
  }
  if (!decoders[slot].channels) return -EBADF;
  if (command.operation == FIRMWARE_OPUS_DECODE) {
    if (!command.bytes || command.bytes > MEDIA_PACKET_LIMIT) return -EINVAL;
    memcpy(packet_storage, message->packet, command.bytes);
    int frames = opus_multistream_decode_float(decoder, packet_storage, command.bytes,
        pcm_storage, MEDIA_FRAME_LIMIT, 0);
    if (frames <= 0 || frames > MEDIA_FRAME_LIMIT) return frames < 0 ? frames : -EIO;
    memcpy(message->pcm, pcm_storage, (size_t)frames * decoders[slot].channels * sizeof(float));
    message->frames = frames;
    return 0;
  }
  if (command.bytes) return -EINVAL;
  if (command.operation == FIRMWARE_OPUS_RESET)
    return opus_multistream_decoder_ctl(decoder, 4028);
  if (command.operation == FIRMWARE_OPUS_CLOSE) {
    memset(&decoders[slot], 0, sizeof(decoders[slot]));
    return 0;
  }
  return -EINVAL;
}

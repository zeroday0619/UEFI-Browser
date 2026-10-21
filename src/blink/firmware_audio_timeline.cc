#include "firmware_media.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <new>
#include "media/base/audio_buffer.h"
#include "media/base/audio_discard_helper.h"
#include "media/base/channel_layout.h"
namespace {
struct Timeline {
  unsigned channels;
  int64_t origin;
  media::AudioDiscardHelper discard{48000, 0, false};
};
}
extern "C" void* firmware_audio_timeline_create(unsigned channels, unsigned discard, int64_t origin) {
  if (channels < 1 || channels > 2 || discard > 65535 || origin < 0 || origin > 86400000000LL)
    return nullptr;
  auto* context = new (std::nothrow) Timeline{channels, origin};
  if (context) context->discard.Reset(discard);
  return context;
}
extern "C" void firmware_audio_timeline_destroy(void* context) {
  delete static_cast<Timeline*>(context);
}
extern "C" int firmware_audio_timeline_process(void* opaque, const FirmwareOpusPacket* packet,
    const float* input, unsigned frames, int16_t* output, int64_t* first) {
  auto* context = static_cast<Timeline*>(opaque);
  if (!context || frames > FIRMWARE_OPUS_FRAME_LIMIT) return -1;
  media::AudioDiscardHelper::TimeInfo timing{
      base::Microseconds(packet->timestamp), base::Microseconds(packet->duration), std::nullopt};
  if (packet->padding)
    timing.discard_padding = media::DecoderBuffer::DiscardPadding(
        packet->discard_all ? media::kInfiniteDuration : base::Microseconds(packet->discard_front),
        base::Microseconds(packet->discard_back));
  if (!frames) { context->discard.ProcessBuffers(timing, nullptr); return 0; }
  auto buffer = media::AudioBuffer::CreateBuffer(media::kSampleFormatF32,
      context->channels == 1 ? media::CHANNEL_LAYOUT_MONO : media::CHANNEL_LAYOUT_STEREO,
      context->channels, 48000, frames);
  if (!buffer) return -1;
  std::memcpy(buffer->interleaved_data_cast<float>().data(), input,
              size_t(frames) * context->channels * sizeof(float));
  if (!context->discard.ProcessBuffers(timing, buffer.get())) return 0;
  const auto timestamp = buffer->timestamp();
  if (timestamp.is_inf() || timestamp == media::kNoTimestamp) return -1;
  const double relative = double(timestamp.InMicroseconds() - context->origin) * .048;
  if (!std::isfinite(relative) || std::abs(relative) > 86400.0 * 48000) return -1;
  *first = std::llround(relative);
  const auto samples = buffer->interleaved_data_cast<float>();
  for (int frame = 0; frame < buffer->frame_count(); ++frame)
    for (unsigned channel = 0; channel < 2; ++channel) {
      float value = samples[frame * context->channels + (context->channels == 1 ? 0 : channel)];
      if (!std::isfinite(value)) value = 0;
      output[frame * 2 + channel] = int16_t(std::lround(std::clamp(value, -1.0f, 1.0f) * 32767));
    }
  return buffer->frame_count();
}

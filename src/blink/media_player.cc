#include "blink/media_player.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <limits>
#include <vector>
#include "base/functional/bind.h"
#include "base/memory/weak_ptr.h"
#include "base/task/single_thread_task_runner.h"
#include "base/timer/timer.h"
#include "firmware_audio.h"
#include "media/base/media_content_type.h"
#include "media/base/media_log.h"
#include "media/base/media_tracks.h"
#include "media/base/stream_parser_buffer.h"
#include "media/formats/webm/webm_stream_parser.h"
#include "media/gfx/paint_canvas_video_renderer.h"
#include "third_party/blink/public/platform/web_media_player.h"
#include "third_party/blink/public/platform/web_media_player_source.h"
#include "third_party/blink/public/platform/web_time_range.h"
#include "third_party/blink/public/platform/web_url_error.h"
#include "third_party/blink/public/platform/web_url_request.h"
#include "third_party/blink/public/platform/web_url_response.h"
#include "third_party/blink/public/web/web_associated_url_loader.h"
#include "third_party/blink/public/web/web_associated_url_loader_client.h"
#include "third_party/blink/public/web/web_local_frame.h"
#include "third_party/blink/renderer/platform/heap/persistent.h"
#include "third_party/blink/renderer/platform/media/media_player_client.h"
#include "third_party/libvpx/source/libvpx/vpx/vp8dx.h"
#include "third_party/libvpx/source/libvpx/vpx/vpx_decoder.h"
#include "third_party/libyuv/include/libyuv/convert_argb.h"

namespace uefi_browser {
namespace {
constexpr size_t kEncodedLimit = 8 * 1024 * 1024;
constexpr size_t kDecodedLimit = 64 * 1024 * 1024;
constexpr size_t kPacketLimit = 8192;
class MediaLog final : public media::MediaLog {
 public:
  MediaLog() = default;
};
class Player final : public blink::WebMediaPlayer,
                     public blink::WebAssociatedURLLoaderClient {
 public:
  Player(blink::WebLocalFrame* frame, blink::WebMediaPlayerClient* client)
      : frame_(frame),
        client_(static_cast<blink::MediaPlayerClient*>(client)) {}
  ~Player() override { Shutdown(); }
  void Shutdown() override {
    weak_.InvalidateWeakPtrs();
    CloseAudio();
    timer_.Stop();
    if (loader_)
      loader_->Cancel();
    loader_.reset();
    parser_.reset();
    if (codec_open_) {
      vpx_codec_destroy(&codec_);
      codec_open_ = false;
    }
    painter_.ResetCache();
    frames_.clear();
    packets_.clear();
    client_ = nullptr;
    frame_ = nullptr;
  }
  LoadTiming Load(LoadType type,
                  const blink::WebMediaPlayerSource& source,
                  CorsMode cors,
                  bool) override {
    Reset();
    if (type != kLoadTypeURL || !source.IsURL()) {
      PostFailure(kNetworkStateFormatError,
                  "UEFI_MEDIA_SOURCE: only URL WebM video and PCM WAV audio "
                  "are supported");
      return kImmediate;
    }
    network_ = kNetworkStateLoading;
    source_ = GURL(source.GetAsURL().GetString().Utf8());
    blink::WebURLRequest request(source.GetAsURL());
    request.SetHttpMethod(blink::WebString::FromUtf8("GET"));
    request.SetRequestContext(client_->IsAudioElement()
                                  ? blink::mojom::RequestContextType::AUDIO
                                  : blink::mojom::RequestContextType::VIDEO);
    // Access checks stay in Blink and the firmware loader; no universal-access
    // loader is used.
    request.SetMode(cors == kCorsModeUnspecified
                        ? network::mojom::RequestMode::kSameOrigin
                        : network::mojom::RequestMode::kCors);
    request.SetCredentialsMode(
        cors == kCorsModeUseCredentials
            ? network::mojom::CredentialsMode::kInclude
            : network::mojom::CredentialsMode::kSameOrigin);
    loader_ = frame_->CreateAssociatedURLLoader(
        blink::WebAssociatedURLLoaderOptions());
    loader_->LoadAsynchronously(request, this);
    return kImmediate;
  }
  void Play() override {
    if (!client_)
      return;
    if (IsEnded())
      position_ = 0;
    paused_ = false;
    if (HasAudio() && !StartAudio())
      return;
    anchor_ = base::TimeTicks::Now();
    client_->DidPlayerStartPlaying();
    timer_.Start(FROM_HERE, base::Milliseconds(16), this, &Player::Tick);
  }
  void Pause(PauseReason) override {
    position_ = CurrentTime();
    if (HasAudio())
      firmware_audio_request(AUDIO_STOP, audio_handle_, 0, 0, 0);
    paused_ = true;
    timer_.Stop();
    if (client_)
      client_->DidPlayerPaused(IsEnded());
  }
  void Seek(double seconds) override {
    if (!client_ || !HasMedia() || !std::isfinite(seconds))
      return;
    if (HasAudio())
      firmware_audio_request(AUDIO_STOP, audio_handle_, 0, 0, 0);
    position_ = std::clamp(seconds, 0.0, duration_);
    anchor_ = base::TimeTicks::Now();
    seeking_ = true;
    base::SingleThreadTaskRunner::GetCurrentDefault()->PostTask(
        FROM_HERE, base::BindOnce(&Player::FinishSeek, weak_.GetWeakPtr()));
  }
  void SetRate(double rate) override {
    if (!std::isfinite(rate) || rate < 0 || rate > 16)
      return;
    if (HasAudio() && rate &&
        (rate * audio_rate_ < 8000 || rate * audio_rate_ > 48000))
      return;
    position_ = CurrentTime();
    anchor_ = base::TimeTicks::Now();
    rate_ = rate;
    if (HasAudio() && !paused_)
      StartAudio();
  }
  void SetVolume(double volume) override {
    volume_ = std::clamp(volume, 0.0, 1.0);
    UpdateVolume();
  }
  void SetLatencyHint(double) override {}
  void SetPreservesPitch(bool) override {}
  void SetWasPlayedWithUserActivationAndHighMediaEngagement(bool) override {}
  void SetShouldPauseWhenFrameIsHidden(bool value) override {
    pause_when_hidden_ = value;
  }
  bool GetShouldPauseWhenFrameIsHidden() override { return pause_when_hidden_; }
  void OnFrameHidden() override {
    if (pause_when_hidden_)
      Pause(PauseReason::kFrameHidden);
  }
  void OnRequestPictureInPicture() override {}
  void OnFrozen() override { Pause(PauseReason::kFrameFrozen); }
  bool SetSinkId(const blink::WebString&,
                 blink::WebSetSinkIdCompleteCallback callback) override {
    std::move(callback).Run(blink::WebSetSinkIdError::kNotSupported);
    return true;
  }
  blink::WebTimeRanges Buffered() const override {
    return !HasMedia() ? blink::WebTimeRanges()
                       : blink::WebTimeRanges(0, duration_);
  }
  blink::WebTimeRanges Seekable() const override { return Buffered(); }
  bool HasVideo() const override { return !frames_.empty(); }
  bool HasAudio() const override { return audio_handle_ > 0; }
  bool IsVideoBeingCaptured() const override { return false; }
  gfx::Size NaturalSize() const override { return size_; }
  gfx::Size VisibleSize() const override { return size_; }
  bool Paused() const override { return paused_; }
  bool Seeking() const override { return seeking_; }
  double Duration() const override { return duration_; }
  double CurrentTime() const override {
    if (!HasMedia())
      return 0;
    if (HasAudio()) {
      if (paused_ || seeking_ || !rate_)
        return position_;
      long frame =
          firmware_audio_request(AUDIO_POSITION, audio_handle_, 0, 0, 0);
      return frame >= 0
                 ? std::clamp(double(frame) / audio_rate_, 0.0, duration_)
                 : position_;
    }
    double value = position_;
    if (!paused_ && !seeking_)
      value += (base::TimeTicks::Now() - anchor_).InSecondsF() * rate_;
    return std::clamp(value, 0.0, duration_);
  }
  bool IsEnded() const override {
    return HasMedia() && CurrentTime() >= duration_;
  }
  NetworkState GetNetworkState() const override { return network_; }
  ReadyState GetReadyState() const override { return ready_; }
  blink::WebString GetErrorMessage() const override {
    return blink::WebString::FromUtf8(error_);
  }
  bool DidLoadingProgress() override { return std::exchange(progress_, false); }
  bool WouldTaintOrigin() const override { return false; }
  double MediaTimeForTimeValue(double value) const override { return value; }
  unsigned DecodedFrameCount() const override {
    return static_cast<unsigned>(frames_.size());
  }
  unsigned DroppedFrameCount() const override { return dropped_; }
  uint64_t AudioDecodedByteCount() const override { return audio_frames_ * 4; }
  uint64_t VideoDecodedByteCount() const override { return decoded_bytes_; }
  void SetVolumeMultiplier(double volume) override {
    volume_multiplier_ = std::clamp(volume, 0.0, 1.0);
    UpdateVolume();
  }
  void SuspendForFrameClosed() override { Pause(PauseReason::kFrameFrozen); }
  bool HasAvailableVideoFrame() const override { return !frames_.empty(); }
  bool HasReadableVideoFrame() const override {
    return HasAvailableVideoFrame();
  }
  void Paint(cc::PaintCanvas* canvas,
             const gfx::Rect& rectangle,
             const cc::PaintFlags& flags,
             bool) override {
    media::PaintCanvasVideoRenderer::PaintParams parameters;
    parameters.dest_rect = gfx::RectF(rectangle);
    painter_.Paint(CurrentFrame(), canvas, flags, parameters, nullptr);
  }
  scoped_refptr<media::VideoFrame> GetCurrentFrameThenUpdate() override {
    return CurrentFrame();
  }
  std::optional<media::VideoFrame::ID> CurrentFrameId() const override {
    auto frame = CurrentFrame();
    return frame ? std::optional(frame->unique_id()) : std::nullopt;
  }
  media::PaintCanvasVideoRenderer* GetPaintCanvasVideoRenderer() override {
    return &painter_;
  }
  base::WeakPtr<blink::WebMediaPlayer> AsWeakPtr() override {
    return weak_.GetWeakPtr();
  }
  GURL GetSrcAfterRedirects() override { return source_; }
  void RecordAutoPictureInPictureInfo(
      const media::PictureInPictureEventsInfo::AutoPipInfo&) override {}
  void RequestVideoFrameCallback() override { frame_callback_ = true; }
  std::unique_ptr<VideoFramePresentationMetadata>
  GetVideoFramePresentationMetadata() override {
    if (frames_.empty())
      return nullptr;
    auto result = std::make_unique<VideoFramePresentationMetadata>();
    result->presented_frames = presented_;
    result->presentation_time = presentation_time_;
    result->expected_display_time = presentation_time_;
    result->width = size_.width();
    result->height = size_.height();
    result->media_time = frames_[displayed_]->timestamp();
    result->rendering_interval = base::Milliseconds(16);
    result->average_frame_duration = base::Seconds(duration_ / frames_.size());
    return result;
  }
  bool WillFollowRedirect(const blink::WebURL& url,
                          const blink::WebURLResponse&) override {
    source_ = GURL(url.GetString().Utf8());
    return true;
  }
  void DidReceiveResponse(const blink::WebURLResponse& response) override {
    if (response.HttpStatusCode() != 200)
      PostFailure(kNetworkStateNetworkError,
                  "UEFI_MEDIA_HTTP: HTTP response is not 200");
  }
  void DidReceiveData(base::span<const char> bytes) override {
    if (failed_)
      return;
    if (bytes.size() > kEncodedLimit - encoded_.size()) {
      PostFailure(kNetworkStateDecodeError,
                  "UEFI_MEDIA_QUOTA: encoded stream exceeds 8 MiB");
      return;
    }
    encoded_.insert(encoded_.end(), bytes.begin(), bytes.end());
    received_bytes_ += bytes.size();
    progress_ = true;
  }
  void DidFail(const blink::WebURLError&) override {
    PostFailure(kNetworkStateNetworkError,
                "UEFI_MEDIA_NETWORK: media fetch failed");
  }
  void DidFinishLoading() override {
    if (failed_)
      return;
    base::SingleThreadTaskRunner::GetCurrentDefault()->PostTask(
        FROM_HERE, base::BindOnce(&Player::ParseStart, weak_.GetWeakPtr()));
  }

 private:
  bool HasMedia() const { return HasAudio() || !frames_.empty(); }
  void CloseAudio() {
    if (audio_handle_ > 0)
      firmware_audio_request(AUDIO_CLOSE, audio_handle_, 0, 0, 0);
    audio_handle_ = 0;
    audio_frames_ = 0;
    audio_samples_.clear();
  }
  void UpdateVolume() {
    if (!HasAudio())
      return;
    double volume = volume_ * volume_multiplier_;
    unsigned gain = unsigned(std::lround(volume * 65536));
    firmware_audio_request(AUDIO_VOLUME, audio_handle_, gain, 0, 0);
  }
  bool StartAudio() {
    if (!rate_) {
      firmware_audio_request(AUDIO_STOP, audio_handle_, 0, 0, 0);
      return true;
    }
    long result = firmware_audio_request(
        AUDIO_START, audio_handle_,
        std::min(audio_frames_, size_t(std::llround(position_ * audio_rate_))),
        unsigned(std::lround(audio_rate_ * rate_)), 0);
    if (result < 0) {
      PostFailure(kNetworkStateDecodeError,
                  "UEFI_AUDIO_OUTPUT: audio device could not start playback");
      return false;
    }
    return true;
  }
  void ParseAudio() {
    auto read16 = [](const uint8_t* data) {
      return unsigned(data[0]) | unsigned(data[1]) << 8;
    };
    auto read32 = [](const uint8_t* data) {
      return uint32_t(data[0]) | uint32_t(data[1]) << 8 |
             uint32_t(data[2]) << 16 | uint32_t(data[3]) << 24;
    };
    const uint8_t* format = nullptr;
    const uint8_t* samples = nullptr;
    size_t sample_bytes = 0;
    size_t end = size_t(read32(encoded_.data() + 4)) + 8;
    if (end != encoded_.size()) {
      AudioFormatFailure();
      return;
    }
    for (size_t offset = 12; offset < end;) {
      if (end - offset < 8) {
        AudioFormatFailure();
        return;
      }
      const uint8_t* chunk = encoded_.data() + offset;
      size_t length = read32(chunk + 4);
      offset += 8;
      if (length > end - offset || (length & 1) > end - offset - length) {
        AudioFormatFailure();
        return;
      }
      if (!std::memcmp(chunk, "fmt ", 4)) {
        if (format || length < 16) {
          AudioFormatFailure();
          return;
        }
        format = chunk + 8;
      } else if (!std::memcmp(chunk, "data", 4)) {
        if (samples) {
          AudioFormatFailure();
          return;
        }
        samples = chunk + 8;
        sample_bytes = length;
      }
      offset += length + (length & 1);
    }
    if (!format || !samples || read16(format) != 1) {
      AudioFormatFailure();
      return;
    }
    unsigned channels = read16(format + 2), bits = read16(format + 14);
    audio_rate_ = read32(format + 4);
    if ((channels != 1 && channels != 2) || (bits != 8 && bits != 16) ||
        audio_rate_ < 8000 || audio_rate_ > 48000) {
      AudioFormatFailure();
      return;
    }
    unsigned alignment = channels * (bits / 8);
    if (read16(format + 12) != alignment ||
        read32(format + 8) != audio_rate_ * alignment || !sample_bytes ||
        sample_bytes % alignment) {
      AudioFormatFailure();
      return;
    }
    audio_frames_ = sample_bytes / alignment;
    if (audio_frames_ > FIRMWARE_AUDIO_LIMIT / 4) {
      PostFailure(kNetworkStateFormatError,
                  "UEFI_AUDIO_QUOTA: PCM exceeds DMA capacity");
      return;
    }
    audio_samples_.resize(audio_frames_ * 2);
    for (size_t frame = 0; frame < audio_frames_; ++frame)
      for (unsigned channel = 0; channel < 2; ++channel) {
        const uint8_t* sample = samples + frame * alignment +
                                (channels == 1 ? 0 : channel * (bits / 8));
        audio_samples_[frame * 2 + channel] =
            bits == 8 ? (int(sample[0]) - 128) * 256 : int16_t(read16(sample));
      }
    audio_handle_ = firmware_audio_request(AUDIO_CREATE, 0, audio_frames_ * 4,
                                           audio_rate_, 0);
    if (audio_handle_ <= 0) {
      PostFailure(kNetworkStateFormatError,
                  "UEFI_AUDIO_DEVICE: no available AC97 output");
      return;
    }
    audio_uploaded_ = 0;
    encoded_.clear();
    UploadAudio();
  }
  void AudioFormatFailure() {
    PostFailure(kNetworkStateFormatError,
                "UEFI_AUDIO_FORMAT: expected bounded PCM WAV, mono/stereo, "
                "8/16-bit, 8-48 kHz");
  }
  void UploadAudio() {
    if (failed_)
      return;
    size_t total = audio_frames_ * 4;
    if (audio_uploaded_ < total) {
      size_t bytes = std::min(size_t{16384}, total - audio_uploaded_);
      const uint8_t* samples =
          reinterpret_cast<const uint8_t*>(audio_samples_.data());
      if (firmware_audio_request(
              AUDIO_UPLOAD, audio_handle_, audio_uploaded_,
              reinterpret_cast<uintptr_t>(samples + audio_uploaded_),
              bytes) != long(bytes)) {
        PostFailure(kNetworkStateFormatError,
                    "UEFI_AUDIO_UPLOAD: PCM transfer failed");
        return;
      }
      audio_uploaded_ += bytes;
      base::SingleThreadTaskRunner::GetCurrentDefault()->PostTask(
          FROM_HERE, base::BindOnce(&Player::UploadAudio, weak_.GetWeakPtr()));
      return;
    }
    audio_samples_.clear();
    audio_samples_.shrink_to_fit();
    loader_.reset();
    duration_ = double(audio_frames_) / audio_rate_;
    anchor_ = base::TimeTicks::Now();
    UpdateVolume();
    network_ = kNetworkStateLoaded;
    ready_ = kReadyStateHaveMetadata;
    client_->AddTrack(media::MediaTrack::CreateAudioTrack(
        "audio", media::MediaTrack::AudioKind::kMain, "", "", true));
    client_->DidMediaMetadataChange(
        true, false, media::AudioCodec::kPCM, media::VideoCodec::kUnknown,
        media::MediaContentType::kPersistent, false);
    client_->DurationChanged();
    client_->ReadyStateChanged();
    ready_ = kReadyStateHaveEnoughData;
    client_->ReadyStateChanged();
    client_->NetworkStateChanged();
    if (!paused_)
      StartAudio();
    std::printf("AUDIO: PCM frames=%zu rate=%u duration=%.3f handle=%ld\n",
                audio_frames_, audio_rate_, duration_, audio_handle_);
  }
  void Reset() {
    weak_.InvalidateWeakPtrs();
    CloseAudio();
    timer_.Stop();
    if (loader_)
      loader_->Cancel();
    loader_.reset();
    parser_.reset();
    if (codec_open_) {
      vpx_codec_destroy(&codec_);
      codec_open_ = false;
    }
    encoded_.clear();
    packets_.clear();
    painter_.ResetCache();
    frames_.clear();
    error_.clear();
    failed_ = false;
    progress_ = false;
    paused_ = true;
    seeking_ = false;
    position_ = 0;
    duration_ = std::numeric_limits<double>::quiet_NaN();
    ready_ = kReadyStateHaveNothing;
    network_ = kNetworkStateEmpty;
    decoded_bytes_ = 0;
    received_bytes_ = 0;
    packet_ = 0;
    displayed_ = 0;
    presented_ = 0;
    dropped_ = 0;
    size_ = {};
    video_track_ = -1;
    frame_callback_ = false;
  }
  void PostFailure(NetworkState state, const char* message) {
    if (failed_)
      return;
    failed_ = true;
    base::SingleThreadTaskRunner::GetCurrentDefault()->PostTask(
        FROM_HERE, base::BindOnce(&Player::Fail, weak_.GetWeakPtr(), state,
                                  std::string(message)));
  }
  void Fail(NetworkState state, std::string message) {
    CloseAudio();
    timer_.Stop();
    paused_ = true;
    if (loader_)
      loader_->Cancel();
    loader_.reset();
    parser_.reset();
    if (codec_open_) {
      vpx_codec_destroy(&codec_);
      codec_open_ = false;
    }
    painter_.ResetCache();
    frames_.clear();
    packets_.clear();
    encoded_.clear();
    decoded_bytes_ = 0;
    // Blink's decode-error path requires previously published media metadata.
    // Buffered decoding can fail before that transition, so it is a load
    // failure.
    network_ =
        state == kNetworkStateDecodeError && ready_ == kReadyStateHaveNothing
            ? kNetworkStateFormatError
            : state;
    error_ = std::move(message);
    std::printf("MEDIA: %s\n", error_.c_str());
    if (client_)
      client_->NetworkStateChanged();
  }
  void ParseStart() {
    if (encoded_.size() >= 12 && !std::memcmp(encoded_.data(), "RIFF", 4) &&
        !std::memcmp(encoded_.data() + 8, "WAVE", 4)) {
      ParseAudio();
      return;
    }
    parser_ = std::make_unique<media::WebMStreamParser>();
    parser_->Init(
        base::BindOnce(
            [](base::WeakPtr<Player> self,
               const media::StreamParser::InitParameters& parameters) {
              if (self)
                self->duration_ = parameters.duration.InSecondsF();
            },
            weak_.GetWeakPtr()),
        base::BindRepeating(&Player::Configure, base::Unretained(this)),
        base::BindRepeating(&Player::Buffers, base::Unretained(this)),
        base::BindRepeating(
            [](base::WeakPtr<Player> self, media::EmeInitDataType,
               const std::vector<uint8_t>&) {
              if (self)
                self->PostFailure(
                    kNetworkStateFormatError,
                    "UEFI_MEDIA_ENCRYPTED: encrypted video is unsupported");
            },
            weak_.GetWeakPtr()),
        base::BindRepeating([] {}), base::BindRepeating([] {}), &media_log_);
    if (!parser_->AppendToParseBuffer(encoded_)) {
      PostFailure(kNetworkStateDecodeError,
                  "UEFI_MEDIA_PARSE: media buffer rejected");
      return;
    }
    encoded_.clear();
    ParseNext();
  }
  bool Configure(std::unique_ptr<media::MediaTracks> tracks) {
    if (video_track_ != -1 || !tracks->GetAudioConfigs().empty() ||
        tracks->GetVideoConfigs().size() != 1)
      return false;
    auto& [identifier, config] = *tracks->GetVideoConfigs().begin();
    if (config.is_encrypted() ||
        (config.codec() != media::VideoCodec::kVP8 &&
         config.codec() != media::VideoCodec::kVP9) ||
        config.coded_size().width() > 1920 ||
        config.coded_size().height() > 1080)
      return false;
    size_ = config.visible_rect().size();
    if (size_.IsEmpty())
      return false;
    vpx_codec_dec_cfg_t configuration{};
    configuration.threads = 1;
    auto* interface = config.codec() == media::VideoCodec::kVP8
                          ? vpx_codec_vp8_dx()
                          : vpx_codec_vp9_dx();
    if (vpx_codec_dec_init(&codec_, interface, &configuration, 0) !=
        VPX_CODEC_OK)
      return false;
    codec_open_ = true;
    video_track_ = identifier;
    codec_type_ = config.codec();
    return true;
  }
  bool Buffers(const media::StreamParser::BufferQueueMap& buffers) {
    for (const auto& [track, queue] : buffers) {
      if (track != video_track_ ||
          queue.size() > kPacketLimit - packets_.size())
        return false;
      for (auto& buffer : queue) {
        if (buffer->decrypt_config() || buffer->timestamp().is_inf() ||
            buffer->timestamp().is_negative())
          return false;
        packets_.push_back(buffer);
      }
    }
    return true;
  }
  void ParseNext() {
    if (failed_)
      return;
    auto result = parser_->Parse(media::StreamParser::kMaxPendingBytesPerParse);
    if (result == media::StreamParser::ParseStatus::kFailed) {
      PostFailure(kNetworkStateFormatError,
                  "UEFI_MEDIA_FORMAT: expected clear VP8/VP9 video-only WebM");
      return;
    }
    if (result == media::StreamParser::ParseStatus::kSuccessHasMoreData) {
      base::SingleThreadTaskRunner::GetCurrentDefault()->PostTask(
          FROM_HERE, base::BindOnce(&Player::ParseNext, weak_.GetWeakPtr()));
      return;
    }
    if (!codec_open_ || packets_.empty()) {
      PostFailure(kNetworkStateDecodeError,
                  "UEFI_MEDIA_EMPTY: no decodable frames");
      return;
    }
    DecodeNext();
  }
  void DecodeNext() {
    if (failed_)
      return;
    if (packet_ == packets_.size()) {
      Loaded();
      return;
    }
    auto& packet = packets_[packet_++];
    if (vpx_codec_decode(&codec_, base::span(*packet).data(),
                         static_cast<unsigned>(packet->size()), nullptr,
                         0) != VPX_CODEC_OK) {
      PostFailure(kNetworkStateDecodeError,
                  "UEFI_MEDIA_CODEC: compressed frame rejected");
      return;
    }
    vpx_codec_iter_t iterator = nullptr;
    while (auto* image = vpx_codec_get_frame(&codec_, &iterator)) {
      if (image->fmt != VPX_IMG_FMT_I420 || image->d_w > 1920 ||
          image->d_h > 1080 || !image->d_w || !image->d_h ||
          image->d_w != unsigned(size_.width()) ||
          image->d_h != unsigned(size_.height())) {
        PostFailure(
            kNetworkStateDecodeError,
            "UEFI_MEDIA_PIXELS: unsupported frame dimensions or pixel format");
        return;
      }
      size_t bytes = size_t(image->d_w) * image->d_h * 4;
      if (bytes > kDecodedLimit - decoded_bytes_) {
        PostFailure(kNetworkStateDecodeError,
                    "UEFI_MEDIA_QUOTA: decoded frames exceed 64 MiB");
        return;
      }
      auto frame = media::VideoFrame::CreateFrame(media::PIXEL_FORMAT_ARGB,
                                                  size_, gfx::Rect(size_),
                                                  size_, packet->timestamp());
      if (frame) {
        // Frame storage includes row padding and alignment beyond visible
        // pixels.
        bytes = frame->data_span(0).size() +
                frame->layout().buffer_addr_align() - 1;
        if (bytes > kDecodedLimit - decoded_bytes_) {
          PostFailure(kNetworkStateDecodeError,
                      "UEFI_MEDIA_QUOTA: decoded frames exceed 64 MiB");
          return;
        }
      }
      if (!frame || libyuv::I420ToARGB(image->planes[0], image->stride[0],
                                       image->planes[1], image->stride[1],
                                       image->planes[2], image->stride[2],
                                       frame->writable_data(0),
                                       static_cast<int>(frame->stride(0)),
                                       size_.width(), size_.height())) {
        PostFailure(kNetworkStateDecodeError,
                    "UEFI_MEDIA_PIXELS: frame conversion failed");
        return;
      }
      frame->set_color_space(gfx::ColorSpace::CreateSRGB());
      decoded_bytes_ += bytes;
      frames_.push_back(std::move(frame));
    }
    // Each compressed packet yields back to the renderer event loop before the
    // next decode.
    base::SingleThreadTaskRunner::GetCurrentDefault()->PostTask(
        FROM_HERE, base::BindOnce(&Player::DecodeNext, weak_.GetWeakPtr()));
  }
  void Loaded() {
    if (frames_.empty() || !client_) {
      PostFailure(kNetworkStateDecodeError,
                  "UEFI_MEDIA_EMPTY: decoder produced no frames");
      return;
    }
    std::stable_sort(frames_.begin(), frames_.end(),
                     [](const auto& left, const auto& right) {
                       return left->timestamp() < right->timestamp();
                     });
    double last = frames_.back()->timestamp().InSecondsF();
    if (!std::isfinite(duration_) || duration_ <= last)
      duration_ =
          last +
          (frames_.size() > 1
               ? (last - frames_[frames_.size() - 2]->timestamp().InSecondsF())
               : 1.0 / 30);
    if (duration_ <= 0 || duration_ > 3600) {
      PostFailure(kNetworkStateDecodeError,
                  "UEFI_MEDIA_DURATION: invalid duration");
      return;
    }
    vpx_codec_destroy(&codec_);
    codec_open_ = false;
    parser_.reset();
    packets_.clear();
    loader_.reset();
    anchor_ = base::TimeTicks::Now();
    network_ = kNetworkStateLoaded;
    ready_ = kReadyStateHaveMetadata;
    client_->AddTrack(media::MediaTrack::CreateVideoTrack(
        "video", media::MediaTrack::VideoKind::kMain, "", "", true));
    client_->DidMediaMetadataChange(
        false, true, media::AudioCodec::kUnknown, codec_type_,
        media::MediaContentType::kPersistent, false);
    client_->SizeChanged();
    client_->DurationChanged();
    client_->ReadyStateChanged();
    ready_ = kReadyStateHaveEnoughData;
    client_->ReadyStateChanged();
    client_->NetworkStateChanged();
    client_->OnFirstFrame(base::TimeTicks::Now(), received_bytes_);
    Present(true);
    std::printf("MEDIA: decoded frames=%zu bytes=%llu duration=%.3f codec=%s\n",
                frames_.size(), static_cast<unsigned long long>(decoded_bytes_),
                duration_,
                codec_type_ == media::VideoCodec::kVP8 ? "VP8" : "VP9");
  }
  scoped_refptr<media::VideoFrame> CurrentFrame() const {
    return frames_.empty() ? nullptr : frames_[displayed_];
  }
  void Present(bool force) {
    if (frames_.empty() || !client_)
      return;
    double time = CurrentTime();
    size_t next = 0;
    while (next + 1 < frames_.size() &&
           frames_[next + 1]->timestamp().InSecondsF() <= time)
      ++next;
    if (force || displayed_ != next) {
      if (!force && next > displayed_ + 1)
        dropped_ += static_cast<unsigned>(next - displayed_ - 1);
      displayed_ = next;
      ++presented_;
      presentation_time_ = base::TimeTicks::Now();
      client_->Repaint();
      if (std::exchange(frame_callback_, false))
        client_->OnRequestVideoFrameCallback();
    }
  }
  void FinishSeek() {
    seeking_ = false;
    anchor_ = base::TimeTicks::Now();
    if (HasAudio() && !paused_)
      StartAudio();
    Present(true);
    if (client_)
      client_->TimeChanged();
  }
  void Tick() {
    if (!client_ || !HasMedia())
      return;
    Present(false);
    if (IsEnded()) {
      position_ = duration_;
      if (HasAudio())
        firmware_audio_request(AUDIO_STOP, audio_handle_, 0, 0, 0);
      paused_ = true;
      timer_.Stop();
      client_->TimeChanged();
    }
  }
  long audio_handle_ = 0;
  unsigned audio_rate_ = 48000;
  size_t audio_frames_ = 0, audio_uploaded_ = 0;
  double volume_ = 1, volume_multiplier_ = 1;
  std::vector<int16_t> audio_samples_;
  blink::WebLocalFrame* frame_;
  blink::WeakPersistent<blink::MediaPlayerClient> client_;
  std::unique_ptr<blink::WebAssociatedURLLoader> loader_;
  std::unique_ptr<media::WebMStreamParser> parser_;
  MediaLog media_log_;
  media::PaintCanvasVideoRenderer painter_;
  std::vector<uint8_t> encoded_;
  std::vector<scoped_refptr<media::StreamParserBuffer>> packets_;
  std::vector<scoped_refptr<media::VideoFrame>> frames_;
  vpx_codec_ctx_t codec_{};
  bool codec_open_ = false;
  int video_track_ = -1;
  media::VideoCodec codec_type_ = media::VideoCodec::kUnknown;
  gfx::Size size_;
  GURL source_;
  std::string error_;
  size_t decoded_bytes_ = 0, received_bytes_ = 0, packet_ = 0, displayed_ = 0;
  uint32_t presented_ = 0, dropped_ = 0;
  bool paused_ = true, seeking_ = false, failed_ = false, progress_ = false,
       pause_when_hidden_ = false, frame_callback_ = false;
  double duration_ = std::numeric_limits<double>::quiet_NaN(), position_ = 0,
         rate_ = 1;
  NetworkState network_ = kNetworkStateEmpty;
  ReadyState ready_ = kReadyStateHaveNothing;
  base::TimeTicks anchor_, presentation_time_;
  base::RepeatingTimer timer_;
  base::WeakPtrFactory<Player> weak_{this};
};
}  // namespace
std::unique_ptr<blink::WebMediaPlayer> CreateMediaPlayer(
    blink::WebLocalFrame* frame,
    blink::WebMediaPlayerClient* client) {
  return std::make_unique<Player>(frame, client);
}
}  // namespace uefi_browser

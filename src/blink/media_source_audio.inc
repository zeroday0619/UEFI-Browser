// Chromium retains Opus pre-skip and discard-padding semantics. The firmware
// receives copied stereo PCM and owns all DMA descriptors and sample storage.
  static constexpr double kMseAudioLookAheadSeconds = 2.0;
  void ResetMseAudioBatch() {
    mse_audio_packets_.clear();
    mse_audio_packet_offset_ = 0;
    mse_audio_pumping_ = mse_audio_pump_scheduled_ = false;
    mse_audio_output_.reset();
    mse_audio_output_overflow_ = false;
    mse_audio_enqueued_end_ = false;
    mse_audio_config_pending_ = false;
    mse_audio_queued_end_time_ = 0;
  }
  void CloseMseAudio() {
    mse_audio_decoder_.reset();
    mse_audio_offload_handle_ = 0;
    ResetMseAudioBatch();
    mse_audio_stream_ = nullptr;
    mse_audio_pending_ = mse_audio_ready_ = mse_audio_end_ = false;
    mse_audio_started_ = false;
    mse_audio_pcm_.clear();
    mse_audio_offset_ = 0;
    mse_audio_silence_ = mse_audio_submitted_ = 0;
    mse_audio_origin_ = 0;
  }
  bool ConfigureMseAudio() {
    const auto& config = mse_audio_stream_->audio_decoder_config();
    if (config.is_encrypted() || config.codec() != media::AudioCodec::kOpus ||
        config.channels() < 1 || config.channels() > 2)
      return false;
    if (!HasAudio()) {
      audio_rate_ = 48000;
      audio_handle_ = firmware_audio_request(AUDIO_STREAM_CREATE, 0, 0, 48000, 0);
      if (audio_handle_ <= 0) return false;
      UpdateVolume();
    }
    mse_audio_ready_ = false;
    mse_audio_offload_handle_ = 0;
    mse_audio_decoder_ = std::make_unique<media::OpusAudioDecoder>(
        base::SingleThreadTaskRunner::GetCurrentDefault(),
        media::OpusAudioDecoder::ExecutionMode::kSynchronous);
    mse_audio_decoder_->Initialize(config, nullptr,
        base::BindPostTaskToCurrentDefault(base::BindOnce(
            &Player::MseAudioInitialized, weak_.GetWeakPtr(), mse_generation_)),
        base::BindRepeating(&Player::CollectMseAudioOutput, weak_.GetWeakPtr(), mse_generation_),
        base::BindRepeating([](media::WaitingReason) {}));
    return true;
  }
  void MseAudioInitialized(uint64_t generation, media::DecoderStatus status) {
    if (generation != mse_generation_ || failed_) return;
    if (!status.is_ok()) {
      PostFailure(kNetworkStateDecodeError, "UEFI_MSE_OPUS: decoder initialization failed");
      return;
    }
#if defined(UEFI_RUNTIME)
    if (mse_audio_decoder_->FirmwareHandle() > 0) {
      const long handle = mse_audio_decoder_->FirmwareHandle();
      const long result = firmware_opus_stream_attach(handle, audio_handle_,
          mse_audio_decoder_->FirmwareInitialDiscard(), std::llround(mse_audio_origin_ * 1000000));
      if (result) {
        PostFailure(kNetworkStateDecodeError, "UEFI_MSE_QUEUE: stream attachment failed");
        return;
      }
      mse_audio_offload_handle_ = handle;
      ScheduleMseAudioPump();
    }
#endif
    mse_audio_ready_ = true;
    ReadMseAudio();
  }
  bool StartMseAudio() {
    if (!rate_) {
      // Preserve queued PCM while the media clock is frozen at playback rate zero.
      if (mse_audio_started_) {
        long result = firmware_audio_request(AUDIO_STOP, audio_handle_, 0, 0, 0);
        if (result < 0) {
          PostFailure(kNetworkStateDecodeError, "UEFI_MSE_AUDIO: DMA pause failed");
          return false;
        }
        position_ = mse_audio_origin_ + double(result) / 48000;
        mse_audio_started_ = false;
      }
      return true;
    }
    if (seeking_ || mse_waiting_ || !mse_audio_submitted_) return true;
    long result = firmware_audio_request(AUDIO_START, audio_handle_, 0,
                                         unsigned(std::lround(48000 * rate_)), 0);
    if (result == -EAGAIN) return true;
    if (result < 0) {
      PostFailure(kNetworkStateDecodeError, "UEFI_MSE_AUDIO: DMA start failed");
      return false;
    }
    mse_audio_started_ = true;
    return true;
  }
  double MseAudioTime() const {
    long frame = firmware_audio_request(AUDIO_POSITION, audio_handle_, 0, 0, 0);
    return frame < 0 ? position_ : mse_audio_origin_ + double(frame) / 48000;
  }
  void ReadMseAudio() {
    if (!mse_audio_stream_ || !mse_audio_ready_ || mse_audio_pending_ ||
        mse_audio_config_pending_ || mse_seek_pending_ || failed_ || mse_stopped_ || mse_audio_pumping_) return;
    mse_audio_pumping_ = true;
    const uint64_t generation = mse_generation_;
    auto self = weak_.GetWeakPtr();
    const auto deadline = base::TimeTicks::Now() + base::Milliseconds(4);
    for (;;) {
      if ((!mse_audio_offload_handle_ && (!PumpMseAudio() ||
          double(mse_audio_submitted_) / 48000 + mse_audio_origin_ > CurrentTime() + kMseAudioLookAheadSeconds)) ||
          mse_audio_end_ || (mse_audio_offload_handle_ && mse_audio_enqueued_end_))
        break;
      if (mse_audio_packet_offset_ == mse_audio_packets_.size()) {
        mse_audio_packets_.clear();
        mse_audio_packet_offset_ = 0;
        mse_audio_pending_ = true;
        // ChunkDemuxerStream posts completion after releasing its lock.
        mse_audio_stream_->Read(kMseReadBatch, base::BindOnce(
            &Player::MseAudioRead, weak_.GetWeakPtr(), generation));
        break;
      }
      if (mse_audio_offload_handle_) {
        const auto& packet = mse_audio_packets_[mse_audio_packet_offset_];
        long result = EnqueueMseAudio(packet);
        if (result == -EAGAIN) { ScheduleMseAudioPump(); break; }
        if (result) {
          PostFailure(kNetworkStateDecodeError, "UEFI_MSE_QUEUE: compressed submission failed");
          break;
        }
        ++mse_audio_packet_offset_;
      } else {
        auto packet = std::move(mse_audio_packets_[mse_audio_packet_offset_++]);
        DecodeMseAudioPacket(generation, std::move(packet));
      }
      if (!self || self->mse_generation_ != generation) return;
      if (failed_ || mse_stopped_ || mse_audio_end_) break;
      if (base::TimeTicks::Now() >= deadline) {
        ScheduleMseAudioPump();
        break;
      }
    }
    mse_audio_pumping_ = false;
  }
  void ScheduleMseAudioPump() {
    if (mse_audio_pump_scheduled_) return;
    mse_audio_pump_scheduled_ = true;
    base::SingleThreadTaskRunner::GetCurrentDefault()->PostDelayedTask(
        FROM_HERE, base::BindOnce(&Player::ContinueMseAudio,
                                  weak_.GetWeakPtr(), mse_generation_),
        mse_audio_offload_handle_ ? base::Milliseconds(16) : base::TimeDelta());
  }
  void ContinueMseAudio(uint64_t generation) {
    if (generation != mse_generation_) return;
    mse_audio_pump_scheduled_ = false;
    auto self = weak_.GetWeakPtr();
    if (mse_audio_offload_handle_) {
      FirmwareOpusStreamStatus status{};
      if (firmware_opus_stream_status(mse_audio_offload_handle_, &status) || status.error) {
        PostFailure(kNetworkStateDecodeError, "UEFI_MSE_QUEUE: asynchronous decoding failed");
        return;
      }
      mse_audio_submitted_ = status.submitted;
      mse_audio_end_ = status.ended;
      if (mse_audio_config_pending_ && !status.queued && !status.pending_frames) {
        // Closing a decoder cancels its queue, so configuration changes must
        // wait until every old packet and PCM fragment reaches the device.
        mse_audio_config_pending_ = false;
        std::printf("MSE: audio configuration drained packets=%llu submitted_frames=%llu\n",
            static_cast<unsigned long long>(status.decoded),
            static_cast<unsigned long long>(status.submitted));
        if (!ConfigureMseAudio())
          PostFailure(kNetworkStateDecodeError, "UEFI_MSE_OPUS: configuration rejected");
        return;
      }
      MseMaybeReady();
      if (!self || self->mse_generation_ != generation) return;
    }
    ReadMseAudio();
    if (self && self->mse_generation_ == generation && self->mse_audio_offload_handle_ &&
        !self->failed_ && !self->mse_stopped_) self->ScheduleMseAudioPump();
  }
  long EnqueueMseAudio(const scoped_refptr<media::DecoderBuffer>& packet) {
    FirmwareOpusPacket message{};
    message.end = packet->end_of_stream();
    if (!message.end) {
      if (packet->is_encrypted()) return -EINVAL;
      if (packet->size() > FIRMWARE_OPUS_PACKET_LIMIT || packet->timestamp().is_inf() ||
          packet->timestamp() == media::kNoTimestamp || packet->duration().is_inf() ||
          packet->duration() < base::TimeDelta()) return -EINVAL;
      message.bytes = packet->size();
      message.timestamp = packet->timestamp().InMicroseconds();
      message.duration = packet->duration().InMicroseconds();
      if (packet->discard_padding()) {
        message.padding = 1;
        auto [front, back] = *packet->discard_padding();
        message.discard_all = front == media::kInfiniteDuration;
        if ((!message.discard_all && front.is_inf()) || back.is_inf()) return -EINVAL;
        message.discard_front = message.discard_all ? 0 : front.InMicroseconds();
        message.discard_back = back.InMicroseconds();
      }
    }
    long result = firmware_opus_stream_enqueue(mse_audio_offload_handle_, &message,
        message.bytes ? base::span(*packet).data() : nullptr);
    if (!result) {
      mse_audio_enqueued_end_ = message.end;
      if (!message.end) mse_audio_queued_end_time_ = double(message.timestamp + message.duration) / 1000000;
    }
    return result;
  }
  void MseAudioRead(uint64_t generation, media::DemuxerStream::Status status,
                    std::vector<scoped_refptr<media::DecoderBuffer>> buffers) {
    if (generation != mse_generation_ || failed_ || !mse_audio_stream_) return;
    mse_audio_pending_ = false;
    if (status == media::DemuxerStream::kConfigChanged) {
      if (mse_audio_offload_handle_) {
        mse_audio_config_pending_ = true;
        ScheduleMseAudioPump();
        return;
      }
      if (!ConfigureMseAudio())
        PostFailure(kNetworkStateDecodeError, "UEFI_MSE_OPUS: configuration rejected");
      return;
    }
    if (status == media::DemuxerStream::kAborted) return;
    if (status != media::DemuxerStream::kOk || !ValidMseBatch(buffers) ||
        mse_audio_packet_offset_ != mse_audio_packets_.size()) {
      PostFailure(kNetworkStateDecodeError, "UEFI_MSE_OPUS: compressed read failed");
      return;
    }
    if (buffers.size() > mse_audio_maximum_batch_) {
      mse_audio_maximum_batch_ = buffers.size();
      std::printf("MSE: audio batch packets=%zu\n", mse_audio_maximum_batch_);
    }
    mse_audio_packets_ = std::move(buffers);
    mse_audio_packet_offset_ = 0;
    ReadMseAudio();
  }
  void DecodeMseAudioPacket(uint64_t generation,
                            scoped_refptr<media::DecoderBuffer> packet) {
    const bool end = packet->end_of_stream();
    mse_audio_pending_ = true;
    mse_audio_output_.reset();
    mse_audio_output_overflow_ = false;
    media::DecoderStatus decoded_status = media::DecoderStatus::Codes::kFailed;
    // Synchronous Opus callbacks only collect results. Blink notifications run
    // after Decode returns so reentrant seek or teardown cannot destroy an
    // active decoder, and each packet avoids two renderer-queue round trips.
    mse_audio_decoder_->Decode(std::move(packet), base::BindOnce(
        [](media::DecoderStatus* result, media::DecoderStatus status) {
          *result = std::move(status);
        }, base::Unretained(&decoded_status)));
    auto output = std::move(mse_audio_output_);
    if (mse_audio_output_overflow_)
      decoded_status = media::DecoderStatus::Codes::kFailed;
    auto self = weak_.GetWeakPtr();
    if (decoded_status.is_ok() && output)
      MseAudioOutput(generation, std::move(output));
    if (self)
      self->MseAudioDecoded(generation, end, std::move(decoded_status));
  }
  void CollectMseAudioOutput(uint64_t generation,
                            scoped_refptr<media::AudioBuffer> buffer) {
    if (generation != mse_generation_ || failed_) return;
    // Opus emits at most one buffer per packet; retain a bounded result.
    if (!buffer || mse_audio_output_) mse_audio_output_overflow_ = true;
    else mse_audio_output_ = std::move(buffer);
  }
  void MseAudioDecoded(uint64_t generation, bool end, media::DecoderStatus status) {
    if (generation != mse_generation_ || failed_) return;
    mse_audio_pending_ = false;
    if (!status.is_ok()) {
      PostFailure(kNetworkStateDecodeError, "UEFI_MSE_OPUS: packet decoding failed");
      return;
    }
    mse_audio_end_ = end;
    MseMaybeReady();
  }
  void MseAudioOutput(uint64_t generation, scoped_refptr<media::AudioBuffer> buffer) {
    if (generation != mse_generation_ || failed_) return;
    if (buffer->sample_format() != media::kSampleFormatF32 ||
        buffer->sample_rate() != 48000 || buffer->channel_count() < 1 ||
        buffer->channel_count() > 2 || buffer->frame_count() > 5760 ||
        buffer->timestamp().is_inf() || buffer->timestamp() == media::kNoTimestamp) {
      PostFailure(kNetworkStateDecodeError, "UEFI_MSE_OPUS: PCM format rejected");
      return;
    }
    const double relative = buffer->timestamp().InSecondsF() - mse_audio_origin_;
    if (!std::isfinite(relative) || std::abs(relative) > 86400) {
      PostFailure(kNetworkStateDecodeError, "UEFI_MSE_OPUS: timestamp rejected");
      return;
    }
    int64_t first = std::llround(relative * 48000);
    const int64_t skip = std::max<int64_t>(0, int64_t(mse_audio_submitted_) - first);
    if (skip >= buffer->frame_count()) return;
    if (!mse_audio_pcm_.empty() || mse_audio_silence_) {
      PostFailure(kNetworkStateDecodeError, "UEFI_MSE_OPUS: output backpressure violated");
      return;
    }
    if (first > int64_t(mse_audio_submitted_))
      mse_audio_silence_ = first - mse_audio_submitted_;
    auto samples = buffer->interleaved_data_cast<float>();
    const unsigned channels = buffer->channel_count();
    mse_audio_pcm_.reserve((buffer->frame_count() - skip) * 2);
    for (int64_t frame = skip; frame < buffer->frame_count(); ++frame)
      for (unsigned channel = 0; channel < 2; ++channel) {
        float value = samples[frame * channels + (channels == 1 ? 0 : channel)];
        if (!std::isfinite(value)) value = 0;
        mse_audio_pcm_.push_back(int16_t(std::lround(std::clamp(value, -1.0f, 1.0f) * 32767)));
      }
    PumpMseAudio();
    MseMaybeReady();
  }
  bool PumpMseAudio() {
    if (mse_audio_pcm_.empty() && !mse_audio_silence_) return true;
    if (double(mse_audio_submitted_) / 48000 + mse_audio_origin_ > CurrentTime() + kMseAudioLookAheadSeconds)
      return false;
    int16_t silence[8192]{};
    size_t frames = mse_audio_silence_ ? std::min<uint64_t>(4096, mse_audio_silence_)
        : std::min<size_t>(4096, (mse_audio_pcm_.size() - mse_audio_offset_) / 2);
    const int16_t* samples = mse_audio_silence_ ? silence
        : mse_audio_pcm_.data() + mse_audio_offset_;
    long result = firmware_audio_request(AUDIO_STREAM_WRITE, audio_handle_,
        mse_audio_submitted_, reinterpret_cast<uintptr_t>(samples), frames * 4);
    if (result == -EAGAIN) return false;
    if (result != long(frames * 4)) {
      PostFailure(kNetworkStateDecodeError, "UEFI_MSE_AUDIO: PCM submission failed");
      return false;
    }
    mse_audio_submitted_ += frames;
    if (mse_audio_silence_) mse_audio_silence_ -= frames;
    else {
      mse_audio_offset_ += frames * 2;
      if (mse_audio_offset_ == mse_audio_pcm_.size()) {
        mse_audio_pcm_.clear();
        mse_audio_offset_ = 0;
      }
    }
    if (!paused_ && !seeking_ && !mse_waiting_ && !mse_audio_started_)
      StartMseAudio();
    if (!mse_audio_pcm_.empty() || mse_audio_silence_) {
      ScheduleMseAudioPump();
      return false;
    }
    return true;
  }
  long mse_audio_offload_handle_ = 0;
  bool mse_audio_enqueued_end_ = false, mse_audio_config_pending_ = false;
  double mse_audio_queued_end_time_ = 0;
  std::unique_ptr<media::OpusAudioDecoder> mse_audio_decoder_;
  std::vector<scoped_refptr<media::DecoderBuffer>> mse_audio_packets_;
  size_t mse_audio_packet_offset_ = 0, mse_audio_maximum_batch_ = 0;
  bool mse_audio_pumping_ = false, mse_audio_pump_scheduled_ = false;
  scoped_refptr<media::AudioBuffer> mse_audio_output_;
  bool mse_audio_output_overflow_ = false;
  media::DemuxerStream* mse_audio_stream_ = nullptr;
  std::vector<int16_t> mse_audio_pcm_;
  size_t mse_audio_offset_ = 0;
  uint64_t mse_audio_submitted_ = 0, mse_audio_silence_ = 0;
  double mse_audio_origin_ = 0;
  bool mse_audio_pending_ = false, mse_audio_ready_ = false;
  bool mse_audio_end_ = false, mse_audio_started_ = false;

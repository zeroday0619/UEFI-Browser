// These members share Player's frame presentation and decoder implementation.
// Host callbacks remain posted because Blink notifications can reenter the
// demuxer. ChunkDemuxerStream::Read already posts its completion after unlocking.
  static constexpr uint32_t kMseReadBatch = 8;
  static bool ValidMseBatch(
      const std::vector<scoped_refptr<media::DecoderBuffer>>& buffers) {
    if (buffers.empty() || buffers.size() > kMseReadBatch) return false;
    for (size_t index = 0; index < buffers.size(); ++index)
      if (!buffers[index] ||
          (buffers[index]->end_of_stream() && index + 1 != buffers.size()))
        return false;
    return true;
  }
  void ResetMseVideoBatch() {
    mse_packets_.clear();
    mse_packet_offset_ = 0;
    mse_pumping_ = mse_pump_scheduled_ = false;
  }
  void CloseMse() {
    ++mse_generation_;
    CloseMseAudio();
    if (mse_demuxer_ && !mse_stopped_)
      mse_demuxer_->Stop();
    mse_demuxer_.reset();
    mse_stream_ = nullptr;
    ResetMseVideoBatch();
    mse_ranges_.clear();
    mse_video_ = mse_waiting_ = mse_end_ = mse_stopped_ = false;
    mse_read_pending_ = mse_seek_pending_ = mse_first_frame_ = false;
    mse_decoded_end_ = 0;
    mse_frame_duration_ = 1.0 / 30;
  }
  void StartMse() {
    network_ = kNetworkStateLoading;
    mse_waiting_ = true;
    mse_demuxer_ = std::make_unique<media::ChunkDemuxer>(
        base::BindPostTaskToCurrentDefault(
            base::BindOnce(&Player::OpenMse, weak_.GetWeakPtr())),
        base::BindPostTaskToCurrentDefault(
            base::BindRepeating(&Player::MseProgress, weak_.GetWeakPtr())),
        base::BindPostTaskToCurrentDefault(base::BindRepeating(
            [](base::WeakPtr<Player> self, media::EmeInitDataType,
               const std::vector<uint8_t>&) {
              if (self)
                self->PostFailure(kNetworkStateFormatError,
                    "UEFI_MEDIA_ENCRYPTED: encrypted media is unsupported");
            }, weak_.GetWeakPtr())), &media_log_);
    mse_demuxer_->Initialize(this, base::BindPostTaskToCurrentDefault(
        base::BindOnce(&Player::InitializeMse, weak_.GetWeakPtr())));
  }
  void OpenMse() {
    if (client_ && mse_demuxer_ && !failed_)
      client_->MediaSourceOpened(
          std::make_unique<blink::WebMediaSourceImpl>(mse_demuxer_.get()));
  }
  void InitializeMse(media::PipelineStatus status) {
    if (failed_ || !client_ || !mse_demuxer_)
      return;
    if (status != media::PIPELINE_OK) {
      PostFailure(kNetworkStateFormatError,
                  "UEFI_MSE_INIT: source initialization failed");
      return;
    }
    auto streams = mse_demuxer_->GetAllStreams();
    bool valid = !streams.empty();
    for (auto stream : streams) {
      if (stream->type() == media::DemuxerStream::VIDEO && !mse_stream_)
        mse_stream_ = stream;
      else if (stream->type() == media::DemuxerStream::AUDIO && !mse_audio_stream_)
        mse_audio_stream_ = stream;
      else valid = false;
    }
    if (!valid || (mse_stream_ && !ConfigureVideo(mse_stream_->video_decoder_config())) ||
        (mse_audio_stream_ && !ConfigureMseAudio())) {
      PostFailure(kNetworkStateFormatError,
                  "UEFI_MSE_CODEC: expected clear VP8/VP9 video and mono/stereo Opus");
      return;
    }
    mse_video_ = mse_stream_ != nullptr;
    duration_ = mse_demuxer_->GetDuration();
    ready_ = kReadyStateHaveMetadata;
    client_->DidMediaMetadataChange(mse_audio_stream_ != nullptr, mse_video_,
        mse_audio_stream_ ? media::AudioCodec::kOpus : media::AudioCodec::kUnknown,
        codec_type_, media::MediaContentType::kPersistent, false);
    if (mse_video_) client_->SizeChanged();
    client_->DurationChanged();
    client_->ReadyStateChanged();
    ReadMse();
    std::printf("MSE: source initialized video=%s audio=%s\n",
                !mse_video_ ? "none" : codec_type_ == media::VideoCodec::kVP8 ? "VP8" : "VP9",
                mse_audio_stream_ ? "Opus" : "none");
  }
  void OnBufferedTimeRangesChanged(
      const media::Ranges<base::TimeDelta>& ranges) override {
    mse_ranges_ = ranges;
    base::SingleThreadTaskRunner::GetCurrentDefault()->PostTask(
        FROM_HERE, base::BindOnce(&Player::MseProgress, weak_.GetWeakPtr()));
  }
  void SetDuration(base::TimeDelta duration) override {
    duration_ = duration == media::kInfiniteDuration
                    ? std::numeric_limits<double>::infinity()
                    : duration.InSecondsF();
    base::SingleThreadTaskRunner::GetCurrentDefault()->PostTask(
        FROM_HERE, base::BindOnce(&Player::MseDurationChanged,
                                  weak_.GetWeakPtr()));
  }
  void MseDurationChanged() {
    if (client_ && HasMedia() && !failed_)
      client_->DurationChanged();
  }
  void OnDemuxerError(media::PipelineStatus) override {
    PostFailure(kNetworkStateDecodeError,
                "UEFI_MSE_DEMUX: source parsing or buffering failed");
  }
  void MseProgress() {
    if (!mse_demuxer_ || failed_ || mse_stopped_)
      return;
    progress_ = true;
    // An append after endOfStream reopens the upstream stream.
    if (mse_end_ && !mse_ranges_.empty() &&
        mse_ranges_.back().second.InSecondsF() > mse_decoded_end_ + 0.001)
      mse_end_ = false;
    if (mse_audio_end_ && !mse_ranges_.empty() &&
        mse_ranges_.back().second.InSecondsF() >
            mse_audio_origin_ + double(mse_audio_submitted_) / 48000 + .001)
      mse_audio_end_ = false;
    if (mse_audio_enqueued_end_ && !mse_ranges_.empty() &&
        mse_ranges_.back().second.InSecondsF() > mse_audio_queued_end_time_ + .001)
      mse_audio_enqueued_end_ = false;
    auto self = weak_.GetWeakPtr();
    const uint64_t generation = mse_generation_;
    ReadMse();
    if (self && self->mse_generation_ == generation) self->ReadMseAudio();
  }
  void PruneMseFrames() {
    if (frames_.size() < 2)
      return;
    const double time = CurrentTime();
    size_t count = 0;
    while (count + 1 < frames_.size() &&
           frames_[count + 1]->timestamp().InSecondsF() <= time)
      ++count;
    if (!seeking_)
      count = std::min(count, displayed_);
    if (!count)
      return;
    // Retain the displayed frame and all future frames; the painter may hold a
    // reference to the previous frame until the next presentation.
    for (size_t index = 0; index < count; ++index)
      decoded_bytes_ -= frames_[index]->data_span(0).size() +
                        frames_[index]->layout().buffer_addr_align() - 1;
    frames_.erase(frames_.begin(), frames_.begin() + count);
    displayed_ = displayed_ >= count ? displayed_ - count : 0;
  }
  void ReadMse() {
    if (!mse_stream_ || failed_ || mse_stopped_ || mse_read_pending_ ||
        mse_seek_pending_ || mse_end_ || mse_pumping_)
      return;
    mse_pumping_ = true;
    const uint64_t generation = mse_generation_;
    auto self = weak_.GetWeakPtr();
    const auto deadline = base::TimeTicks::Now() + base::Milliseconds(4);
    for (;;) {
      PruneMseFrames();
      // Each packet observes the existing decoded-memory and look-ahead limits.
      if (!seeking_ && !frames_.empty() &&
          (mse_decoded_end_ > CurrentTime() + 0.5 ||
           decoded_bytes_ + size_t(size_.GetArea()) * 4 + 4096 > kDecodedLimit))
        break;
      if (mse_packet_offset_ == mse_packets_.size()) {
        mse_packets_.clear();
        mse_packet_offset_ = 0;
        mse_read_pending_ = true;
        mse_stream_->Read(kMseReadBatch, base::BindOnce(
            &Player::MseRead, weak_.GetWeakPtr(), generation));
        break;
      }
      auto buffer = std::move(mse_packets_[mse_packet_offset_++]);
      if (buffer->end_of_stream()) {
        mse_end_ = true;
      } else {
        if (buffer->decrypt_config() || buffer->timestamp().is_inf() ||
            buffer->timestamp().is_negative()) {
          PostFailure(kNetworkStateDecodeError,
                      "UEFI_MSE_PACKET: invalid timestamp or encrypted buffer");
          break;
        }
        received_bytes_ += buffer->size();
        if (!DecodePacket(buffer)) break;
        if (!frames_.empty()) {
          const double duration = buffer->duration() == media::kNoTimestamp ||
                                  buffer->duration().is_inf()
                                      ? 1.0 / 30 : buffer->duration().InSecondsF();
          mse_frame_duration_ = std::max(0.001, duration);
          mse_decoded_end_ = std::max(mse_decoded_end_,
              frames_.back()->timestamp().InSecondsF() + std::max(0.001, duration));
        }
      }
      MseMaybeReady();
      if (!self || self->mse_generation_ != generation) return;
      if (failed_ || mse_stopped_ || mse_end_) break;
      // A codec call is indivisible; yield before starting another packet once
      // the time budget expires instead of extending one task for the batch.
      if (base::TimeTicks::Now() >= deadline) {
        ScheduleMseVideoPump();
        break;
      }
    }
    mse_pumping_ = false;
  }
  void ScheduleMseVideoPump() {
    if (mse_pump_scheduled_) return;
    mse_pump_scheduled_ = true;
    base::SingleThreadTaskRunner::GetCurrentDefault()->PostTask(
        FROM_HERE, base::BindOnce(&Player::ContinueMseVideo,
                                  weak_.GetWeakPtr(), mse_generation_));
  }
  void ContinueMseVideo(uint64_t generation) {
    if (generation != mse_generation_) return;
    mse_pump_scheduled_ = false;
    ReadMse();
  }
  void MseRead(uint64_t generation, media::DemuxerStream::Status status,
               std::vector<scoped_refptr<media::DecoderBuffer>> buffers) {
    if (generation != mse_generation_ || failed_ || !mse_stream_)
      return;
    mse_read_pending_ = false;
    if (status == media::DemuxerStream::kConfigChanged) {
      if (!ConfigureVideo(mse_stream_->video_decoder_config())) {
        PostFailure(kNetworkStateDecodeError,
                    "UEFI_MSE_CONFIG: decoder configuration rejected");
        return;
      }
      auto self = weak_.GetWeakPtr();
      client_->SizeChanged();
      if (self && self->mse_generation_ == generation) self->ReadMse();
      return;
    }
    if (status == media::DemuxerStream::kAborted)
      return;
    if (status != media::DemuxerStream::kOk || !ValidMseBatch(buffers) ||
        mse_packet_offset_ != mse_packets_.size()) {
      PostFailure(kNetworkStateDecodeError,
                  "UEFI_MSE_READ: compressed stream read failed");
      return;
    }
    if (buffers.size() > mse_maximum_batch_) {
      mse_maximum_batch_ = buffers.size();
      std::printf("MSE: video batch packets=%zu\n", mse_maximum_batch_);
    }
    mse_packets_ = std::move(buffers);
    mse_packet_offset_ = 0;
    ReadMse();
  }
  void MseMaybeReady() {
    const bool video_ready = !mse_video_ ||
        (!frames_.empty() && (mse_end_ || mse_decoded_end_ > position_));
    const bool audio_ready = !mse_audio_stream_ || mse_audio_submitted_ > 0 || mse_audio_end_;
    if (!video_ready || !audio_ready || failed_) return;
    auto self = weak_.GetWeakPtr();
    const uint64_t generation = mse_generation_;
    if (mse_waiting_) {
      anchor_ = base::TimeTicks::Now();
      mse_waiting_ = false;
    }
    if (seeking_) FinishSeek();
    if (!self || self->mse_generation_ != generation) return;
    if (!mse_first_frame_ && !frames_.empty()) {
      mse_first_frame_ = true;
      client_->OnFirstFrame(base::TimeTicks::Now(), received_bytes_);
      if (!self || self->mse_generation_ != generation) return;
      Present(true);
      if (!self || self->mse_generation_ != generation) return;
    }
    const double current = CurrentTime();
    const bool video_ahead = !mse_video_ || mse_end_ || mse_decoded_end_ > current + .15;
    const bool audio_ahead = !mse_audio_stream_ || mse_audio_end_ ||
        mse_audio_origin_ + double(mse_audio_submitted_) / 48000 > current + .15;
    SetMseReady(video_ahead && audio_ahead ? kReadyStateHaveEnoughData : kReadyStateHaveFutureData);
    if (!self || self->mse_generation_ != generation) return;
    if (mse_audio_stream_ && !paused_ && !mse_audio_started_) StartMseAudio();
  }
  void SetMseReady(ReadyState state) {
    if (ready_ != state && client_) {
      ready_ = state;
      client_->ReadyStateChanged();
    }
  }
  void SeekMse(double seconds) {
    if ((!mse_stream_ && !mse_audio_stream_) || failed_ || mse_stopped_)
      return;
    position_ = std::clamp(seconds, 0.0, duration_);
    auto time = base::Seconds(position_);
    // Coded ranges exclude their end. Decode the final frame when seeking to
    // the exact duration while retaining the requested presentation time.
    if (std::isfinite(duration_) && duration_ > 0 && position_ == duration_)
      time = std::max(base::TimeDelta(), time - base::Microseconds(1));
    ++mse_generation_;
    ResetMseVideoBatch();
    ResetMseAudioBatch();
    if (mse_seek_pending_)
      mse_demuxer_->CancelPendingSeek(time);
    mse_seek_pending_ = true;
    mse_read_pending_ = false;
    mse_waiting_ = seeking_ = true;
    mse_end_ = false;
    mse_decoded_end_ = 0;
    if (mse_audio_stream_) {
      // Cancel compressed work before resetting the device's submission sequence.
      mse_audio_decoder_.reset();
      mse_audio_offload_handle_ = 0;
      firmware_audio_request(AUDIO_STREAM_RESET, audio_handle_, 0, 0, 0);
      mse_audio_ready_ = mse_audio_pending_ = mse_audio_end_ = mse_audio_started_ = false;
      mse_audio_submitted_ = mse_audio_silence_ = mse_audio_offset_ = 0;
      mse_audio_pcm_.clear();
      mse_audio_origin_ = position_;
    }
    painter_.ResetCache();
    frames_.clear();
    decoded_bytes_ = displayed_ = 0;
    mse_demuxer_->StartWaitingForSeek(time);
    mse_demuxer_->Seek(time, base::BindPostTaskToCurrentDefault(base::BindOnce(
        &Player::MseSeekComplete, weak_.GetWeakPtr(), mse_generation_)));
  }
  void MseSeekComplete(uint64_t generation, media::PipelineStatus status) {
    if (generation != mse_generation_ || failed_)
      return;
    mse_seek_pending_ = false;
    if (status != media::PIPELINE_OK ||
        (mse_stream_ && !ConfigureVideo(mse_stream_->video_decoder_config())) ||
        (mse_audio_stream_ && !ConfigureMseAudio())) {
      PostFailure(kNetworkStateDecodeError,
                  "UEFI_MSE_SEEK: stream seek failed");
      return;
    }
    ReadMse();
  }
  void TickMse() {
    auto self = weak_.GetWeakPtr();
    const uint64_t generation = mse_generation_;
    auto tick_time = base::TimeTicks::Now();
    if (!mse_last_tick_.is_null())
      mse_maximum_tick_gap_us_ = std::max(mse_maximum_tick_gap_us_,
          (tick_time-mse_last_tick_).InMicroseconds());
    mse_last_tick_ = tick_time;
    if (mse_last_report_.is_null() || tick_time-mse_last_report_ >= base::Seconds(5)) {
      const double current = CurrentTime();
      std::printf("PERF: media player=%p clock_ms=%lld time_ms=%lld video_ahead_ms=%lld audio_ahead_ms=%lld decoded=%u presented=%u codec_us=%llu pixels_us=%llu tick_gap_us=%lld video_waits=%llu waiting=%u ready=%d size=%dx%d\n",
          static_cast<void*>(this),
          static_cast<long long>(tick_time.since_origin().InMilliseconds()),
          static_cast<long long>(current*1000),
          static_cast<long long>((mse_decoded_end_-current)*1000),
          static_cast<long long>((mse_audio_origin_+double(mse_audio_submitted_)/48000-current)*1000),
          total_decoded_, presented_, static_cast<unsigned long long>(decode_time_us_),
          static_cast<unsigned long long>(pixel_time_us_),
          static_cast<long long>(mse_maximum_tick_gap_us_),
          static_cast<unsigned long long>(mse_video_waits_),
          unsigned(mse_waiting_), int(ready_), size_.width(), size_.height());
      mse_maximum_tick_gap_us_ = 0;
      mse_last_report_ = tick_time;
    }
    Present(false);
    if (!self || self->mse_generation_ != generation) return;
    if (mse_audio_stream_ && mse_audio_end_ && (!mse_video_ || mse_end_) &&
        MseAudioTime() >= mse_audio_origin_ + double(mse_audio_submitted_) / 48000 &&
        std::isfinite(duration_) && duration_ - MseAudioTime() < .1) {
      position_ = duration_;
      mse_waiting_ = true;
    }
    if (IsEnded()) {
      position_ = duration_;
      if (mse_audio_stream_) {
        firmware_audio_request(AUDIO_STOP, audio_handle_, 0, 0, 0);
        mse_audio_started_ = false;
      }
      paused_ = true;
      timer_.Stop();
      client_->TimeChanged();
      return;
    }
    if (mse_video_ && !seeking_ && !mse_waiting_ && !mse_end_ &&
        CurrentTime() >= mse_decoded_end_) {
      ++mse_video_waits_;
      position_ = CurrentTime();
      mse_waiting_ = true;
      if (mse_audio_stream_) {
        firmware_audio_request(AUDIO_STOP, audio_handle_, 0, 0, 0);
        mse_audio_started_ = false;
      }
      SetMseReady(kReadyStateHaveCurrentData);
      if (!self || self->mse_generation_ != generation) return;
    }
    ReadMse();
    if (!self || self->mse_generation_ != generation) return;
    ReadMseAudio();
    if (!self || self->mse_generation_ != generation) return;
    MseMaybeReady();
  }
  std::unique_ptr<media::ChunkDemuxer> mse_demuxer_;
  std::vector<scoped_refptr<media::DecoderBuffer>> mse_packets_;
  size_t mse_packet_offset_ = 0, mse_maximum_batch_ = 0;
  bool mse_pumping_ = false, mse_pump_scheduled_ = false;
  media::DemuxerStream* mse_stream_ = nullptr;
  media::Ranges<base::TimeDelta> mse_ranges_;
  uint64_t mse_generation_ = 0;
  double mse_decoded_end_ = 0, mse_frame_duration_ = 1.0 / 30;
  base::TimeTicks mse_last_tick_, mse_last_report_;
  int64_t mse_maximum_tick_gap_us_ = 0;
  uint64_t mse_video_waits_ = 0;
  bool mse_video_ = false, mse_waiting_ = false, mse_end_ = false;
  bool mse_read_pending_ = false, mse_seek_pending_ = false;
  bool mse_first_frame_ = false, mse_stopped_ = false;

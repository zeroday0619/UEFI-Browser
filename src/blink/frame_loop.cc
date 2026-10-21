// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
#include "blink/frame_loop.h"
#include "blink/scroll_scene.h"
#include "blink/scroll_wire.h"
#include <utility>
#include <cstdio>
#include <algorithm>
#include "base/check.h"
#include "base/functional/bind.h"
#include "base/location.h"
#include "base/time/time.h"
#include "cc/paint/skia_paint_canvas.h"
#include "cc/paint/paint_recorder.h"
#include "third_party/skia/include/core/SkCanvas.h"
#include "third_party/skia/include/core/SkImage.h"
#include "third_party/blink/public/common/metrics/document_update_reason.h"
#include "third_party/blink/public/web/web_view.h"
#include "third_party/blink/public/web/web_frame_widget.h"
#include "renderer_runtime.h"
namespace uefi_browser {
FrameLoop::FrameLoop(blink::WebView& view, blink::WebFrameWidget& widget,
                     SkCanvas& canvas, gfx::Rect viewport,
                     scoped_refptr<base::SingleThreadTaskRunner> runner,
                     base::RepeatingClosure present)
    : view_(view), widget_(widget), canvas_(canvas), viewport_(viewport),
      runner_(std::move(runner)), present_(std::move(present)) {
  CHECK(runner_);
  CHECK(present_);
  CHECK(!viewport_.IsEmpty());
}
FrameLoop::~FrameLoop() {
  std::printf("PERF: maximum_raster_slice_us=%lld\n", static_cast<long long>(maximum_raster_us_));
  std::printf("PERF: maximum_layout_slice_us=%lld\n", static_cast<long long>(maximum_layout_us_));
  Stop();
}
bool FrameLoop::Start() {
  CHECK(runner_->RunsTasksInCurrentSequence());
  if (recording_reflows_) return true;
  requested_ = true;
  if (!visible_) return true;
  if (running_) return true;
  auto delay = std::max(base::TimeDelta(), last_tick_ + base::Milliseconds(16) - base::TimeTicks::Now());
  running_ = runner_->PostDelayedTask(
      FROM_HERE, base::BindOnce(&FrameLoop::Tick, weak_factory_.GetWeakPtr()), delay);
  return running_;
}
void FrameLoop::Stop() {
  CHECK(runner_->RunsTasksInCurrentSequence());
  running_ = false;
  weak_factory_.InvalidateWeakPtrs();
  picture_.reset();
  staging_.reset();
  scroll_capture_.reset();
  wire_encoder_.reset();
  pending_scroll_ = {};
}
void FrameLoop::SetVisible(bool visible) {
  if (visible_ == visible) return;
  visible_ = visible;
  if (visible) Start();
  else Stop();
}
void FrameLoop::Tick() {
  CHECK(runner_->RunsTasksInCurrentSequence());
  if (!running_) return;
  requested_ = false;
  auto alive = weak_factory_.GetWeakPtr();
  auto now = base::TimeTicks::Now();
  last_tick_ = now;
#if defined(UEFI_RUNTIME)
  widget_.BeginNonCompositedFrame(now, ++frame_sequence_);
#endif
  // JavaScript animation callbacks and lifecycle updates may close the frame.
  if (!alive || !running_) return;
  // Returning to the pump permits native input after animation callbacks.
  running_ = runner_->PostTask(FROM_HERE,
      base::BindOnce(&FrameLoop::Layout, alive));
}
void FrameLoop::Layout() {
  if (!running_) return;
  auto alive = weak_factory_.GetWeakPtr();
  auto started = base::TimeTicks::Now();
  widget_.UpdateLifecycle(blink::WebLifecycleUpdate::kLayout, blink::DocumentUpdateReason::kBeginMainFrame);
  if (!alive || !running_) return;
  maximum_layout_us_ = std::max(maximum_layout_us_, (base::TimeTicks::Now()-started).InMicroseconds());
  // Input may run between layout and paint; Record revalidates every lifecycle phase.
  running_ = runner_->PostTask(FROM_HERE, base::BindOnce(&FrameLoop::Record, alive));
}
void FrameLoop::Record() {
  if (!running_) return;
  auto alive = weak_factory_.GetWeakPtr();
  auto started = base::TimeTicks::Now();
  widget_.UpdateAllLifecyclePhases(blink::DocumentUpdateReason::kBeginMainFrame);
  if (!alive || !running_) return;
  auto lifecycle_end = base::TimeTicks::Now();
  ViewportSnapshot viewport{viewport_};
  RecordScroll(viewport);
  if (!alive || !running_) return;
  picture_ = std::move(viewport.picture);
  if (!picture_) {
    const auto paint_started = base::TimeTicks::Now();
    cc::PaintRecorder recorder;
    view_.PaintContent(recorder.beginRecording(), viewport_);
    if (!alive || !running_) return;
    picture_ = recorder.finishRecordingAsPicture();
    viewport.paint_microseconds += (base::TimeTicks::Now()-paint_started).InMicroseconds();
  }
  auto scroll_end = base::TimeTicks::Now();
  if (!staging_) {
    staging_ = SkSurfaces::Raster(SkImageInfo::MakeN32Premul(
        viewport_.width(), viewport_.height()));
    CHECK(staging_);
  }
  raster_y_ = 0;
  raster_total_us_ = 0;
  raster_slices_ = 0;
  auto ended = base::TimeTicks::Now();
  if (ended - started > base::Milliseconds(50))
    std::printf("PERF: record_us=%lld lifecycle_us=%lld picture_us=%lld paint_us=%lld scroll_us=%lld\n",
        static_cast<long long>((ended-started).InMicroseconds()),
        static_cast<long long>((lifecycle_end-started).InMicroseconds()),
        static_cast<long long>((ended-lifecycle_end).InMicroseconds()),
        static_cast<long long>(viewport.paint_microseconds),
        static_cast<long long>((scroll_end-lifecycle_end).InMicroseconds()-viewport.paint_microseconds));
  running_ = runner_->PostTask(FROM_HERE,
      base::BindOnce(&FrameLoop::EncodeScroll, alive));
}
void FrameLoop::RecordScroll(ViewportSnapshot& viewport) {
  pending_scroll_={}; pending_scroll_.acknowledgement=scroll_sequence_;
  if (!renderer_runtime_is_child()) return;
  auto started = base::TimeTicks::Now();
  int reason=0;
  scroll_capture_=BeginScrollScene(view_,&reason,scroll_snapshot_.scene,&viewport);
  auto captured = base::TimeTicks::Now();
  if (scroll_capture_) {
    recording_reflows_=true;
    scroll_capture_->CaptureReflows(view_);
    recording_reflows_=false;
  }
  scroll_encoding_slices_ = 0;
  scroll_encoding_us_ = scroll_encoding_maximum_us_ = 0;
  auto ended = base::TimeTicks::Now();
  if (ended-started > base::Milliseconds(25))
    std::printf("PERF: scroll_capture_us=%lld scroll_reflow_us=%lld shared_paint_us=%lld\n",
        static_cast<long long>((captured-started).InMicroseconds()-viewport.paint_microseconds),
        static_cast<long long>((ended-captured).InMicroseconds()),
        static_cast<long long>(viewport.paint_microseconds));
  if (reason!=scroll_fallback_)
    std::printf("COMPOSITOR: eligibility=%d reason=%s\n",reason,ScrollCaptureFailureName(reason));
  scroll_fallback_=reason;
}
void FrameLoop::EncodeScroll() {
  if (!running_) return;
  if (scroll_capture_) {
    auto started = base::TimeTicks::Now();
    const bool complete = scroll_capture_->EncodeSlice();
    const auto elapsed = (base::TimeTicks::Now()-started).InMicroseconds();
    scroll_encoding_us_ += elapsed;
    scroll_encoding_maximum_us_ = std::max(scroll_encoding_maximum_us_, elapsed);
    ++scroll_encoding_slices_;
    if (!complete) {
      running_ = runner_->PostTask(FROM_HERE,
          base::BindOnce(&FrameLoop::EncodeScroll, weak_factory_.GetWeakPtr()));
      return;
    }
    pending_scroll_.scene = scroll_capture_->TakeScene();
    scroll_capture_.reset();
    if (scroll_encoding_us_ > 25000 || scroll_encoding_slices_ > 1)
      std::printf("PERF: scroll_encode_us=%lld slices=%d maximum_slice_us=%lld ready=%d\n",
          static_cast<long long>(scroll_encoding_us_), scroll_encoding_slices_,
          static_cast<long long>(scroll_encoding_maximum_us_), pending_scroll_.scene != nullptr);
  }
  wire_slices_=0; wire_encoding_us_=wire_maximum_us_=0;
  if (pending_scroll_.scene && renderer_runtime_sandboxed())
    wire_encoder_=std::make_unique<ScrollWireEncoder>(pending_scroll_.scene);
  running_ = runner_->PostTask(FROM_HERE,
      base::BindOnce(&FrameLoop::EncodeWire, weak_factory_.GetWeakPtr()));
}
void FrameLoop::EncodeWire() {
  if (!running_) return;
  if (wire_encoder_) {
    auto started=base::TimeTicks::Now();
    bool complete=wire_encoder_->EncodeSlice();
    auto elapsed=(base::TimeTicks::Now()-started).InMicroseconds();
    wire_encoding_us_+=elapsed; wire_maximum_us_=std::max(wire_maximum_us_,elapsed); ++wire_slices_;
    if (!complete) {
      running_=runner_->PostTask(FROM_HERE,
          base::BindOnce(&FrameLoop::EncodeWire,weak_factory_.GetWeakPtr()));
      return;
    }
    auto bytes=wire_encoder_->TakeBytes(); wire_encoder_.reset();
    if (!bytes.empty()) pending_scroll_.encoded=std::make_shared<const std::vector<uint8_t>>(std::move(bytes));
    if (wire_slices_>1 || wire_encoding_us_>25000)
      std::printf("PERF: scroll_wire_us=%lld slices=%u maximum_slice_us=%lld bytes=%zu\n",
          static_cast<long long>(wire_encoding_us_),wire_slices_,static_cast<long long>(wire_maximum_us_),
          pending_scroll_.encoded ? pending_scroll_.encoded->size() : 0);
  }
  // Pixels, wire bytes and acknowledgement belong to the same immutable paint epoch.
  running_ = runner_->PostTask(FROM_HERE,
      base::BindOnce(&FrameLoop::Raster, weak_factory_.GetWeakPtr()));
}
void FrameLoop::Raster() {
  if (!running_ || !picture_) return;
  auto started = base::TimeTicks::Now();
  auto* target = staging_->getCanvas();
  const int total_rows=viewport_.height();
  const int rows = std::min(raster_rows_, total_rows - raster_y_);
  // Each task replays an immutable snapshot; input may mutate Blink between
  // strips without reentering layout or exposing a partially painted frame.
  if (raster_y_<viewport_.height()) {
    target->save();
    target->clipRect(SkRect::MakeXYWH(0, raster_y_, viewport_.width(), rows));
    target->clear(SK_ColorWHITE);
    picture_->Playback(target);
    target->restore();
  }
  raster_y_ += rows;
  auto elapsed = (base::TimeTicks::Now() - started).InMicroseconds();
  maximum_raster_us_ = std::max(maximum_raster_us_, elapsed);
  raster_total_us_ += elapsed;
  ++raster_slices_;
  // Small strips repeatedly replay filters and images. Bound that overhead
  // while aiming for an eight-millisecond raster task.
  if (elapsed > 12000) raster_rows_ = std::max(64, raster_rows_ / 2);
  else if (elapsed < 4000) raster_rows_ = std::min(256, raster_rows_ * 2);
  if (raster_y_ < total_rows) {
    running_ = runner_->PostTask(FROM_HERE,
        base::BindOnce(&FrameLoop::Raster, weak_factory_.GetWeakPtr()));
    return;
  }
  picture_.reset();
  scroll_snapshot_=std::move(pending_scroll_);
  canvas_.drawImage(staging_->makeImageSnapshot(), 0, 0);
  Finish();
}
void FrameLoop::Finish() {
  const auto latency = (base::TimeTicks::Now() - last_tick_).InMicroseconds();
  if (latency > 50000)
    std::printf("PERF: frame_latency_us=%lld raster_total_us=%lld raster_slices=%d\n",
        static_cast<long long>(latency), static_cast<long long>(raster_total_us_), raster_slices_);
  auto alive = weak_factory_.GetWeakPtr();
  auto present = present_;
  present.Run();
  if (!alive) return;
  running_ = false;
  if (requested_) {
    running_ = runner_->PostDelayedTask(
        FROM_HERE, base::BindOnce(&FrameLoop::Tick, alive),
        std::max(base::TimeDelta(), last_tick_ + base::Milliseconds(16) - base::TimeTicks::Now()));
  }
}
}

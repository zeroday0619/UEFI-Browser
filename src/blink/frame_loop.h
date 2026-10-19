// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
#ifndef UEFI_BLINK_FRAME_LOOP_H_
#define UEFI_BLINK_FRAME_LOOP_H_
#include "base/functional/callback.h"
#include "blink/scroll_snapshot.h"
#include <optional>
#include "cc/paint/paint_record.h"
#include "third_party/skia/include/core/SkSurface.h"
#include "base/memory/weak_ptr.h"
#include "base/time/time.h"
#include "base/task/single_thread_task_runner.h"
#include "blink/navigation.h"
#include "ui/gfx/geometry/rect.h"
namespace uefi_browser {
// The embedding owner must stop and destroy this object before closing the
// frame, view, or raster canvas. Present copies the painted pixels to GOP.
class FrameLoop {
 public:
  FrameLoop(blink::WebView& view, blink::WebFrameWidget& widget,
            SkCanvas& canvas, gfx::Rect viewport,
            scoped_refptr<base::SingleThreadTaskRunner> runner,
            base::RepeatingClosure present);
  ~FrameLoop();
  FrameLoop(const FrameLoop&) = delete;
  FrameLoop& operator=(const FrameLoop&) = delete;
  bool Start();
  void Stop();
  void SetVisible(bool visible);
  const ScrollSnapshot& Snapshot() const { return scroll_snapshot_; }
  void AcknowledgeScroll(uint64_t sequence) { scroll_sequence_=sequence; }
 private:
  void Tick();
  void Layout();
  void Record();
  void RecordScroll();
  void Raster();
  void Finish();
  blink::WebView& view_;
  blink::WebFrameWidget& widget_;
  SkCanvas& canvas_;
  gfx::Rect viewport_;
  scoped_refptr<base::SingleThreadTaskRunner> runner_;
  base::RepeatingClosure present_;
  bool recording_reflows_ = false;
  bool visible_ = true;
  bool running_ = false;
  bool requested_ = false;
  base::TimeTicks last_tick_;
  uint64_t frame_sequence_ = 0;
  std::optional<cc::PaintRecord> picture_;
  sk_sp<SkSurface> staging_;
  int scroll_fallback_=-1;
  uint64_t scroll_sequence_=0;
  ScrollSnapshot scroll_snapshot_, pending_scroll_;
  int raster_y_ = 0;
  int raster_rows_ = 128;
  int64_t raster_total_us_ = 0;
  int raster_slices_ = 0;
  int64_t maximum_raster_us_ = 0;
  int64_t maximum_layout_us_ = 0;
  base::WeakPtrFactory<FrameLoop> weak_factory_{this};
};
}
#endif

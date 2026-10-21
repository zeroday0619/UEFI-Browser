// Copyright 2026 zeroday0619
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
#ifndef UEFI_BROWSER_GPU_CANVAS_H
#define UEFI_BROWSER_GPU_CANVAS_H
#include <cstdint>
#include <memory>
#include "third_party/skia/include/core/SkImage.h"
namespace uefi_browser {
struct GpuCanvasFrame {
  explicit GpuCanvasFrame(uint64_t value):token(value) {}
  ~GpuCanvasFrame();
  uint64_t token;
  sk_sp<SkImage> image;
};
void SetGpuCanvasFrame(int layer, uint64_t token);
std::shared_ptr<GpuCanvasFrame> GetGpuCanvasFrame(int layer);
std::shared_ptr<GpuCanvasFrame> AcquireGpuCanvasFrame(uint64_t token);
}
#endif

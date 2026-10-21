// Copyright 2026 zeroday0619
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
#ifndef UEFI_BROWSER_GPU_COMPOSITOR_H
#define UEFI_BROWSER_GPU_COMPOSITOR_H
#include <memory>
#include "third_party/skia/include/core/SkSurface.h"
namespace uefi_browser {
class GpuCompositor {
 public:
  static bool Requested();
  static std::unique_ptr<GpuCompositor> Create(int width, int height);
  ~GpuCompositor();
  sk_sp<SkSurface> MakeSurface(int width, int height);
  static sk_sp<SkImage> CanvasImage(uint64_t token);
  bool CanPresent();
  bool Present(SkSurface& surface);
  void Poll();
 private:
  struct State;
  explicit GpuCompositor(std::unique_ptr<State> state);
  std::unique_ptr<State> state_;
};
}
#endif

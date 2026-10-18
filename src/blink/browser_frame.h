// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
#ifndef UEFI_BLINK_BROWSER_FRAME_H_
#define UEFI_BLINK_BROWSER_FRAME_H_
#include <memory>
#include <string>
#include "base/functional/callback.h"
#include "network/resource.h"
#include "blink/input.h"
#include "ui/gfx/geometry/size.h"
class SkCanvas;
namespace blink::scheduler { class WebThreadScheduler; }
namespace uefi_browser {
// Blink must be initialized before construction and outlive the frame owner.
class BrowserFrame {
 public:
  BrowserFrame(SystemTable*, blink::scheduler::WebThreadScheduler&, SkCanvas&,
               gfx::Size viewport, base::RepeatingClosure present,
               base::RepeatingCallback<void(std::string)> open_tab);
  ~BrowserFrame();
  BrowserFrame(const BrowserFrame&) = delete;
  BrowserFrame& operator=(const BrowserFrame&) = delete;
  Status Navigate(const char* url);
  bool NavigationPending() const;
  Status NavigationStatus() const;
  std::string CurrentUrl() const;
  bool CanGoBack() const;
  bool CanGoForward() const;
  Status GoBack();
  Status GoForward();
  Status Reload();
  void Pointer(const PointerSample&);
  void Keyboard(const KeySample&);
  void SetVisible(bool visible);
 private:
  class State;
  std::unique_ptr<State> state_;
};
}
#endif

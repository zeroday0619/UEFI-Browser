// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
#ifndef UEFI_BLINK_NAVIGATION_H_
#define UEFI_BLINK_NAVIGATION_H_
#include "network/resource.h"
#include <memory>
#include "base/functional/callback.h"
#include "third_party/blink/public/web/web_frame_load_type.h"
#include "third_party/blink/public/web/web_history_item.h"
#include "third_party/blink/public/platform/web_policy_container.h"
class SkCanvas;
namespace blink {
class WebNavigationControl;
class WebView;
class WebFrameWidget;
} // namespace blink
namespace gfx {
class Rect;
}
namespace uefi_browser {
// The caller owns an initialized Blink frame and pumps its main-thread tasks.
class NavigationRequest { public: virtual ~NavigationRequest() = default; };
std::unique_ptr<NavigationRequest> LoadNavigation(SystemTable *table, blink::WebNavigationControl &frame,
                      const char *url, std::unique_ptr<blink::WebPolicyContainer> policy,
                      blink::WebFrameLoadType load_type = blink::WebFrameLoadType::kStandard,
                      blink::WebHistoryItem history_item = {},
                      base::OnceCallback<void(Status)> completion = {});
void PaintFrame(blink::WebView &view, blink::WebFrameWidget &widget,
                SkCanvas &canvas, const gfx::Rect &viewport);
} // namespace uefi_browser
#endif

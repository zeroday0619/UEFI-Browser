// Copyright 2026 zeroday0619
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
#pragma once
#include <memory>
namespace blink { class WebGraphicsContext3DProvider; }
namespace uefi_browser {
std::unique_ptr<blink::WebGraphicsContext3DProvider> CreateWebGPUProvider();
}

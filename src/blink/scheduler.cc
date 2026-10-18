// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
#include "blink/scheduler.h"
namespace uefi_browser {
std::unique_ptr<blink::scheduler::WebThreadScheduler> CreateScheduler(
    SystemTable* table, MessagePump::PollFunction poll, void* context) {
  return blink::scheduler::WebThreadScheduler::CreateInProcessMainThreadScheduler(
      std::make_unique<MessagePump>(table, poll, context));
}
}

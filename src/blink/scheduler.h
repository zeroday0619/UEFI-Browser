// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
#ifndef UEFI_BLINK_SCHEDULER_H_
#define UEFI_BLINK_SCHEDULER_H_
#include <memory>
#include "blink/message_pump.h"
#include "third_party/blink/public/platform/scheduler/web_thread_scheduler.h"
namespace uefi_browser {
// The owner must retain the returned scheduler until all Blink views and
// frames have closed, then call Shutdown before destroying the scheduler.
std::unique_ptr<blink::scheduler::WebThreadScheduler> CreateScheduler(
    SystemTable* table, MessagePump::PollFunction poll, void* context);
}
#endif

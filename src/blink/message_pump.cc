// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
#include "blink/message_pump.h"
#include <algorithm>
#include <cstdio>
#include "base/check.h"
namespace uefi_browser {
MessagePump::MessagePump(SystemTable* table, PollFunction poll, void* context)
    : table_(table), poll_(poll), context_(context) {
  CHECK(table_);
  CHECK(table_->boot);
  CHECK(table_->boot->function[28]);
}
MessagePump::~MessagePump() = default;
void MessagePump::Run(Delegate* delegate) {
  CHECK(delegate);
  base::TimeTicks last_poll;
  loop_.Run(*delegate, [this, delegate, &last_poll] {
    auto now = base::TimeTicks::Now();
    if (!last_poll.is_null() && now - last_poll > base::Milliseconds(50))
      std::printf("PERF: input_poll_gap_us=%lld\n", static_cast<long long>((now - last_poll).InMicroseconds()));
    last_poll = now;
    delegate->BeginNativeWorkBeforeDoWork();
    auto work_item = delegate->BeginWorkItem();
    return poll_ && poll_(context_);
  }, [this](const Delegate::NextWorkInfo& next) {
    int64_t microseconds = 1000;
    if (!next.delayed_run_time.is_max()) {
      // Idle callbacks can consume time, so resample before sleeping rather
      // than applying the delegate's earlier remaining-delay estimate.
      microseconds = std::clamp(
          (next.delayed_run_time - base::TimeTicks::Now()).InMicroseconds(),
          int64_t{0}, int64_t{1000});
    }
    if (microseconds) {
      auto stall = reinterpret_cast<Status(UEFI_API *)(size_t)>(
          table_->boot->function[28]);
      CHECK_EQ(stall(static_cast<size_t>(microseconds)), Status{0});
    }
  });
}
void MessagePump::Quit() { loop_.Quit(); }
void MessagePump::ScheduleWork() { loop_.ScheduleWork(); }
void MessagePump::ScheduleDelayedWork(const Delegate::NextWorkInfo&) {
  // Re-query the delegate because a new deadline can supersede the deadline
  // observed before an idle callback or a nested run loop.
  loop_.ScheduleWork();
}
}  // namespace uefi_browser

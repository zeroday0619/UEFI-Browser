// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
#ifndef UEFI_BLINK_SCHEDULER_H_
#define UEFI_BLINK_SCHEDULER_H_
#include <array>
#include <memory>
#include "base/task/task_observer.h"
#include "blink/message_pump.h"
#include "third_party/blink/public/platform/scheduler/web_thread_scheduler.h"
namespace uefi_browser {
// Long-task attribution distinguishes queue starvation from decoder work.
// The observer must stop before its scheduler shuts down.
class TaskDiagnostics final : public base::TaskObserver {
 public:
  TaskDiagnostics();
  ~TaskDiagnostics() override;
  TaskDiagnostics(const TaskDiagnostics&) = delete;
  TaskDiagnostics& operator=(const TaskDiagnostics&) = delete;
  void Stop();
  void WillProcessTask(const base::PendingTask&, bool) override;
  void DidProcessTask(const base::PendingTask&) override;

 private:
  struct Sample {
    base::TimeTicks started;
    bool blocked = false;
  };
  // Nested debugger loops must not overwrite the outer task's timestamp.
  std::array<Sample, 32> samples_{};
  size_t depth_ = 0;
  bool registered_ = false;
};
// The owner must retain the returned scheduler until all Blink views and
// frames have closed, then call Shutdown before destroying the scheduler.
std::unique_ptr<blink::scheduler::WebThreadScheduler> CreateScheduler(
    SystemTable* table, MessagePump::PollFunction poll, void* context);
}
#endif

// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
#include "blink/scheduler.h"
#include <algorithm>
#include <cstdio>
#include "base/pending_task.h"
#include "base/task/current_thread.h"
#include "renderer_runtime.h"
extern "C" const int firmware_task_diagnostics;
namespace uefi_browser {
TaskDiagnostics::TaskDiagnostics() {
  if (!firmware_task_diagnostics) return;
  base::CurrentThread::Get().AddTaskObserver(this);
  registered_ = true;
  std::puts("PERF: scheduler task diagnostics enabled; threshold_us=50000");
}
TaskDiagnostics::~TaskDiagnostics() { Stop(); }
void TaskDiagnostics::Stop() {
  if (!registered_) return;
  base::CurrentThread::Get().RemoveTaskObserver(this);
  registered_ = false;
}
void TaskDiagnostics::WillProcessTask(const base::PendingTask&, bool blocked) {
  if (depth_ < samples_.size())
    samples_[depth_] = {base::TimeTicks::Now(), blocked};
  ++depth_;
}
void TaskDiagnostics::DidProcessTask(const base::PendingTask& task) {
  if (!depth_) return;
  --depth_;
  if (depth_ >= samples_.size()) return;
  const auto sample = samples_[depth_];
  const auto elapsed = base::TimeTicks::Now() - sample.started;
  if (elapsed < base::Milliseconds(50)) return;
  const auto eligible = task.GetDesiredExecutionTime();
  const int64_t queued = eligible.is_null() ? -1 :
      std::max(int64_t{0}, (sample.started - eligible).InMicroseconds());
  const char* file = task.posted_from.file_name();
  const char* function = task.posted_from.function_name();
  std::printf("PERF: scheduler_task_us=%lld queue_wait_us=%lld blocked=%u depth=%zu role=%s location=%s:%d function=%s\n",
      static_cast<long long>(elapsed.InMicroseconds()),
      static_cast<long long>(queued), unsigned(sample.blocked), depth_,
      renderer_runtime_is_child() ? "renderer" : "browser",
      file ? file : "unavailable", task.posted_from.line_number(),
      function ? function : "unavailable");
}
std::unique_ptr<blink::scheduler::WebThreadScheduler> CreateScheduler(
    SystemTable* table, MessagePump::PollFunction poll, void* context) {
  return blink::scheduler::WebThreadScheduler::CreateInProcessMainThreadScheduler(
      std::make_unique<MessagePump>(table, poll, context));
}
}

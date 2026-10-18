// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
#ifndef UEFI_BLINK_PUMP_LOOP_H_
#define UEFI_BLINK_PUMP_LOOP_H_
#include <atomic>
#include <cstdint>
namespace uefi_browser {
// The loop is independent of Chromium types so its reentrancy and wake-up
// behavior can be tested without substituting Chromium scheduler symbols.
class PumpLoop {
 public:
  void Quit() {
    if (current_) current_->quit = true;
  }
  void ScheduleWork() { scheduled_.store(true, std::memory_order_release); }
  template <typename Delegate, typename Poll, typename Wait>
  void Run(Delegate& delegate, Poll poll, Wait wait) {
    State state{false, current_};
    current_ = &state;
    while (!state.quit) {
      scheduled_.exchange(false, std::memory_order_acquire);
      bool native_work = poll();
      if (state.quit) break;
      auto next = delegate.DoWork();
      if (state.quit) break;
      if (native_work || next.is_immediate()) continue;
      delegate.DoIdleWork();
      if (state.quit) break;
      if (scheduled_.load(std::memory_order_acquire)) continue;
      delegate.BeforeWait();
      if (state.quit) break;
      if (scheduled_.load(std::memory_order_acquire)) continue;
      // A bounded firmware stall also services input and network polling when
      // Chromium has no pending delayed tasks. It bounds cross-thread wake-up
      // latency without calling firmware services from another processor.
      wait(next);
    }
    current_ = state.previous;
  }
 private:
  struct State { bool quit; State* previous; };
  State* current_ = nullptr;
  std::atomic<bool> scheduled_{false};
};
}  // namespace uefi_browser
#endif

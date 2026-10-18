// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
#ifndef UEFI_BLINK_MESSAGE_PUMP_H_
#define UEFI_BLINK_MESSAGE_PUMP_H_
#include "base/message_loop/message_pump.h"
#include "blink/pump_loop.h"
#include "uefi.h"
namespace uefi_browser {
// Chromium owns task ordering and timers; firmware polling supplies native
// input and transport progress while Boot Services remain available.
class MessagePump final : public base::MessagePump {
 public:
  using PollFunction = bool (*)(void*);
  MessagePump(SystemTable* table, PollFunction poll, void* context);
  ~MessagePump() override;
  void Run(Delegate* delegate) override;
  void Quit() override;
  void ScheduleWork() override;
  void ScheduleDelayedWork(const Delegate::NextWorkInfo& next) override;
 private:
  SystemTable* table_;
  PollFunction poll_;
  void* context_;
  PumpLoop loop_;
};
}  // namespace uefi_browser
#endif

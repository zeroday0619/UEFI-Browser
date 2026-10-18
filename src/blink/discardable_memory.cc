// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
#include <cstdlib>
#include <list>
#include <memory>
#include "base/check.h"
#include "base/memory/discardable_memory_allocator.h"
#include "base/trace_event/memory_allocator_dump.h"
#include "base/trace_event/process_memory_dump.h"

namespace uefi_browser {
namespace {
class Memory;
struct State {
  std::list<Memory*> blocks;
  size_t bytes = 0;
};
class Memory final : public base::DiscardableMemory {
 public:
  Memory(std::shared_ptr<State> state, size_t size, void* data)
      : state_(std::move(state)), size_(size), data_(data) {
    position_ = state_->blocks.insert(state_->blocks.end(), this);
    state_->bytes += size_;
  }
  ~Memory() override {
    Release();
    state_->blocks.erase(position_);
  }
  bool Lock() override {
    CHECK(!locked_);
    if (!data_)
      return false;
    locked_ = true;
    return true;
  }
  void Unlock() override {
    CHECK(locked_);
    locked_ = false;
  }
  void* data() const override {
    CHECK(locked_ && data_);
    return data_;
  }
  void DiscardForTesting() override {
    CHECK(!locked_);
    Release();
  }
  void ReleaseIfUnlocked() {
    if (!locked_)
      Release();
  }
  base::trace_event::MemoryAllocatorDump* CreateMemoryAllocatorDump(
      const char* name, base::trace_event::ProcessMemoryDump* dump) const override {
    auto* result = dump->CreateAllocatorDump(name);
    result->AddScalar("size", "bytes", data_ ? size_ : 0);
    return result;
  }
 private:
  void Release() {
    if (data_) {
      std::free(data_);
      data_ = nullptr;
      state_->bytes -= size_;
    }
  }
  std::shared_ptr<State> state_;
  std::list<Memory*>::iterator position_;
  size_t size_;
  void* data_;
  bool locked_ = true;
};
class Allocator final : public base::DiscardableMemoryAllocator {
 public:
  Allocator() { SetInstance(this); }
  ~Allocator() override { SetInstance(nullptr); }
  std::unique_ptr<base::DiscardableMemory> AllocateLockedDiscardableMemory(
      size_t size) override {
    constexpr size_t budget = 64u * 1024u * 1024u;
    if (!size || size > budget)
      return nullptr;
    if (state_->bytes > budget - size)
      ReleaseFreeMemory();
    if (state_->bytes > budget - size)
      return nullptr;
    void* data = std::malloc(size);
    if (!data) {
      ReleaseFreeMemory();
      data = std::malloc(size);
    }
    if (!data)
      return nullptr;
    return std::make_unique<Memory>(state_, size, data);
  }
  size_t GetBytesAllocated() const override { return state_->bytes; }
  void ReleaseFreeMemory() override {
    for (Memory* block : state_->blocks)
      block->ReleaseIfUnlocked();
  }
 private:
  // Cached Skia blocks can outlive the allocator during static destruction.
  std::shared_ptr<State> state_ = std::make_shared<State>();
};
}
std::unique_ptr<base::DiscardableMemoryAllocator> CreateDiscardableAllocator() {
  return std::make_unique<Allocator>();
}
}

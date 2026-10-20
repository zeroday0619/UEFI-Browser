// Copyright 2026 zeroday0619
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
#include "blink/gpu_context.h"

#include <cstdio>
#include <cstring>
#include <vector>
#include "base/functional/bind.h"
#include "base/memory/weak_ptr.h"
#include "base/task/single_thread_task_runner.h"
#include "base/time/time.h"
#include "dawn/native/DawnNative.h"
#include "dawn/wire/WireClient.h"
#include "dawn/wire/WireServer.h"
#include "gpu/command_buffer/client/webgpu_interface.h"
#include "gpu/command_buffer/common/capabilities.h"
#include "gpu/command_buffer/common/sync_token.h"
#include "gpu/config/gpu_feature_info.h"
#include "third_party/blink/public/platform/web_graphics_context_3d_provider.h"

namespace uefi_browser {
namespace {
const DawnProcTable& FirmwareProcs() {
  static const DawnProcTable procs = [] {
    auto table = dawn::native::GetProcs();
    table.adapterRequestDevice = [](WGPUAdapter adapter,
        const WGPUDeviceDescriptor* descriptor, WGPURequestDeviceCallbackInfo callback) {
      WGPUDeviceDescriptor augmented = descriptor ? *descriptor : WGPU_DEVICE_DESCRIPTOR_INIT;
      std::vector<WGPUFeatureName> features;
      for (size_t index = 0; index < augmented.requiredFeatureCount; ++index)
        features.push_back(augmented.requiredFeatures[index]);
      // Canvas readback needs internal copy usage without changing the web-visible usage.
      features.push_back(WGPUFeatureName_DawnInternalUsages);
      augmented.requiredFeatures = features.data();
      augmented.requiredFeatureCount = features.size();
      return dawn::native::GetProcs().adapterRequestDevice(adapter, &augmented, callback);
    };
    return table;
  }();
  return procs;
}

// Wire messages own their storage until dispatch, including reentrant replies.
class WireQueue final : public dawn::wire::CommandSerializer {
 public:
  dawn::wire::CommandHandler* destination = nullptr;
  bool failed = false;
  std::optional<std::span<volatile std::byte>> GetCommandSpace(size_t size) override {
    if (failed || size > GetMaximumAllocationSize() || size > 32*1024*1024 - bytes_) return std::nullopt;
    messages_.emplace_back((size + 7) / 8);
    lengths_.push_back(size);
    bytes_ += size;
    return std::span<volatile std::byte>(reinterpret_cast<volatile std::byte*>(messages_.back().data()), size);
  }
  size_t GetMaximumAllocationSize() const override { return 1024*1024; }
  void Clear() {
    messages_.clear();
    lengths_.clear();
    bytes_ = 0;
  }
  bool Flush() override {
    if (failed || !destination) return false;
    auto messages = std::move(messages_);
    auto lengths = std::move(lengths_);
    messages_.clear(); lengths_.clear(); bytes_ = 0;
    for (size_t index = 0; index < messages.size(); ++index) {
      if (!destination->HandleCommands({reinterpret_cast<const volatile std::byte*>(messages[index].data()), lengths[index]})) {
        failed = true; return false;
      }
    }
    return true;
  }
 private:
  size_t bytes_ = 0;
  std::vector<std::vector<uint64_t>> messages_;
  std::vector<size_t> lengths_;
};

class FirmwareGPUChannel final : public gpu::webgpu::APIChannel {
 public:
  FirmwareGPUChannel()
      : client_({.serializer = &commands_}),
        server_(std::make_unique<dawn::wire::WireServer>(
            dawn::wire::WireServerDescriptor{.procs = &FirmwareProcs(), .serializer = &replies_})) {
    commands_.destination = server_.get();
    replies_.destination = &client_;
    auto reservation = client_.ReserveInstance();
    instance_ = reservation.instance;
    if (!server_->InjectInstance(native_->Get(), reservation.handle)) Disconnect();
  }
  void Start() { Poll(); }
  WGPUInstance GetWGPUInstance() const override { return instance_; }
  void Disconnect() override {
    if (disconnected_) return;
    disconnected_ = true;
    weak_.InvalidateWeakPtrs();
    client_.Disconnect();
    if (!flushing_) ReleaseNativeService();
    if (lost_callback_ && base::SingleThreadTaskRunner::HasCurrentDefault())
      base::SingleThreadTaskRunner::GetCurrentDefault()->PostTask(
          FROM_HERE, base::BindOnce(&FirmwareGPUChannel::NotifyLost, weak_.GetWeakPtr()));
  }
  void SetLostCallback(base::RepeatingClosure callback) {
    lost_callback_ = std::move(callback);
  }
  bool lost() const { return disconnected_; }
  void Flush() {
    if (disconnected_ || flushing_) return;
    auto keep_alive = base::WrapRefCounted(this);
    flushing_ = true;
    if (!commands_.Flush()) Disconnect();
    if (!disconnected_) dawn::native::InstanceProcessEvents(native_->Get());
    if (!disconnected_ && !replies_.Flush()) Disconnect();
    flushing_ = false;
    if (disconnected_) ReleaseNativeService();
  }
  dawn::wire::WireClient& client() { return client_; }
 private:
  ~FirmwareGPUChannel() override {
    Disconnect();
    if (instance_) dawn::wire::client::GetProcs().instanceRelease(instance_);
  }
  void ReleaseNativeService() {
    // DOM handles may survive navigation until GC, but native devices must not.
    if (!server_) return;
    commands_.destination = nullptr;
    replies_.destination = nullptr;
    server_.reset();
    native_.reset();
    commands_.Clear();
    replies_.Clear();
    std::printf("GPU: released native Dawn service\n");
  }
  void NotifyLost() {
    auto keep_alive = base::WrapRefCounted(this);
    if (lost_callback_) lost_callback_.Run();
  }
  void Poll() {
    auto keep_alive = base::WrapRefCounted(this);
    Flush();
    // Promise callbacks run from a fresh task rather than reentering a wire flush.
    dawn::wire::client::GetProcs().instanceProcessEvents(instance_);
    if (!disconnected_) base::SingleThreadTaskRunner::GetCurrentDefault()->PostDelayedTask(
        FROM_HERE, base::BindOnce(&FirmwareGPUChannel::Poll, weak_.GetWeakPtr()), base::Milliseconds(2));
  }
  std::unique_ptr<dawn::native::Instance> native_ = std::make_unique<dawn::native::Instance>();
  WireQueue commands_, replies_;
  dawn::wire::WireClient client_;
  std::unique_ptr<dawn::wire::WireServer> server_;
  WGPUInstance instance_ = nullptr;
  bool disconnected_ = false, flushing_ = false;
  base::RepeatingClosure lost_callback_;
  base::WeakPtrFactory<FirmwareGPUChannel> weak_{this};
};

class FirmwareWebGPU final : public gpu::webgpu::WebGPUInterface {
 public:
  FirmwareWebGPU() : channel_(base::MakeRefCounted<FirmwareGPUChannel>()) { channel_->Start(); }
  ~FirmwareWebGPU() override {
    channel_->SetLostCallback({});
    channel_->Disconnect();
  }
  void SetLostCallback(base::RepeatingClosure callback) {
    channel_->SetLostCallback(std::move(callback));
  }
  bool lost() const { return channel_->lost(); }
  void FlushCommands() override { awaiting_ = false; channel_->Flush(); }
  bool EnsureAwaitingFlush() override { bool previous = awaiting_; awaiting_ = true; return !previous; }
  void FlushAwaitingCommands() override { if (awaiting_) FlushCommands(); }
  scoped_refptr<gpu::webgpu::APIChannel> GetAPIChannel() const override { return channel_; }
  void GenSyncTokenCHROMIUM(GLbyte* token) override { FlushCommands(); std::memset(token, 0, sizeof(gpu::SyncToken)); }
  void GenUnverifiedSyncTokenCHROMIUM(GLbyte* token) override { GenSyncTokenCHROMIUM(token); }
  void VerifySyncTokensCHROMIUM(GLbyte**, GLsizei) override { FlushCommands(); }
  void WaitSyncTokenCHROMIUM(const GLbyte*) override { FlushCommands(); }
  void ShallowFlushCHROMIUM() override { FlushCommands(); }
  gpu::webgpu::ReservedBuffer ReserveBuffer(WGPUDevice device, const WGPUBufferDescriptor* descriptor) override {
    auto reserved = channel_->client().ReserveBuffer(device, descriptor);
    return {reserved.buffer, reserved.handle.id, reserved.handle.generation, reserved.deviceHandle.id, reserved.deviceHandle.generation};
  }
  gpu::webgpu::ReservedTexture ReserveTexture(WGPUDevice device, const WGPUTextureDescriptor* descriptor) override {
    auto reserved = channel_->client().ReserveTexture(device, descriptor);
    return {reserved.texture, reserved.handle.id, reserved.handle.generation, reserved.deviceHandle.id, reserved.deviceHandle.generation};
  }
  WGPUDevice DeprecatedEnsureDefaultDeviceSync() override { return nullptr; }
  void AssociateMailbox(GLuint, GLuint, GLuint, GLuint, uint64_t, uint64_t, const WGPUTextureFormat*, GLuint, gpu::webgpu::MailboxFlags, const gpu::Mailbox&) override { channel_->Disconnect(); }
  void AssociateMailboxForBuffer(GLuint, GLuint, GLuint, GLuint, uint64_t, const gpu::Mailbox&) override { channel_->Disconnect(); }
  void DissociateMailbox(GLuint, GLuint) override { FlushCommands(); }
  void DissociateMailboxForBuffer(GLuint, GLuint) override { FlushCommands(); }
  void DissociateMailboxForPresent(GLuint, GLuint, GLuint, GLuint) override { FlushCommands(); }
  void SetWebGPUExecutionContextToken(uint32_t, uint32_t, uint32_t, uint32_t, uint32_t) override {}
 private:
  scoped_refptr<FirmwareGPUChannel> channel_;
  bool awaiting_ = false;
};

class FirmwareGPUProvider final : public blink::WebGraphicsContext3DProvider {
 public:
  gpu::InterfaceBase* InterfaceBase() override { return &webgpu_; }
  gpu::gles2::GLES2Interface* ContextGL() override { return nullptr; }
  gpu::raster::RasterInterface* RasterInterface() override { return nullptr; }
  gpu::webgpu::WebGPUInterface* WebGPUInterface() override { return &webgpu_; }
  gpu::ContextSupport* ContextSupport() override { return nullptr; }
  bool IsContextLost() override { return webgpu_.lost(); }
  bool BindToCurrentSequence() override { return !IsContextLost(); }
  const gpu::Capabilities& GetCapabilities() const override { return capabilities_; }
  const gpu::GpuFeatureInfo& GetGpuFeatureInfo() const override { return features_; }
  const blink::WebglPreferences& GetWebglPreferences() const override { return preferences_; }
  void SetLostContextCallback(base::RepeatingClosure callback) override { webgpu_.SetLostCallback(std::move(callback)); }
  void SetErrorMessageCallback(base::RepeatingCallback<void(const char*, int32_t)> callback) override { error_callback_ = std::move(callback); }
  cc::ImageDecodeCache* ImageDecodeCache(SkColorType) override { return nullptr; }
  gpu::SharedImageInterface* SharedImageInterface() override { return nullptr; }
  viz::RasterContextProvider* RasterContextProvider() const override { return nullptr; }
 private:
  FirmwareWebGPU webgpu_;
  gpu::Capabilities capabilities_;
  gpu::GpuFeatureInfo features_;
  blink::WebglPreferences preferences_;
  base::RepeatingCallback<void(const char*, int32_t)> error_callback_;
};
}
std::unique_ptr<blink::WebGraphicsContext3DProvider> CreateWebGPUProvider() {
  std::printf("GPU: creating Dawn wire channel with static SwiftShader Vulkan\n");
  return std::make_unique<FirmwareGPUProvider>();
}
}

// Copyright 2026 zeroday0619
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
#include "blink/webgl_context.h"
#include <cstdio>
#include "base/command_line.h"
#include "base/functional/bind.h"
#include "gpu/ipc/gl_in_process_context.h"
#include "gpu/ipc/in_process_gpu_thread_holder.h"
#include "gpu/command_buffer/client/gles2_implementation.h"
#include "third_party/blink/public/platform/web_graphics_context_3d_provider.h"
#include "ui/gl/init/gl_factory.h"
#include "ui/gl/gl_implementation.h"
#include "ui/ozone/public/ozone_platform.h"

namespace uefi_browser {
namespace {
class FirmwareGLProvider final : public blink::WebGraphicsContext3DProvider {
 public:
  bool Initialize(blink::Platform::WebGLContextType type) {
    static bool initialized = false;
    if (!initialized) {
      auto* command = base::CommandLine::ForCurrentProcess();
      command->AppendSwitchASCII("use-gl", "angle");
      command->AppendSwitchASCII("use-angle", "vulkan");
      command->AppendSwitchASCII("ozone-platform", "headless");
      ui::OzonePlatform::InitializeForGPU({});
      if (!gl::init::InitializeGLOneOff(gl::GpuPreference::kDefault)) return false;
      initialized = true;
    }
    // Contexts retain the service so the GPU thread outlives their command buffers.
    static std::weak_ptr<gpu::InProcessGpuThreadHolder> current;
    service_ = current.lock();
    if (!service_) {
      service_ = std::make_shared<gpu::InProcessGpuThreadHolder>();
      current = service_;
    }
    return context_.Initialize(service_->GetTaskExecutor(),
        type == blink::Platform::kWebGL2ContextType ? gpu::CONTEXT_TYPE_WEBGL2 : gpu::CONTEXT_TYPE_WEBGL1) == gpu::ContextResult::kSuccess;
  }
  gpu::InterfaceBase* InterfaceBase() override { return context_.GetImplementation(); }
  gpu::gles2::GLES2Interface* ContextGL() override { return context_.GetImplementation(); }
  gpu::raster::RasterInterface* RasterInterface() override { return nullptr; }
  gpu::webgpu::WebGPUInterface* WebGPUInterface() override { return nullptr; }
  gpu::ContextSupport* ContextSupport() override { return context_.GetImplementation(); }
  bool IsContextLost() override { return context_.GetImplementation()->GetGraphicsResetStatusKHR() != GL_NO_ERROR; }
  bool BindToCurrentSequence() override { return !IsContextLost(); }
  const gpu::Capabilities& GetCapabilities() const override { return context_.GetCapabilities(); }
  const gpu::GpuFeatureInfo& GetGpuFeatureInfo() const override { return context_.GetGpuFeatureInfo(); }
  const blink::WebglPreferences& GetWebglPreferences() const override { return preferences_; }
  void SetLostContextCallback(base::RepeatingClosure callback) override {
    context_.GetImplementation()->SetLostContextCallback(
        callback ? base::BindOnce(std::move(callback)) : base::OnceClosure());
  }
  void SetErrorMessageCallback(base::RepeatingCallback<void(const char*, int32_t)> callback) override {
    context_.GetImplementation()->SetErrorMessageCallback(std::move(callback));
  }
  cc::ImageDecodeCache* ImageDecodeCache(SkColorType) override { return nullptr; }
  gpu::SharedImageInterface* SharedImageInterface() override { return context_.GetSharedImageInterface(); }
  viz::RasterContextProvider* RasterContextProvider() const override { return nullptr; }
 private:
  std::shared_ptr<gpu::InProcessGpuThreadHolder> service_;
  gpu::GLInProcessContext context_;
  blink::WebglPreferences preferences_;
};
}
std::unique_ptr<blink::WebGraphicsContext3DProvider> CreateWebGLProvider(
    blink::Platform::WebGLContextType type, blink::Platform::WebGLContextInfo* information) {
  auto provider = std::make_unique<FirmwareGLProvider>();
  if (!provider->Initialize(type)) return nullptr;
  information->using_gpu_compositing = false;
  information->angle_implementation = gl::ANGLEImplementation::kVulkan;
  information->vendor_info = blink::WebString::FromAscii("UEFI");
  information->renderer_info = blink::WebString::FromAscii("ANGLE / SwiftShader");
  std::printf("GPU: ANGLE WebGL provider initialized type=%d\n", static_cast<int>(type));
  return provider;
}
}

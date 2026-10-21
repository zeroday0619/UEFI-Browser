// Copyright 2026 zeroday0619
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
#include <cstdio>
#include "third_party/vulkan-headers/src/include/vulkan/vulkan.h"
extern "C" PFN_vkVoidFunction uefi_swiftshader_vkGetInstanceProcAddr(VkInstance, const char*);
extern "C" PFN_vkVoidFunction firmware_venus_get_proc(VkInstance, const char*) __attribute__((weak));
extern "C" PFN_vkVoidFunction uefi_browser_vkGetInstanceProcAddr(VkInstance instance, const char* name) {
  // The optional hardware image links the broker proxy; ordinary images retain software Vulkan.
  static bool announced = false;
  if (!announced) {
    std::printf("GPU: Vulkan backend=%s\n", firmware_venus_get_proc ? "Mesa Venus" : "SwiftShader");
    announced = true;
  }
  return firmware_venus_get_proc ? firmware_venus_get_proc(instance, name)
                               : uefi_swiftshader_vkGetInstanceProcAddr(instance, name);
}

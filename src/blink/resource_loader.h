// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
#ifndef UEFI_BLINK_RESOURCE_LOADER_H_
#define UEFI_BLINK_RESOURCE_LOADER_H_
#include <memory>
#include <utility>
#include "base/memory/scoped_refptr.h"
#include "network/resource.h"
#include "services/network/public/cpp/cross_origin_embedder_policy.h"
#include "services/network/public/cpp/orb/orb_api.h"
#include "url/origin.h"
namespace base { class SingleThreadTaskRunner; class WaitableEvent; }
namespace blink { class URLLoader; }
namespace uefi_browser {
// Each document or worker owns its media range history on its loading sequence.
struct ResourceSecurityContext {
  ResourceSecurityContext(url::Origin value,
      network::mojom::CrossOriginEmbedderPolicyValue policy)
      : origin(std::move(value)) { embedder_policy.value = policy; }
  const url::Origin origin;
  network::CrossOriginEmbedderPolicy embedder_policy;
  network::orb::PerFactoryState media_responses;
};
#if defined(UEFI_RUNTIME)
// Install before frame creation and clear after all frames are closed.
void InstallResourceLoader(SystemTable* table);
void UninstallResourceLoader();
#endif
// The document origin is supplied by the embedder, not by the request.
std::unique_ptr<blink::URLLoader> CreateResourceLoader(
    SystemTable* table, std::shared_ptr<ResourceSecurityContext> security,
    scoped_refptr<base::SingleThreadTaskRunner> task_runner,
    base::WaitableEvent* termination_event = nullptr);
}
#endif

// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
#ifndef UEFI_BLINK_RESOURCE_LOADER_H_
#define UEFI_BLINK_RESOURCE_LOADER_H_
#include <memory>
#include "base/memory/scoped_refptr.h"
#include "network/resource.h"
#include "url/origin.h"
namespace base { class SingleThreadTaskRunner; }
namespace blink { class URLLoader; }
namespace uefi_browser {
#if defined(UEFI_RUNTIME)
// Install before frame creation and clear after all frames are closed.
void InstallResourceLoader(SystemTable* table);
void UninstallResourceLoader();
#endif
// The document origin is supplied by the embedder, not by the request.
std::unique_ptr<blink::URLLoader> CreateResourceLoader(
    SystemTable* table, const url::Origin& document_origin,
    scoped_refptr<base::SingleThreadTaskRunner> task_runner);
}
#endif

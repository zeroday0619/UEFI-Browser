#ifndef UEFI_BROWSER_CACHE_STORAGE_H
#define UEFI_BROWSER_CACHE_STORAGE_H
#include "mojo/public/cpp/bindings/pending_receiver.h"
#include "third_party/blink/public/common/tokens/tokens.h"
#include "third_party/blink/public/mojom/cache_storage/cache_storage.mojom.h"
namespace uefi_browser {
void BindCacheStorage(
    blink::LocalFrameToken token,
    mojo::PendingReceiver<blink::mojom::CacheStorage> receiver);
}
#endif

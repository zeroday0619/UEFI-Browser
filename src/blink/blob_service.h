#ifndef UEFI_BROWSER_BLOB_SERVICE_H
#define UEFI_BROWSER_BLOB_SERVICE_H
#include "base/functional/callback.h"
#include "mojo/public/cpp/bindings/pending_receiver.h"
#include "third_party/blink/public/mojom/blob/blob_registry.mojom.h"
#include <memory>
#include <optional>
#include <vector>
namespace uefi_browser {
using BlobBytes = std::vector<uint8_t>;
using BlobReadCallback = base::OnceCallback<void(std::optional<BlobBytes>)>;
void BindBlobRegistry(
    mojo::PendingReceiver<blink::mojom::BlobRegistry> receiver);
blink::mojom::SerializedBlobPtr MakeBlob(BlobBytes bytes,
                                         const std::string &type);
void ReadBlob(mojo::PendingRemote<blink::mojom::Blob> blob, uint64_t offset,
              uint64_t length, BlobReadCallback callback);
} // namespace uefi_browser
#endif

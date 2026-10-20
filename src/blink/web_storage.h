#ifndef UEFI_WEB_STORAGE_H
#define UEFI_WEB_STORAGE_H
#include "mojo/public/cpp/bindings/pending_receiver.h"
#include "third_party/blink/public/common/tokens/tokens.h"
#include "third_party/blink/public/mojom/dom_storage/session_storage_namespace.mojom-blink.h"
#include "third_party/blink/public/mojom/dom_storage/storage_area.mojom-blink.h"
#include "third_party/blink/renderer/platform/storage/blink_storage_key.h"
#include <string>
namespace blink {
void BindFirmwareStorageArea(const BlinkStorageKey &, const LocalFrameToken &,
                             const String &,
                             mojo::PendingReceiver<mojom::blink::StorageArea>);
void BindFirmwareStorageNamespace(
    const String &,
    mojo::PendingReceiver<mojom::blink::SessionStorageNamespace>);
void ReleaseFirmwareStorageNamespace(const std::string &);
} // namespace blink
#endif

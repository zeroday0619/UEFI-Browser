#ifndef UEFI_BROWSER_INDEXED_DB_H
#define UEFI_BROWSER_INDEXED_DB_H
#include "mojo/public/cpp/bindings/pending_receiver.h"
#include "third_party/blink/public/common/tokens/tokens.h"
#include "third_party/blink/public/mojom/indexeddb/indexeddb.mojom.h"
namespace uefi_browser {
void BindIndexedDB(blink::LocalFrameToken token,
                   mojo::PendingReceiver<blink::mojom::IDBFactory> receiver);
}
#endif

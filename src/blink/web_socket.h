#ifndef UEFI_BROWSER_WEB_SOCKET_H
#define UEFI_BROWSER_WEB_SOCKET_H
#include "mojo/public/cpp/bindings/pending_receiver.h"
#include "third_party/blink/public/common/tokens/tokens.h"
#include "third_party/blink/public/mojom/websockets/websocket_connector.mojom.h"
#include <memory>
namespace uefi_browser {
class WebSocketService {
public:
  virtual ~WebSocketService() = default;
  virtual void
      Bind(mojo::PendingReceiver<blink::mojom::WebSocketConnector>) = 0;
};
std::unique_ptr<WebSocketService>
    CreateWebSocketService(blink::LocalFrameToken);
} // namespace uefi_browser
#endif

#ifndef UEFI_BROWSER_BROKER_H
#define UEFI_BROWSER_BROKER_H
#include <cstdio>
#include "blink/web_socket.h"
#include "blink/cache_storage.h"
#include "blink/indexed_db.h"
#include <utility>
#include "mojo/public/cpp/bindings/generic_pending_receiver.h"
#include "mojo/public/cpp/bindings/receiver.h"
#include "mojo/public/cpp/bindings/receiver_set.h"
#include "mojo/public/cpp/bindings/remote.h"
#include "mojo/public/cpp/bindings/remote_set.h"
#include "third_party/blink/public/mojom/browser_interface_broker.mojom.h"
#include "third_party/blink/public/mojom/permissions/permission.mojom.h"
#include "third_party/blink/public/mojom/permissions/permission_status.mojom.h"

namespace uefi_browser {
// Unsupported capabilities receive terminal responses instead of abandoned promises.
class BrowserBroker final : public blink::mojom::BrowserInterfaceBroker,
                            public blink::mojom::PermissionService {
 public:
  mojo::PendingRemote<blink::mojom::BrowserInterfaceBroker> Bind() {
    return receiver_.BindNewPipeAndPassRemote();
  }
  void SetFrameToken(blink::LocalFrameToken token) { token_ = token; sockets_ = CreateWebSocketService(token); }
  void GetInterface(mojo::GenericPendingReceiver receiver) override {
    if (auto database = receiver.As<blink::mojom::IDBFactory>()) {
      BindIndexedDB(token_, std::move(database));
      return;
    }
    if (auto cache = receiver.As<blink::mojom::CacheStorage>()) {
      BindCacheStorage(token_, std::move(cache));
      return;
    }
    if (auto socket = receiver.As<blink::mojom::WebSocketConnector>()) {
      if (sockets_) sockets_->Bind(std::move(socket));
      return;
    }
    if (auto permission = receiver.As<blink::mojom::PermissionService>()) {
      permissions_.Add(this, std::move(permission));
      return;
    }
    if (receiver.interface_name())
      std::printf("BROKER: unsupported interface %s\n", receiver.interface_name()->c_str());
  }
  void HasPermission(blink::mojom::PermissionDescriptorPtr,
                     HasPermissionCallback callback) override {
    std::move(callback).Run(Denied());
  }
  void RequestPermission(blink::mojom::PermissionDescriptorPtr,
                         RequestPermissionCallback callback) override {
    std::move(callback).Run(Denied());
  }
  void RevokePermission(blink::mojom::PermissionDescriptorPtr,
                        RevokePermissionCallback callback) override {
    std::move(callback).Run(Denied());
  }
  void RequestPermissions(std::vector<blink::mojom::PermissionDescriptorPtr> permissions,
                          RequestPermissionsCallback callback) override {
    std::vector<blink::mojom::PermissionStatusWithDetailsPtr> results;
    for (size_t index = 0; index < permissions.size(); ++index) results.push_back(Denied());
    std::move(callback).Run(std::move(results));
  }
  void RegisterPageEmbeddedPermissionControl(
      std::vector<blink::mojom::PermissionDescriptorPtr>,
      blink::mojom::EmbeddedPermissionRequestDescriptorPtr,
      mojo::PendingRemote<blink::mojom::EmbeddedPermissionControlClient> client) override {
    mojo::Remote<blink::mojom::EmbeddedPermissionControlClient> remote(std::move(client));
    remote->OnEmbeddedPermissionControlRegistered(false, std::nullopt);
  }
  void RequestPageEmbeddedPermission(
      std::vector<blink::mojom::PermissionDescriptorPtr>,
      blink::mojom::EmbeddedPermissionRequestDescriptorPtr,
      RequestPageEmbeddedPermissionCallback callback) override {
    std::move(callback).Run(blink::mojom::EmbeddedPermissionControlResult::kNotSupported);
  }
  void AddPermissionObserver(blink::mojom::PermissionDescriptorPtr,
      blink::mojom::PermissionStatusWithDetailsPtr last_status,
      mojo::PendingRemote<blink::mojom::PermissionObserver> observer) override {
    Observe(std::move(observer), last_status->status);
  }
  void AddPageEmbeddedPermissionObserver(blink::mojom::PermissionDescriptorPtr,
      blink::mojom::PermissionStatus last_status,
      mojo::PendingRemote<blink::mojom::PermissionObserver> observer) override {
    Observe(std::move(observer), last_status);
  }
  void NotifyEventListener(blink::mojom::PermissionDescriptorPtr,
                           const std::string&, bool) override {}
 private:
  blink::LocalFrameToken token_;
  std::unique_ptr<WebSocketService> sockets_;
  static blink::mojom::PermissionStatusWithDetailsPtr Denied() {
    return blink::mojom::PermissionStatusWithDetails::New(
        blink::mojom::PermissionStatus::DENIED, nullptr);
  }
  void Observe(mojo::PendingRemote<blink::mojom::PermissionObserver> observer,
               blink::mojom::PermissionStatus previous) {
    mojo::Remote<blink::mojom::PermissionObserver> remote(std::move(observer));
    if (previous != blink::mojom::PermissionStatus::DENIED)
      remote->OnPermissionStatusChange(Denied());
    observers_.Add(std::move(remote));
  }
  mojo::Receiver<blink::mojom::BrowserInterfaceBroker> receiver_{this};
  mojo::ReceiverSet<blink::mojom::PermissionService> permissions_;
  mojo::RemoteSet<blink::mojom::PermissionObserver> observers_;
};
}
#endif

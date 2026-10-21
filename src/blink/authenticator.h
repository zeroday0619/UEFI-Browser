#ifndef UEFI_BROWSER_AUTHENTICATOR_H
#define UEFI_BROWSER_AUTHENTICATOR_H

#include <utility>
#include <vector>
#include "mojo/public/cpp/bindings/receiver_set.h"
#include "third_party/blink/public/mojom/webauthn/authenticator.mojom.h"

namespace uefi_browser {
// Availability queries must complete even when no authenticator backend exists.
class FirmwareAuthenticator final : public blink::mojom::Authenticator {
 public:
  void Bind(mojo::PendingReceiver<blink::mojom::Authenticator> receiver) {
    receivers_.Add(this, std::move(receiver));
  }
  void IsUserVerifyingPlatformAuthenticatorAvailable(
      IsUserVerifyingPlatformAuthenticatorAvailableCallback callback) override {
    std::move(callback).Run(false);
  }
  void IsConditionalMediationAvailable(
      IsConditionalMediationAvailableCallback callback) override {
    std::move(callback).Run(false);
  }
  void GetClientCapabilities(GetClientCapabilitiesCallback callback) override {
    std::vector<blink::mojom::WebAuthnClientCapabilityPtr> capabilities;
    for (const char* name : {"conditionalCreate", "conditionalGet", "hybridTransport",
         "passkeyPlatformAuthenticator", "userVerifyingPlatformAuthenticator",
         "relatedOrigins", "signalAllAcceptedCredentials", "signalCurrentUserDetails",
         "signalUnknownCredential", "immediateGet"})
      capabilities.push_back(blink::mojom::WebAuthnClientCapability::New(name, false));
    std::move(callback).Run(std::move(capabilities));
  }
  void MakeCredential(blink::mojom::PublicKeyCredentialCreationOptionsPtr,
                      MakeCredentialCallback callback) override {
    std::move(callback).Run(blink::mojom::AuthenticatorStatus::NOT_ALLOWED_ERROR,
                            nullptr, nullptr);
  }
  void GetCredential(blink::mojom::GetCredentialOptionsPtr,
                     GetCredentialCallback callback) override {
    std::move(callback).Run(blink::mojom::GetCredentialResponse::NewGetAssertionResponse(
        blink::mojom::GetAssertionResponse::New(
            blink::mojom::AuthenticatorStatus::NOT_ALLOWED_ERROR, nullptr, nullptr)));
  }
  void Report(blink::mojom::PublicKeyCredentialReportOptionsPtr,
              ReportCallback callback) override {
    std::move(callback).Run(blink::mojom::AuthenticatorStatus::NOT_IMPLEMENTED, nullptr);
  }
  void Cancel() override {}

 private:
  mojo::ReceiverSet<blink::mojom::Authenticator> receivers_;
};
}
#endif

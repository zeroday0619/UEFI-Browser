#include "blink/workers.h"
#include "base/functional/bind.h"
#include "base/memory/weak_ptr.h"
#include "base/task/single_thread_task_runner.h"
#include "blink/resource_loader.h"
#include "firmware_threads.h"
#include "mojo/public/cpp/bindings/receiver.h"
#include "net/cookies/site_for_cookies.h"
#include "third_party/blink/public/common/loader/worker_main_script_load_parameters.h"
#include "third_party/blink/public/mojom/worker/dedicated_worker_host.mojom.h"
#include "third_party/blink/public/platform/web_dedicated_worker.h"
#include "third_party/blink/public/platform/web_url_request.h"
#include "third_party/blink/public/web/web_document.h"
#include "third_party/blink/public/web/web_local_frame.h"
#include "third_party/blink/renderer/platform/loader/fetch/url_loader/url_loader.h"
#include "third_party/blink/renderer/platform/loader/fetch/url_loader/url_loader_factory.h"
#include "third_party/blink/renderer/core/frame/web_local_frame_impl.h"
#include "third_party/blink/renderer/core/frame/local_frame.h"
#include "third_party/blink/renderer/core/frame/local_dom_window.h"
#include "third_party/blink/renderer/core/frame/policy_container.h"
#include "url/origin.h"

namespace uefi_browser {
namespace {
class WorkerLoaderFactory final : public blink::URLLoaderFactory {
public:
  WorkerLoaderFactory(SystemTable *table, std::shared_ptr<ResourceSecurityContext> security,
                      base::WaitableEvent *termination_event)
      : table_(table), security_(std::move(security)),
        termination_event_(termination_event) {}
  std::unique_ptr<blink::URLLoader> CreateURLLoader(
      const network::ResourceRequest &,
      scoped_refptr<base::SingleThreadTaskRunner>,
      scoped_refptr<base::SingleThreadTaskRunner> runner,
      mojo::PendingRemote<blink::mojom::blink::KeepAliveHandle>,
      blink::BackForwardCacheLoaderHelper *,
      blink::Vector<std::unique_ptr<blink::URLLoaderThrottle>>) override {
    return CreateResourceLoader(table_, security_, std::move(runner),
                                termination_event_);
  }

private:
  SystemTable *table_;
  std::shared_ptr<ResourceSecurityContext> security_;
  base::WaitableEvent *termination_event_;
};
class WorkerFetchContext final : public blink::WebWorkerFetchContext {
public:
  WorkerFetchContext(SystemTable *table, url::Origin origin,
                     net::SiteForCookies cookies, url::Origin top,
                     network::mojom::CrossOriginEmbedderPolicyValue policy)
      : WebWorkerFetchContext(blink::RendererPreferences()), table_(table),
        origin_(std::move(origin)), cookies_(std::move(cookies)),
        top_(std::move(top)), policy_(policy) {}
  void SetTerminateSyncLoadEvent(base::WaitableEvent *event) override {
    termination_event_ = event;
  }
  void InitializeOnWorkerThread(blink::AcceptLanguagesWatcher *) override {
    // Workers keep their ORB media history on their own loading sequence.
    security_ = std::make_shared<ResourceSecurityContext>(origin_, policy_);
    factory_ = std::make_unique<WorkerLoaderFactory>(table_, security_,
                                                     termination_event_);
  }
  blink::URLLoaderFactory *GetURLLoaderFactory() override {
    return factory_.get();
  }
  std::unique_ptr<blink::URLLoaderFactory> WrapURLLoaderFactory(
      blink::CrossVariantMojoRemote<
          network::mojom::URLLoaderFactoryInterfaceBase>) override {
    return std::make_unique<WorkerLoaderFactory>(table_, security_,
                                                 termination_event_);
  }
  void FinalizeRequest(blink::WebURLRequest &request) override {
    request.SetSiteForCookies(cookies_);
  }
  std::vector<std::unique_ptr<blink::URLLoaderThrottle>>
  CreateThrottles(const network::ResourceRequest &) override {
    return {};
  }
  blink::mojom::ControllerServiceWorkerMode
  GetControllerServiceWorkerMode() const override {
    return blink::mojom::ControllerServiceWorkerMode::kNoController;
  }
  net::SiteForCookies SiteForCookies() const override { return cookies_; }
  std::optional<blink::WebSecurityOrigin> TopFrameOrigin() const override {
    return blink::WebSecurityOrigin(top_);
  }
  blink::WebString GetAcceptLanguages() const override {
    return blink::WebString::FromUtf8("en-US,en");
  }
  bool IsDedicatedWorkerOrSharedWorkerFetchContext() const override {
    return true;
  }
  void SetIsOnSubframe(bool value) override { subframe_ = value; }
  bool IsOnSubframe() const override { return subframe_; }
  scoped_refptr<WorkerFetchContext> Clone() const {
    auto copy =
        base::AdoptRef(new WorkerFetchContext(table_, origin_, cookies_, top_, policy_));
    copy->subframe_ = subframe_;
    return copy;
  }

private:
  SystemTable *table_;
  url::Origin origin_;
  net::SiteForCookies cookies_;
  url::Origin top_;
  const network::mojom::CrossOriginEmbedderPolicyValue policy_;
  std::shared_ptr<ResourceSecurityContext> security_;
  bool subframe_ = false;
  base::WaitableEvent *termination_event_ = nullptr;
  std::unique_ptr<WorkerLoaderFactory> factory_;
};
class WorkerHost final : public blink::WebDedicatedWorkerHostFactoryClient,
                         public blink::mojom::DedicatedWorkerHost {
public:
  explicit WorkerHost(blink::WebDedicatedWorker *worker) : worker_(worker) {}
  void CreateWorkerHost(
      const blink::DedicatedWorkerToken &, const blink::WebURL &url,
      network::mojom::CredentialsMode,
      const blink::WebFetchClientSettingsObject &,
      blink::CrossVariantMojoRemote<blink::mojom::BlobURLTokenInterfaceBase>)
      override {
    if (firmware_threads_available() < 2) {
      base::SingleThreadTaskRunner::GetCurrentDefault()->PostTask(
          FROM_HERE, base::BindOnce(&WorkerHost::Fail, weak_.GetWeakPtr()));
      return;
    }
    worker_->OnWorkerHostCreated(mojo::NullRemote(),
                                 receiver_.BindNewPipeAndPassRemote(),
                                 blink::WebSecurityOrigin::Create(url));
    // Blink fetches and validates the script with the worker's own fetch
    // context.
    worker_->OnScriptLoadStarted(nullptr, mojo::NullRemote(), nullptr,
                                 mojo::NullReceiver(), mojo::NullReceiver());
  }
  scoped_refptr<blink::WebWorkerFetchContext> CloneWorkerFetchContext(
      blink::WebWorkerFetchContext *context,
      scoped_refptr<base::SingleThreadTaskRunner>) override {
    return static_cast<WorkerFetchContext *>(context)->Clone();
  }

private:
  void Fail() { worker_->OnScriptLoadStartFailed(); }
  blink::WebDedicatedWorker *worker_;
  mojo::Receiver<blink::mojom::DedicatedWorkerHost> receiver_{this};
  base::WeakPtrFactory<WorkerHost> weak_{this};
};
} // namespace
std::unique_ptr<blink::WebDedicatedWorkerHostFactoryClient>
CreateWorkerHost(blink::WebDedicatedWorker *worker) {
  return std::make_unique<WorkerHost>(worker);
}
scoped_refptr<blink::WebWorkerFetchContext>
CreateWorkerFetchContext(SystemTable *table, blink::WebLocalFrame *frame) {
  auto origin = url::Origin(frame->GetDocument().GetSecurityOrigin());
  auto top = frame->Top()->ToWebLocalFrame();
  auto top_origin =
      top ? url::Origin(top->GetDocument().GetSecurityOrigin()) : origin;
  return base::AdoptRef(new WorkerFetchContext(
      table, origin, net::SiteForCookies::FromOrigin(top_origin), top_origin,
      static_cast<blink::WebLocalFrameImpl*>(frame)->GetFrame()->DomWindow()
          ->GetPolicyContainer()->GetPolicies().cross_origin_embedder_policy.value));
}
} // namespace uefi_browser

#ifndef UEFI_BLINK_WORKERS_H
#define UEFI_BLINK_WORKERS_H
#include "base/memory/scoped_refptr.h"
#include "third_party/blink/public/platform/web_dedicated_worker_host_factory_client.h"
#include "third_party/blink/public/platform/web_worker_fetch_context.h"
#include "uefi.h"
#include <memory>
namespace blink {
class WebLocalFrame;
class WebDedicatedWorker;
} // namespace blink
namespace uefi_browser {
std::unique_ptr<blink::WebDedicatedWorkerHostFactoryClient>
CreateWorkerHost(blink::WebDedicatedWorker *worker);
scoped_refptr<blink::WebWorkerFetchContext>
CreateWorkerFetchContext(SystemTable *table, blink::WebLocalFrame *frame);
} // namespace uefi_browser
#endif

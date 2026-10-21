// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
#include "blink/resource_loader.h"
#include "blink/cookies.h"
#include "blink/devtools_resources.h"
#include "blink/response_headers.h"
#if defined(UEFI_RUNTIME)
#include "third_party/blink/renderer/core/loader/loader_factory_for_frame.h"
#include "third_party/blink/renderer/core/frame/local_dom_window.h"
#include "third_party/blink/renderer/core/dom/document.h"
#include "third_party/blink/renderer/core/frame/policy_container.h"
#include "third_party/blink/renderer/platform/supplementable.h"
#endif

#include <optional>
#include <algorithm>
#include <cstdio>
#include <string>
#include <utility>
#include <vector>
#include "base/check.h"
#include "base/synchronization/waitable_event.h"
#if defined(UEFI_RUNTIME)
#include "firmware_threads.h"
#endif
#include "base/functional/bind.h"
#include "base/memory/weak_ptr.h"
#include "base/strings/string_util.h"
#include "base/task/single_thread_task_runner.h"
#include "net/base/net_errors.h"
#include "net/http/http_util.h"
#include "net/base/port_util.h"
#include "net/url_request/redirect_info.h"
#include "third_party/blink/public/common/loader/referrer_utils.h"
#include "base/strings/string_split.h"
#include "services/network/cors/cors_util.h"
#include "services/network/cors/preflight_result.h"
#include "services/network/public/cpp/cors/cors.h"
#include "services/network/public/cpp/cross_origin_resource_policy.h"
#include "services/network/public/cpp/document_isolation_policy.h"
#include "services/network/public/mojom/url_response_head.mojom.h"
#include "services/network/public/cpp/resource_request_body.h"
#include "services/network/public/cpp/resource_request.h"
#include "third_party/blink/public/platform/resource_load_info_notifier_wrapper.h"
#include "third_party/blink/public/platform/web_url_error.h"
#include "third_party/blink/public/platform/web_url_response.h"
#include "third_party/blink/renderer/platform/blob/blob_data.h"
#include "third_party/blink/renderer/platform/loader/fetch/url_loader/url_loader.h"
#include "third_party/blink/renderer/platform/loader/fetch/url_loader/url_loader_client.h"
#include "third_party/blink/renderer/platform/weborigin/kurl.h"
#include "third_party/blink/renderer/platform/weborigin/security_origin.h"

namespace uefi_browser {
namespace {
class ResourceLoader final : public blink::URLLoader {
 public:
  ResourceLoader(SystemTable* table, std::shared_ptr<ResourceSecurityContext> security,
                 scoped_refptr<base::SingleThreadTaskRunner> task_runner,
                 base::WaitableEvent* termination_event)
      : termination_event_(termination_event), table_(table), security_(std::move(security)),
        origin_(security_->origin), task_runner_(std::move(task_runner)) {
    CHECK(table_);
    CHECK(task_runner_);
  }
  ~ResourceLoader() override { resource_async_release(job_); resource_release(&resource_); }

  void LoadSynchronously(
      std::unique_ptr<network::ResourceRequest> request,
      scoped_refptr<const blink::SecurityOrigin>, bool, bool, base::TimeDelta timeout,
      blink::URLLoaderClient*, blink::WebURLResponse& response,
      std::optional<blink::WebURLError>& error,
      scoped_refptr<blink::SharedBuffer>& data, int64_t& encoded_data_length,
      uint64_t& encoded_body_length, scoped_refptr<blink::BlobDataHandle>& blob,
      std::unique_ptr<blink::ResourceLoadInfoNotifierWrapper>) override {
    if (!termination_event_) {
      data = nullptr;
      blob = nullptr;
      encoded_data_length = encoded_body_length = 0;
      error.emplace(net::ERR_NOT_IMPLEMENTED, blink::WebURL(blink::KURL(request->url)));
      return;
    }
    class Client final : public blink::URLLoaderClient {
     public:
      blink::WebURLResponse response;
      std::optional<blink::WebURLError> error;
      scoped_refptr<blink::SharedBuffer> data = blink::SharedBuffer::Create();
      int64_t encoded = 0;
      uint64_t body = 0;
      void DidReceiveResponse(const blink::WebURLResponse& value,
          std::variant<mojo::ScopedDataPipeConsumerHandle, blink::SegmentedBuffer> buffer,
          std::optional<mojo_base::BigBuffer>) override {
        response = value;
        if (auto* segments = std::get_if<blink::SegmentedBuffer>(&buffer))
          for (auto segment : *segments) data->Append(segment);
      }
#if defined(UEFI_RUNTIME)
      void DidReceiveData(base::span<const char> value) override { data->Append(value); }
#endif
      void DidFail(const blink::WebURLError& value, base::TimeTicks, int64_t, uint64_t, int64_t) override { error = value; }
      void DidFinishLoading(base::TimeTicks, int64_t length, uint64_t body_length, int64_t) override {
        encoded = length; body = body_length;
      }
    } client;
    CHECK(!request_);
    synchronous_ = true;
    request_ = std::move(request);
    original_url_ = request_->url;
    request_origin_ = origin_;
    client_ = &client;
    auto deadline = base::TimeTicks::Now() +
        (timeout.is_positive() ? std::min(timeout, base::Seconds(30)) : base::Seconds(30));
    while (client_) {
      if (termination_event_ && termination_event_->IsSignaled()) { Fail(net::ERR_ABORTED); break; }
      if (base::TimeTicks::Now() >= deadline) { Fail(net::ERR_TIMED_OUT); break; }
      Run();
#if defined(UEFI_RUNTIME)
      if (client_) firmware_threads_yield();
#endif
    }
    response = client.response;
    error = client.error;
    data = error ? nullptr : client.data;
    encoded_data_length = client.encoded;
    encoded_body_length = client.body;
    blob = nullptr;
  }

  void LoadAsynchronously(
      std::unique_ptr<network::ResourceRequest> request,
      scoped_refptr<const blink::SecurityOrigin>, bool,
      std::unique_ptr<blink::ResourceLoadInfoNotifierWrapper>,
      blink::CodeCacheHost*, blink::URLLoaderClient* client) override {
    CHECK(!request_);
    CHECK(client);
    request_ = std::move(request);
    original_url_ = request_->url;
    client_ = client;
    request_origin_ = origin_;
    Schedule();
  }

  void Freeze(blink::LoaderFreezeMode mode) override {
    frozen_ = mode != blink::LoaderFreezeMode::kNone;
    Schedule();
  }
  void DidChangePriority(blink::WebURLRequest::Priority, int) override {}
  scoped_refptr<base::SingleThreadTaskRunner> GetTaskRunnerForBodyLoader() override {
    return task_runner_;
  }

 private:
  void Schedule() {
    if (synchronous_ || !client_ || frozen_ || pending_)
      return;
    pending_ = true;
    task_runner_->PostDelayedTask(FROM_HERE, base::BindOnce(&ResourceLoader::Run,
        weak_factory_.GetWeakPtr()), base::Milliseconds(2));
  }

  bool CrossOrigin() const {
    return cors_tainted_ || !origin_.IsSameOriginWith(request_->url);
  }
  bool CredentialsIncluded() const {
    if (CrossOrigin() && request_->mode == network::mojom::RequestMode::kNoCors &&
        security_->embedder_policy.value == network::mojom::CrossOriginEmbedderPolicyValue::kCredentialless)
      return false;
    return request_->credentials_mode != network::mojom::CredentialsMode::kOmit &&
        (request_->credentials_mode == network::mojom::CredentialsMode::kInclude || !CrossOrigin());
  }
  bool CookiesAllowed() const {
    return CredentialsIncluded() && request_->site_for_cookies.IsFirstParty(request_->url);
  }
  int ValidateRequest() const {
    if (IsDevToolsHost(request_->url) &&
        (!IsDevToolsOrigin(request_->url) || !IsDevToolsOrigin(origin_.GetURL()) ||
         request_->method != "GET"))
      return net::ERR_BLOCKED_BY_CLIENT;
    if (!request_->url.is_valid() || !request_->url.SchemeIsHTTPOrHTTPS() ||
        request_->url.has_username() || request_->url.has_password())
      return net::ERR_INVALID_URL;
    if (!net::IsPortAllowedForScheme(request_->url.EffectiveIntPort(), request_->url.scheme()))
      return net::ERR_UNSAFE_PORT;
    if (origin_.opaque() || !request_->request_initiator ||
        !origin_.IsSameOriginWith(*request_->request_initiator))
      return net::ERR_BLOCKED_BY_CLIENT;
    if (origin_.GetURL().SchemeIs("https") && !request_->url.SchemeIs("https"))
      return net::ERR_BLOCKED_BY_CLIENT;
    if (CrossOrigin() && request_->mode == network::mojom::RequestMode::kSameOrigin)
      return net::ERR_BLOCKED_BY_CLIENT;
    if (!resource_method_supported(request_->method.c_str()) || request_->keepalive ||
        !request_->cors_exempt_headers.IsEmpty())
      return net::ERR_NOT_IMPLEMENTED;
    for (const auto& header : request_->headers.GetHeaderVector()) {
      if (base::EqualsCaseInsensitiveASCII(header.key, "cookie") ||
          base::EqualsCaseInsensitiveASCII(header.key, "proxy-authorization"))
        return net::ERR_BLOCKED_BY_CLIENT;
    }
    return net::OK;
  }
  int Prepare() {
    int result = ValidateRequest();
    if (result != net::OK) return result;
    body_.clear();
    if (request_->request_body) {
      for (const auto& element : *request_->request_body->elements()) {
        const auto* bytes = element.TryAs<network::DataElementBytes>();
        if (!bytes) return net::ERR_NOT_IMPLEMENTED;
        if (bytes->bytes().size() > RESOURCE_BODY_LIMIT - body_.size())
          return net::ERR_FILE_TOO_BIG;
        body_.insert(body_.end(), bytes->bytes().begin(), bytes->bytes().end());
      }
    }
    if (!body_.empty() && (request_->method == "GET" || request_->method == "HEAD"))
      return net::ERR_INVALID_ARGUMENT;
    cors_tainted_ = CrossOrigin();
    cors_ = cors_tainted_ && network::cors::IsCorsEnabledRequestMode(request_->mode);
    unsafe_headers_ = network::cors::CorsUnsafeNotForbiddenRequestHeaderNames(
        request_->headers.GetHeaderVector(), false, false);
    std::sort(unsafe_headers_.begin(), unsafe_headers_.end());
    preflight_ = cors_ && (!network::cors::IsCorsSafelistedMethod(request_->method) ||
        !unsafe_headers_.empty() || request_->mode == network::mojom::RequestMode::kCorsWithForcedPreflight);
    prepared_ = true;
    return net::OK;
  }
  std::optional<std::string> ResponseHeader(const char* name) const {
    std::optional<std::string> value;
    for (size_t index = 0; index < resource_.header_count; ++index) {
      const auto& header = resource_.headers[index];
      if (!base::EqualsCaseInsensitiveASCII(name, header.name)) continue;
      if (value) *value += ", " + std::string(header.value);
      else value = header.value;
    }
    return value;
  }
  bool CheckCors() const {
    return network::cors::CheckAccess(request_->url,
        ResponseHeader("access-control-allow-origin"), ResponseHeader("access-control-allow-credentials"),
        request_->credentials_mode, request_origin_).has_value();
  }
  bool CheckPreflight() const {
    if (resource_.status_code < 200 || resource_.status_code >= 300 || !CheckCors()) return false;
    std::optional<network::mojom::CorsError> error;
    auto result = network::cors::PreflightResult::Create(request_->credentials_mode,
        ResponseHeader("access-control-allow-methods"), ResponseHeader("access-control-allow-headers"),
        ResponseHeader("access-control-max-age"), &error);
    return result && !result->EnsureAllowedCrossOriginMethod(request_->method, true) &&
        !result->EnsureAllowedCrossOriginHeaders(request_->headers, false,
            network::cors::NonWildcardRequestHeadersSupport(true));
  }
  int CheckResponsePolicy(bool redirect) {
    auto response = network::mojom::URLResponseHead::New();
    response->headers = ResponseHeaders(resource_);
    if (!response->headers) return net::ERR_INVALID_RESPONSE;
    response->content_length = resource_.length;
    response->request_include_credentials = CredentialsIncluded();
    if (network::CrossOriginResourcePolicy::IsBlocked(request_->url, original_url_,
        origin_, *response, request_->mode, request_->destination,
        security_->embedder_policy, nullptr, network::DocumentIsolationPolicy(), nullptr)) {
      std::puts("NETWORK POLICY: CORP blocked");
      return net::ERR_BLOCKED_BY_RESPONSE;
    }
    // Redirect bodies are never delivered, but every redirect must pass CORP.
    if (redirect || manual_redirect_) return net::OK;
    using Analyzer = network::orb::ResponseAnalyzer;
    auto analyzer = Analyzer::Create(&security_->media_responses);
    auto decision = analyzer->Init(request_->url, request_origin_, request_->mode,
                                    request_->destination, *response);
    if (decision == Analyzer::Decision::kSniffMore) {
      const size_t length = std::min<size_t>(1024, resource_.length);
      decision = analyzer->Sniff(length ? std::string_view(
          reinterpret_cast<const char*>(resource_.body), length) : std::string_view());
    }
    if (decision == Analyzer::Decision::kSniffMore)
      decision = analyzer->HandleEndOfSniffableResponseBody();
    // A bounded cache may forget previous media, which only makes later checks stricter.
    if (security_->media_responses.size() > 256) security_->media_responses.clear();
    if (decision != Analyzer::Decision::kBlock) return net::OK;
    std::puts("NETWORK POLICY: ORB blocked");
    if (analyzer->ShouldHandleBlockedResponseAs() == Analyzer::BlockedResponseHandling::kNetworkError)
      return net::ERR_BLOCKED_BY_ORB;
    orb_empty_ = true;
    return net::OK;
  }
  void StartRequest() {
    net::HttpRequestHeaders outgoing;
    if (preflight_) {
      outgoing.SetHeader("Access-Control-Request-Method", request_->method);
      if (!unsafe_headers_.empty())
        outgoing.SetHeader("Access-Control-Request-Headers", base::JoinString(unsafe_headers_, ","));
    } else {
      outgoing = request_->headers;
      if (CookiesAllowed()) {
        auto cookie = CookieHeader(request_->url, false);
        if (!cookie.empty()) outgoing.SetHeader("Cookie", cookie);
      }
    }
    // Origin is derived from the initiating document and redirect chain.
    outgoing.RemoveHeader("Origin");
    if (cors_ || (request_->method != "GET" && request_->method != "HEAD"))
      outgoing.SetHeader("Origin", request_origin_.Serialize());
    outgoing.SetHeader("User-Agent", firmware_browser_user_agent_for_url(origin_.GetURL().spec().c_str()));
    std::vector<Header> headers;
    for (const auto& header : outgoing.GetHeaderVector())
      headers.push_back({const_cast<char*>(header.key.c_str()), const_cast<char*>(header.value.c_str())});
    GURL::Replacements replacements;
    replacements.ClearRef();
    auto url = request_->url.ReplaceComponents(replacements);
    ResourceRequestSpec spec{url.spec().c_str(), preflight_ ? "OPTIONS" : request_->method.c_str(),
        headers.data(), headers.size(), preflight_ ? nullptr : body_.data(), preflight_ ? 0 : body_.size()};
    job_ = resource_async_request(table_, &spec, 1);
  }
  blink::WebURLResponse Response() const {
    blink::WebURLResponse response{blink::WebURL(blink::KURL(request_->url))};
    response.SetHttpStatusCode(resource_.status_code);
    // The firmware response does not expose a reason phrase; Mojo requires a non-null string.
    response.SetHttpStatusText(blink::WebString::FromUtf8(""));
    response.SetExpectedContentLength(orb_empty_ ? 0 : resource_.length);
    response.SetType(manual_redirect_ ? network::mojom::FetchResponseType::kOpaqueRedirect :
        cors_ ? network::mojom::FetchResponseType::kCors : CrossOrigin() ?
        network::mojom::FetchResponseType::kOpaque : network::mojom::FetchResponseType::kBasic);
    // Chromium's legacy opaque-fetch result strips all headers and payload bytes.
    if (orb_empty_) return response;
    std::string mime_type, charset;
    if (auto headers = ResponseHeaders(resource_))
      headers->GetMimeTypeAndCharset(&mime_type, &charset);
    response.SetMimeType(blink::WebString::FromUtf8(mime_type));
    response.SetTextEncodingName(blink::WebString::FromUtf8(charset));
    for (size_t index = 0; index < resource_.header_count; ++index) {
      const Header& header = resource_.headers[index];
      response.AddHttpHeaderField(blink::WebString::FromLatin1(header.name), blink::WebString::FromLatin1(header.value));
    }
    std::vector<blink::WebString> exposed;
    auto names = ResponseHeader("access-control-expose-headers");
    if (names) {
      auto tokens = base::SplitString(*names, ",", base::TRIM_WHITESPACE, base::SPLIT_WANT_NONEMPTY);
      bool valid = std::all_of(tokens.begin(), tokens.end(), [](const auto& token) { return net::HttpUtil::IsToken(token); });
      if (valid) for (const auto& token : tokens) {
        if (token == "*" && request_->credentials_mode != network::mojom::CredentialsMode::kInclude) {
          for (size_t index = 0; index < resource_.header_count; ++index)
            exposed.push_back(blink::WebString::FromLatin1(resource_.headers[index].name));
        } else exposed.push_back(blink::WebString::FromUtf8(token));
      }
    }
    response.SetCorsExposedHeaderNames(exposed);
    return response;
  }
  bool FollowRedirect() {
    const char* location = resource_header(&resource_, "location");
    if (!location) return false;
    if (request_->redirect_mode == network::mojom::RedirectMode::kError) {
      Fail(net::ERR_FAILED); return true;
    }
    if (request_->redirect_mode == network::mojom::RedirectMode::kManual) {
      manual_redirect_ = true; return false;
    }
    if (++redirects_ > 20) { Fail(net::ERR_TOO_MANY_REDIRECTS); return true; }
    GURL next = request_->url.Resolve(location);
    if (!next.is_valid() || !next.SchemeIsHTTPOrHTTPS() || next.has_username() || next.has_password() ||
        (request_->url.SchemeIs("https") && !next.SchemeIs("https"))) {
      Fail(net::ERR_UNSAFE_REDIRECT); return true;
    }
    auto redirect = net::RedirectInfo::ComputeRedirectInfo(request_->method, request_->url,
        request_->site_for_cookies, net::RedirectInfo::FirstPartyURLPolicy::NEVER_CHANGE_URL,
        request_->referrer_policy, request_->referrer.spec(), request_->request_initiator,
        resource_.status_code, next, ResponseHeader("referrer-policy"), false);
    next = redirect.new_url;
    const std::string& method = redirect.new_method;
    auto weak = weak_factory_.GetWeakPtr();
    bool raw_headers = false;
    std::vector<std::string> removed;
    net::HttpRequestHeaders modified;
    bool follow = client_->WillFollowRedirect(blink::WebURL(blink::KURL(next)), request_->site_for_cookies,
        blink::WebString::FromUtf8(redirect.new_referrer), blink::ReferrerUtils::NetToMojoReferrerPolicy(redirect.new_referrer_policy),
        blink::WebString::FromUtf8(method), Response(), raw_headers, &removed, modified, false);
    if (!weak) return true;
    if (!follow) { client_ = nullptr; return true; }
    if (!url::Origin::Create(request_->url).IsSameOriginWith(next)) {
      request_->headers.RemoveHeader("Authorization");
      if (!origin_.IsSameOriginWith(request_->url)) request_origin_ = url::Origin();
    }
    if (method != request_->method) {
      request_->request_body = nullptr;
      for (const char* name : {"Content-Type", "Content-Encoding", "Content-Language", "Content-Location"})
        request_->headers.RemoveHeader(name);
    }
    for (const auto& name : removed) request_->headers.RemoveHeader(name);
    for (const auto& header : modified.GetHeaderVector()) request_->headers.SetHeader(header.key, header.value);
    request_->referrer = GURL(redirect.new_referrer);
    request_->referrer_policy = redirect.new_referrer_policy;
    request_->method = method;
    request_->url = std::move(next);
    resource_release(&resource_);
    prepared_ = false;
    Schedule();
    return true;
  }

  void Fail(int reason) {
    std::printf("BLINK: resource failed error=%d url=%s\n", reason,
                request_->url.GetWithEmptyPath().spec().c_str());
    auto* client = std::exchange(client_, nullptr);
    blink::WebURLError error(reason, blink::WebURL(blink::KURL(request_->url)));
    client->DidFail(error, base::TimeTicks::Now(), 0, 0, 0);
  }

  void Run() {
    pending_ = false;
    if (frozen_ || !client_)
      return;
    if (!downloaded_) {
      if (!prepared_) {
        int validation = Prepare();
        if (validation != net::OK) { Fail(validation); return; }
      }
      if (IsDevToolsHost(request_->url)) {
        if (LoadDevToolsResource(request_->url, &resource_)) {
          Fail(net::ERR_FAILED); return;
        }
      } else {
      if (!job_) StartRequest();
      if (!job_) { Fail(net::ERR_INSUFFICIENT_RESOURCES); return; }
      Status status;
      if (!resource_async_result(job_, &status, &resource_)) { Schedule(); return; }
      resource_async_release(job_);
      job_ = nullptr;
      if (status) {
        std::printf("NETWORK: transport failed status=0x%llx origin=%s\n",
            static_cast<unsigned long long>(status), request_->url.GetWithEmptyPath().spec().c_str());
        Fail(net::ERR_FAILED); return;
      }
      if (preflight_) {
        if (!CheckPreflight()) { Fail(net::ERR_BLOCKED_BY_RESPONSE); return; }
        resource_release(&resource_);
        preflight_ = false;
        Schedule();
        return;
      }
      if (CookiesAllowed()) StoreResponseCookies(request_->url, resource_);
      if (cors_ && !CheckCors()) { Fail(net::ERR_BLOCKED_BY_RESPONSE); return; }
      const int policy = CheckResponsePolicy(resource_is_redirect(resource_.status_code));
      if (policy != net::OK) { Fail(policy); return; }
      if (resource_is_redirect(resource_.status_code) && FollowRedirect()) return;
      }
      downloaded_ = true;
      const char* type = resource_header(&resource_, "content-type");
      std::printf("BLINK: resource %u type=%s bytes=%zu\n",
                  resource_.status_code, type ? type : "", resource_.length);
      if (!body_.empty()) {
        auto weak = weak_factory_.GetWeakPtr();
        client_->DidSendData(body_.size(), body_.size());
        if (!weak || frozen_) return;
      }
    }
    if (!response_sent_) {
      auto response = Response();
      blink::SegmentedBuffer body;
      if (!manual_redirect_ && !orb_empty_)
        body.Append(base::span<const uint8_t>(resource_.body, resource_.length));
      response_sent_ = true;
      auto weak = weak_factory_.GetWeakPtr();
      client_->DidReceiveResponse(response, std::move(body), std::nullopt);
      // ResourceLoader callbacks can synchronously dispose or freeze the loader.
      if (!weak || frozen_)
        return;
    }

    auto* client = std::exchange(client_, nullptr);
    client->DidFinishLoading(base::TimeTicks::Now(),
        blink::URLLoaderClient::kUnknownEncodedDataLength,
        orb_empty_ ? 0 : resource_.encoded_body_length, orb_empty_ ? 0 : resource_.length);
  }

  base::WaitableEvent* termination_event_;
  bool synchronous_ = false;
  SystemTable* table_;
  const std::shared_ptr<ResourceSecurityContext> security_;
  const url::Origin origin_;
  GURL original_url_;
  bool orb_empty_ = false;
  scoped_refptr<base::SingleThreadTaskRunner> task_runner_;
  std::unique_ptr<network::ResourceRequest> request_;
  blink::URLLoaderClient* client_ = nullptr;
  ResourceResponse resource_{};
  ResourceJob* job_ = nullptr;
  url::Origin request_origin_;
  std::vector<uint8_t> body_;
  std::vector<std::string> unsafe_headers_;
  unsigned redirects_ = 0;
  bool prepared_ = false, preflight_ = false, cors_ = false, cors_tainted_ = false, manual_redirect_ = false;
  bool frozen_ = false;
  bool pending_ = false;
  bool downloaded_ = false;
  bool response_sent_ = false;
  base::WeakPtrFactory<ResourceLoader> weak_factory_{this};
};
}
#if defined(UEFI_RUNTIME)
namespace {
SystemTable* resource_system_table = nullptr;
size_t resource_loader_owners = 0;
class DocumentResourceSecurity final
    : public blink::GarbageCollected<DocumentResourceSecurity>,
      public blink::Supplement<blink::Document> {
 public:
  static constexpr char kSupplementName[] = "FirmwareResourceSecurity";
  explicit DocumentResourceSecurity(blink::LocalDOMWindow& window)
      : blink::Supplement<blink::Document>(*window.document()),
        context(std::make_shared<ResourceSecurityContext>(window.GetSecurityOrigin()->ToUrlOrigin(),
            window.GetPolicyContainer()->GetPolicies().cross_origin_embedder_policy.value)) {}
  void Trace(blink::Visitor* visitor) const override {
    blink::Supplement<blink::Document>::Trace(visitor);
  }
  const std::shared_ptr<ResourceSecurityContext> context;
};
std::unique_ptr<blink::URLLoader> CreateFrameResourceLoader(
    blink::LocalDOMWindow& window,
    scoped_refptr<base::SingleThreadTaskRunner> runner) {
  CHECK(resource_system_table);
  auto* security = blink::Supplement<blink::Document>::From<DocumentResourceSecurity>(*window.document());
  if (!security) {
    security = blink::MakeGarbageCollected<DocumentResourceSecurity>(window);
    blink::Supplement<blink::Document>::ProvideTo(*window.document(), security);
  }
  return CreateResourceLoader(resource_system_table, security->context,
                              std::move(runner));
}
}
void InstallResourceLoader(SystemTable* table) {
  CHECK(table);
  CHECK(!resource_system_table || resource_system_table == table);
  if (resource_loader_owners++) return;
  resource_system_table = table;
  blink::LoaderFactoryForFrame::SetFirmwareLoaderFactory(CreateFrameResourceLoader);
}
void UninstallResourceLoader() {
  CHECK(resource_loader_owners);
  if (--resource_loader_owners) return;
  blink::LoaderFactoryForFrame::SetFirmwareLoaderFactory(nullptr);
  resource_system_table = nullptr;
}
#endif
std::unique_ptr<blink::URLLoader> CreateResourceLoader(
    SystemTable* table, std::shared_ptr<ResourceSecurityContext> security,
    scoped_refptr<base::SingleThreadTaskRunner> task_runner,
    base::WaitableEvent* termination_event) {
  return std::make_unique<ResourceLoader>(table, std::move(security),
                                        std::move(task_runner), termination_event);
}
}

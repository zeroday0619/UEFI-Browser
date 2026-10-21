// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
#include "blink/navigation.h"
#include "blink/cookies.h"
#include "blink/browser_settings.h"
#include "blink/devtools_resources.h"
#include "blink/response_headers.h"
#include "services/network/public/cpp/cross_origin_embedder_policy.h"
#include "services/network/public/cpp/cross_origin_embedder_policy_parser.h"

#include <algorithm>
#include <cstdio>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "base/containers/span.h"
#include "base/memory/weak_ptr.h"
#include "base/functional/bind.h"
#include "base/location.h"
#include "base/task/single_thread_task_runner.h"
#include "base/time/time.h"
#include "cc/paint/skia_paint_canvas.h"
#include "net/http/http_util.h"
#include "net/base/port_util.h"
#include "base/check.h"
#include "services/network/public/cpp/web_sandbox_flags.h"
#include "content/renderer/content_security_policy_util.h"
#include "services/network/public/cpp/content_security_policy/content_security_policy.h"
#include "services/network/public/mojom/content_security_policy.mojom.h"
#include "third_party/blink/public/common/metrics/document_update_reason.h"
#include "third_party/blink/public/platform/web_navigation_body_loader.h"
#include "third_party/blink/public/platform/web_string.h"
#include "third_party/blink/public/platform/web_url.h"
#include "third_party/blink/public/platform/web_url_response.h"
#include "third_party/blink/public/web/web_frame_widget.h"
#include "third_party/blink/public/web/web_navigation_control.h"
#include "third_party/blink/public/web/web_navigation_params.h"
#include "third_party/blink/public/web/web_view.h"
#include "third_party/blink/renderer/platform/weborigin/kurl.h"
#include "ui/gfx/geometry/rect.h"
#include "url/gurl.h"

namespace uefi_browser {
namespace {
// Network documents must retain normal navigation security checks. Blink's
// FillStaticResponse helper marks input as static data and bypasses checks.
class NetworkBodyLoader final : public blink::WebNavigationBodyLoader {
public:
  explicit NetworkBodyLoader(const ResourceResponse &response)
      : body_(response.length), encoded_length_(response.encoded_body_length) {
    if (response.length)
      std::copy_n(reinterpret_cast<const char *>(response.body),
                  response.length, body_.begin());
  }
  BodyLoaderType GetType() const override { return BodyLoaderType::kNetwork; }
  void SetDefersLoading(blink::WebLoaderFreezeMode mode) override {
    frozen_ = mode != blink::WebLoaderFreezeMode::kNone;
    Schedule();
  }
  void StartLoadingBody(Client *client) override {
    client_ = client;
    Schedule();
  }

private:
  void Schedule() {
    if (!client_ || frozen_ || finished_ || pending_)
      return;
    pending_ = true;
    CHECK(base::SingleThreadTaskRunner::GetCurrentDefault()->PostTask(
        FROM_HERE, base::BindOnce(&NetworkBodyLoader::Deliver,
                                  weak_factory_.GetWeakPtr())));
  }
  void Deliver() {
    pending_ = false;
    if (!client_ || frozen_ || finished_)
      return;
    if (offset_ < body_.size()) {
      const size_t length = std::min<size_t>(16384, body_.size() - offset_);
      auto chunk = base::span<const char>(body_).subspan(offset_, length);
      offset_ += length;
      auto weak = weak_factory_.GetWeakPtr();
      client_->BodyDataReceived(chunk);
      if (weak)
        Schedule();
      return;
    }
    finished_ = true;
    // Completion runs separately so parsing can establish its pending-script state.
    client_->BodyLoadingFinished(base::TimeTicks::Now(), encoded_length_,
                                 encoded_length_, body_.size(), std::nullopt);
  }
  std::vector<char> body_;
  size_t encoded_length_;
  Client *client_ = nullptr;
  bool frozen_ = false;
  size_t offset_ = 0;
  bool pending_ = false;
  bool finished_ = false;
  base::WeakPtrFactory<NetworkBodyLoader> weak_factory_{this};
};

struct ResponseOwner {
  ResourceResponse response{};
  ~ResponseOwner() { resource_release(&response); }
};

blink::WebURLResponse ConvertResponse(const GURL &url,
                                      const ResourceResponse &resource,
                                      const net::HttpResponseHeaders& headers) {
  blink::WebURLResponse response{blink::WebURL(blink::KURL(url))};
  response.SetHttpStatusCode(resource.status_code);
  // The firmware response does not expose a reason phrase; Mojo requires a non-null string.
  response.SetHttpStatusText(blink::WebString::FromUtf8(""));
  response.SetExpectedContentLength(resource.length);
  std::string mime_type;
  std::string charset;
  for (size_t index = 0; index < resource.header_count; ++index) {
    const Header &header = resource.headers[index];
    response.AddHttpHeaderField(blink::WebString::FromLatin1(header.name),
                                blink::WebString::FromLatin1(header.value));
  }
  headers.GetMimeTypeAndCharset(&mime_type, &charset);
  response.SetMimeType(blink::WebString::FromUtf8(mime_type));
  response.SetTextEncodingName(blink::WebString::FromUtf8(charset));
  return response;
}
} // namespace

namespace {
class AsyncNavigation final : public NavigationRequest {
 public:
  AsyncNavigation(SystemTable* system, blink::WebNavigationControl& navigation,
      const char* address, std::unique_ptr<blink::WebPolicyContainer> container,
      blink::WebFrameLoadType load_type, blink::WebHistoryItem item,
      base::OnceCallback<void(Status)> callback, std::unique_ptr<blink::WebNavigationParams> initial, bool allow_cookies)
      : table(system), frame(navigation), url(address), policy(std::move(container)),
        cookies_allowed(allow_cookies), parameters(initial ? std::move(initial) : std::make_unique<blink::WebNavigationParams>()), completion(std::move(callback)) {
    parameters->navigation_timings.navigation_start = base::TimeTicks::Now();
    parameters->frame_load_type = load_type;
    parameters->history_item = item;
    parameters->url = blink::WebURL(blink::KURL(url));
    if (parameters->http_method.IsEmpty()) parameters->http_method = blink::WebString::FromAscii("GET");
    if (!parameters->http_body.IsNull()) {
      for (size_t index = 0; index < parameters->http_body.ElementCount(); ++index) {
        blink::WebHTTPBody::Element element;
        if (!parameters->http_body.ElementAt(index, element) || element.type != blink::HTTPBodyElementType::kTypeData ||
            element.data.size() > RESOURCE_BODY_LIMIT - body.size()) { invalid_body = true; break; }
        element.data.ForEachSegment([&](base::span<const uint8_t> segment, size_t) {
          body.insert(body.end(), segment.begin(), segment.end());
          return true;
        });
      }
    }
    Schedule();
  }
  ~AsyncNavigation() override { resource_async_release(job); }
 private:
  void Schedule() {
    base::SingleThreadTaskRunner::GetCurrentDefault()->PostDelayedTask(FROM_HERE,
        base::BindOnce(&AsyncNavigation::Run, weak.GetWeakPtr()), base::Milliseconds(2));
  }
  void Finish(Status result) {
    if (completion) std::move(completion).Run(result);
  }
  void Run() {
    auto& timings = parameters->navigation_timings;
    const std::string method = parameters->http_method.Utf8();
    if (invalid_body || (method != "GET" && method != "POST") || (method == "GET" && !body.empty())) {
      Finish(ERROR(3)); return;
    }
  if (url.spec() == "about:blank" || IsBrowserPage(url)) {
    parameters->policy_container = std::move(policy);
    const std::string blank = IsBrowserPage(url) ? BrowserPage(url) : "<!doctype html><title>UEFI BROWSER</title>";
    blink::WebNavigationParams::FillStaticResponse(parameters.get(),
        blink::WebString::FromAscii("text/html"), blink::WebString::FromAscii("UTF-8"),
        base::span(blank.data(), blank.size()));
    auto alive = weak.GetWeakPtr();
    frame.CommitNavigation(std::move(parameters), nullptr);
    if (alive) Finish(0);
    return;
  }
  {
    if (!url.is_valid() || !url.SchemeIsHTTPOrHTTPS() || url.has_username() ||
        url.has_password() || !net::IsPortAllowedForScheme(url.EffectiveIntPort(), url.scheme()))
      { Finish(ERROR(2)); return; }
    GURL::Replacements replacements;
    replacements.ClearRef();
    GURL request_url = url.ReplaceComponents(replacements);
    ResponseOwner owner;
    if (IsDevToolsHost(request_url)) {
      if (method != "GET") { Finish(ERROR(3)); return; }
      timings.fetch_start = base::TimeTicks::Now();
      Status result = LoadDevToolsResource(request_url, &owner.response);
      if (result) { Finish(result); return; }
    } else {
    if (!job) {
      timings.fetch_start = base::TimeTicks::Now();
      // Main document navigation bypasses Blink's subresource hint generation.
      // Child navigation lacks a trusted top-level cookie context and excludes credentials.
      const bool same_origin = !parameters->requestor_origin.IsNull() &&
          parameters->requestor_origin.IsSameOriginWith(blink::WebSecurityOrigin::Create(blink::WebURL(blink::KURL(request_url))));
      std::string cookie_line = cookies_allowed && (method == "GET" || same_origin) ?
          CookieHeader(request_url, false, method == "GET") : std::string();
      Header hints[6];
      size_t hint_count = 0;
      if (request_url.SchemeIs("https")) {
        for (const char* name : {"Sec-CH-UA", "Sec-CH-UA-Mobile", "Sec-CH-UA-Platform"}) {
          const char* value = firmware_browser_client_hint_for_url(name, request_url.spec().c_str());
          if (value) hints[hint_count++] = {const_cast<char*>(name), const_cast<char*>(value)};
        }
      }
      if (!cookie_line.empty()) hints[hint_count++]={const_cast<char*>("Cookie"),cookie_line.data()};
      const std::string content_type = parameters->http_content_type.Utf8();
      std::string origin = parameters->requestor_origin.IsNull() || origin_tainted ? "null" : parameters->requestor_origin.ToString().Utf8();
      if (method == "POST") {
        if (!content_type.empty()) hints[hint_count++] = {const_cast<char*>("Content-Type"), const_cast<char*>(content_type.c_str())};
        hints[hint_count++] = {const_cast<char*>("Origin"), origin.data()};
      }
      ResourceRequestSpec spec{request_url.spec().c_str(), method.c_str(), hints, hint_count, body.data(), body.size()};
      job = resource_async_request(table, &spec, 4);
      if (!job) { Finish(ERROR(9)); return; }
    }
    Status result;
    if (!resource_async_result(job, &result, &owner.response)) { Schedule(); return; }
    resource_async_release(job); job = nullptr;
    if (result) { Finish(result); return; }
    }
    const ResourceResponse &resource = owner.response;
    if (cookies_allowed && !IsDevToolsHost(request_url)) StoreResponseCookies(request_url,resource);
    std::printf("BLINK: navigation HTTP status=%u bytes=%zu\n",
                resource.status_code, resource.length);
    auto response_headers = ResponseHeaders(resource);
    if (!response_headers) { Finish(ERROR(3)); return; }
    auto response = ConvertResponse(url, resource, *response_headers);
    if (resource_is_redirect(resource.status_code)) {
      const char *location = resource_header(&resource, "location");
      if (!location || redirects == 10)
        { Finish(ERROR(24)); return; }
      GURL next = url.Resolve(location);
      if (!next.is_valid() || !next.SchemeIsHTTPOrHTTPS() || next.has_username() || next.has_password())
        { Finish(ERROR(2)); return; }
      if (url.SchemeIs("https") && !next.SchemeIs("https"))
        { Finish(ERROR(26)); return; }
      if (timings.redirect_start.is_null()) timings.redirect_start = timings.fetch_start;
      timings.redirect_end = base::TimeTicks::Now();
      blink::WebNavigationParams::RedirectInfo redirect{};
      redirect.new_url = blink::WebURL(blink::KURL(next));
      if (!url::Origin::Create(url).IsSameOriginWith(next)) origin_tainted = true;
      if (method == "POST" && (resource.status_code == 301 || resource.status_code == 302 || resource.status_code == 303)) {
        parameters->http_method = blink::WebString::FromAscii("GET");
        parameters->http_body.Reset();
        parameters->http_content_type = blink::WebString();
        body.clear();
      }
      redirect.new_http_method = parameters->http_method;
      redirect.redirect_response = response;
      parameters->redirects.push_back(std::move(redirect));
      url = std::move(next);
      ++redirects; Schedule(); return;
    }
    if (resource.status_code == 204 || resource.status_code == 205 ||
        resource.status_code == 304)
      { Finish(ERROR(3)); return; }
    policy->policies.cross_origin_embedder_policy =
        network::ParseCrossOriginEmbedderPolicy(*response_headers).value;
    for (size_t index = 0; index < resource.header_count; ++index) {
      const auto& header = resource.headers[index];
      std::string name(header.name);
      std::transform(name.begin(), name.end(), name.begin(),
          [](unsigned char character) {
            return character >= 'A' && character <= 'Z' ? character + ('a' - 'A') : character;
          });
      if (name != "content-security-policy" &&
          name != "content-security-policy-report-only")
        continue;
      auto policies = network::ParseContentSecurityPolicies(header.value,
          name == "content-security-policy" ? network::mojom::ContentSecurityPolicyType::kEnforce
              : network::mojom::ContentSecurityPolicyType::kReport,
          network::mojom::ContentSecurityPolicySource::kHTTP, url);
      for (const auto& parsed : policies) {
        if (name == "content-security-policy")
          policy->policies.sandbox_flags |= parsed->sandbox;
        policy->policies.content_security_policies.push_back(
            content::ToWebContentSecurityPolicy(parsed));
      }
    }
    parameters->policy_container = std::move(policy);
    parameters->url = blink::WebURL(blink::KURL(url));
    parameters->response = std::move(response);
    parameters->http_status_code = resource.status_code;
    parameters->body_loader = std::make_unique<NetworkBodyLoader>(resource);
    parameters->is_static_data = false;
    auto alive = weak.GetWeakPtr();
    frame.CommitNavigation(std::move(parameters), nullptr);
    if (alive) Finish(0);
    return;
  }

  }
  SystemTable* table;
  blink::WebNavigationControl& frame;
  GURL url;
  std::unique_ptr<blink::WebPolicyContainer> policy;
  bool cookies_allowed;
  std::unique_ptr<blink::WebNavigationParams> parameters;
  base::OnceCallback<void(Status)> completion;
  ResourceJob* job = nullptr;
  unsigned redirects = 0;
  std::vector<char> body;
  bool invalid_body = false, origin_tainted = false;
  base::WeakPtrFactory<AsyncNavigation> weak{this};
};
}
std::unique_ptr<NavigationRequest> LoadNavigation(SystemTable* table,
    blink::WebNavigationControl& frame, const char* address,
    std::unique_ptr<blink::WebPolicyContainer> policy, blink::WebFrameLoadType type,
    blink::WebHistoryItem item, base::OnceCallback<void(Status)> completion,
    std::unique_ptr<blink::WebNavigationParams> parameters, bool allow_cookies) {
  return std::make_unique<AsyncNavigation>(table, frame, address, std::move(policy),
      type, item, std::move(completion), std::move(parameters), allow_cookies);
}

void PaintFrame(blink::WebView &view, blink::WebFrameWidget &widget,
                SkCanvas &canvas, const gfx::Rect &viewport) {
  widget.UpdateAllLifecyclePhases(blink::DocumentUpdateReason::kBeginMainFrame);
  cc::SkiaPaintCanvas paint_canvas(&canvas);
  paint_canvas.save();
  view.PaintContent(&paint_canvas, viewport);
  paint_canvas.restore();
}
} // namespace uefi_browser

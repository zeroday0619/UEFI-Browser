// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
#include "blink/resource_loader.h"
#if defined(UEFI_RUNTIME)
#include "third_party/blink/renderer/core/loader/loader_factory_for_frame.h"
#endif

#include <optional>
#include <algorithm>
#include <cstdio>
#include <string>
#include <utility>
#include <vector>
#include "base/check.h"
#include "base/functional/bind.h"
#include "base/memory/weak_ptr.h"
#include "base/strings/string_util.h"
#include "base/task/single_thread_task_runner.h"
#include "net/base/net_errors.h"
#include "net/http/http_util.h"
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
  ResourceLoader(SystemTable* table, const url::Origin& document_origin,
                 scoped_refptr<base::SingleThreadTaskRunner> task_runner)
      : table_(table), origin_(document_origin), task_runner_(std::move(task_runner)) {
    CHECK(table_);
    CHECK(task_runner_);
  }
  ~ResourceLoader() override { resource_async_release(job_); resource_release(&resource_); }

  void LoadSynchronously(
      std::unique_ptr<network::ResourceRequest> request,
      scoped_refptr<const blink::SecurityOrigin>, bool, bool, base::TimeDelta,
      blink::URLLoaderClient*, blink::WebURLResponse&,
      std::optional<blink::WebURLError>& error,
      scoped_refptr<blink::SharedBuffer>& data, int64_t& encoded_data_length,
      uint64_t& encoded_body_length, scoped_refptr<blink::BlobDataHandle>& blob,
      std::unique_ptr<blink::ResourceLoadInfoNotifierWrapper>) override {
    data = nullptr;
    blob = nullptr;
    encoded_data_length = 0;
    encoded_body_length = 0;
    error.emplace(net::ERR_NOT_IMPLEMENTED, blink::WebURL(blink::KURL(request->url)));
  }

  void LoadAsynchronously(
      std::unique_ptr<network::ResourceRequest> request,
      scoped_refptr<const blink::SecurityOrigin>, bool,
      std::unique_ptr<blink::ResourceLoadInfoNotifierWrapper>,
      blink::CodeCacheHost*, blink::URLLoaderClient* client) override {
    CHECK(!request_);
    CHECK(client);
    request_ = std::move(request);
    client_ = client;
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
    if (!client_ || frozen_ || pending_)
      return;
    pending_ = true;
    task_runner_->PostDelayedTask(FROM_HERE, base::BindOnce(&ResourceLoader::Run,
        weak_factory_.GetWeakPtr()), base::Milliseconds(2));
  }

  int ValidateRequest() const {
    if (!request_->url.is_valid() || !request_->url.SchemeIsHTTPOrHTTPS() ||
        request_->url.has_username() || request_->url.has_password())
      return net::ERR_INVALID_URL;
    // Cross-origin fetching requires CORS and opaque-response policy support.
    // Reject it before sending a request until that service is implemented.
    if (origin_.opaque() || !origin_.IsSameOriginWith(request_->url) ||
        !request_->request_initiator ||
        !origin_.IsSameOriginWith(*request_->request_initiator))
      return net::ERR_BLOCKED_BY_CLIENT;
    if (request_->method != "GET" || request_->request_body || request_->keepalive ||
        !request_->cors_exempt_headers.IsEmpty())
      return net::ERR_NOT_IMPLEMENTED;
    for (const auto& header : request_->headers.GetHeaderVector()) {
      if (base::EqualsCaseInsensitiveASCII(header.key, "cookie") ||
          base::EqualsCaseInsensitiveASCII(header.key, "authorization") ||
          base::EqualsCaseInsensitiveASCII(header.key, "proxy-authorization") ||
          base::EqualsCaseInsensitiveASCII(header.key, "range"))
        return net::ERR_NOT_IMPLEMENTED;
    }
    return net::OK;
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
      int validation = ValidateRequest();
      if (validation != net::OK) {
        Fail(validation);
        return;
      }
      std::vector<Header> headers;
      for (const auto& header : request_->headers.GetHeaderVector()) {
        if (base::EqualsCaseInsensitiveASCII(header.key, "user-agent")) continue;
        headers.push_back({const_cast<char*>(header.key.c_str()),
                           const_cast<char*>(header.value.c_str())});
      }
      headers.push_back({const_cast<char*>("User-Agent"), const_cast<char*>(
          firmware_browser_user_agent_for_url(origin_.GetURL().spec().c_str()))});
      GURL::Replacements replacements;
      replacements.ClearRef();
      auto url = request_->url.ReplaceComponents(replacements);
      if (!job_) job_ = resource_async_start(table_, url.spec().c_str(), headers.data(), headers.size());
      if (!job_) { Fail(net::ERR_INSUFFICIENT_RESOURCES); return; }
      Status status;
      if (!resource_async_result(job_, &status, &resource_)) { Schedule(); return; }
      resource_async_release(job_);
      job_ = nullptr;
      if (status) {
        Fail(net::ERR_FAILED);
        return;
      }
      if (resource_is_redirect(resource_.status_code)) {
        Fail(net::ERR_UNSAFE_REDIRECT);
        return;
      }
      downloaded_ = true;
      const char* type = resource_header(&resource_, "content-type");
      std::printf("BLINK: resource %u type=%s bytes=%zu\n",
                  static_cast<unsigned>(resource_.status_code), type ? type : "",
                  resource_.length);
    }
    if (!response_sent_) {
      blink::WebURLResponse response{blink::WebURL(blink::KURL(request_->url))};
      response.SetHttpStatusCode(resource_.status_code);
      response.SetExpectedContentLength(resource_.length);
      std::string mime_type, charset;
      bool had_charset = false;
      if (const char* type = resource_header(&resource_, "content-type"))
        net::HttpUtil::ParseContentType(type, &mime_type, &charset, &had_charset,
                                       nullptr);
      response.SetMimeType(blink::WebString::FromUtf8(mime_type));
      response.SetTextEncodingName(blink::WebString::FromUtf8(charset));
      for (size_t index = 0; index < resource_.header_count; ++index) {
        const Header& header = resource_.headers[index];
        response.AddHttpHeaderField(blink::WebString::FromLatin1(header.name),
                                    blink::WebString::FromLatin1(header.value));
      }
      blink::SegmentedBuffer body;
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
        resource_.encoded_body_length, resource_.length);
  }

  SystemTable* table_;
  const url::Origin origin_;
  scoped_refptr<base::SingleThreadTaskRunner> task_runner_;
  std::unique_ptr<network::ResourceRequest> request_;
  blink::URLLoaderClient* client_ = nullptr;
  ResourceResponse resource_{};
  ResourceJob* job_ = nullptr;
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
std::unique_ptr<blink::URLLoader> CreateFrameResourceLoader(
    const blink::SecurityOrigin& origin,
    scoped_refptr<base::SingleThreadTaskRunner> runner) {
  CHECK(resource_system_table);
  return CreateResourceLoader(resource_system_table, origin.ToUrlOrigin(),
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
    SystemTable* table, const url::Origin& document_origin,
    scoped_refptr<base::SingleThreadTaskRunner> task_runner) {
  return std::make_unique<ResourceLoader>(table, document_origin,
                                        std::move(task_runner));
}
}

#ifndef UEFI_BLINK_RESPONSE_HEADERS_H_
#define UEFI_BLINK_RESPONSE_HEADERS_H_

#include <string>
#include "net/http/http_response_headers.h"
#include "net/http/http_util.h"
#include "net/http/http_version.h"
#include "network/resource.h"

namespace uefi_browser {
inline scoped_refptr<net::HttpResponseHeaders> ResponseHeaders(
    const ResourceResponse& response) {
  const std::string status = std::to_string(response.status_code);
  net::HttpResponseHeaders::Builder builder(net::HttpVersion(1, 1), status);
  for (size_t index = 0; index < response.header_count; ++index) {
    if (!net::HttpUtil::IsValidHeaderName(response.headers[index].name) ||
        !net::HttpUtil::IsValidHeaderValue(response.headers[index].value))
      return nullptr;
    builder.AddHeader(response.headers[index].name, response.headers[index].value);
  }
  return builder.Build();
}
}  // namespace uefi_browser
#endif

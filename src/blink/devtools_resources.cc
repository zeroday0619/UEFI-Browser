#include "blink/devtools_resources.h"
#include <cstdlib>
#include <cstring>
#include <string>
#include "third_party/zlib/zlib.h"
#include "url/gurl.h"

extern "C" const unsigned char* firmware_devtools_resource(
    const char*, size_t*, size_t*);

namespace uefi_browser {
bool IsDevToolsHost(const GURL& url) {
  return url.host() == "devtools.uefi.invalid";
}
bool IsDevToolsOrigin(const GURL& url) {
  return url.is_valid() && IsDevToolsHost(url) && url.SchemeIs("https") &&
      url.EffectiveIntPort() == 443 && !url.has_username() && !url.has_password();
}
namespace {
const char* MimeType(std::string_view path) {
  if (path.ends_with(".js") || path.ends_with(".mjs")) return "text/javascript; charset=utf-8";
  if (path.ends_with(".html")) return "text/html; charset=utf-8";
  if (path.ends_with(".css")) return "text/css; charset=utf-8";
  if (path.ends_with(".json")) return "application/json; charset=utf-8";
  if (path.ends_with(".svg")) return "image/svg+xml";
  if (path.ends_with(".png")) return "image/png";
  if (path.ends_with(".avif")) return "image/avif";
  if (path.ends_with(".webp")) return "image/webp";
  if (path.ends_with(".woff2")) return "font/woff2";
  if (path.ends_with(".wasm")) return "application/wasm";
  if (path.ends_with(".md") || path.ends_with(".txt")) return "text/plain; charset=utf-8";
  return "application/octet-stream";
}
}
Status LoadDevToolsResource(const GURL& url, ResourceResponse* response) {
  if (!response || !IsDevToolsOrigin(url)) return ERROR(2);
  *response = {};
  size_t compressed = 0, length = 0;
  const std::string path(url.path());
  const auto* data = firmware_devtools_resource(path.c_str(), &compressed, &length);
  if (length > RESOURCE_BODY_LIMIT || compressed > RESOURCE_BODY_LIMIT) return ERROR(4);
  // Missing internal paths return a local response and must never trigger DNS.
  static constexpr char missing[] = "DevTools resource not found.";
  if (!data) length = sizeof(missing) - 1;
  response->body = static_cast<unsigned char*>(std::malloc(length ? length : 1));
  if (!response->body) return ERROR(9);
  if (data) {
    uLongf decoded = length;
    if (uncompress(response->body, &decoded, data, compressed) != Z_OK || decoded != length) {
      resource_release(response); return ERROR(7);
    }
  } else std::memcpy(response->body, missing, length);
  response->length = length;
  response->encoded_body_length = data ? compressed : length;
  response->status_code = data ? 200 : 404;
  const char* names[] = {"Content-Type", "X-Content-Type-Options",
      "Content-Security-Policy", "Cache-Control"};
  const char* values[] = {data ? MimeType(url.path()) : "text/plain; charset=utf-8",
      "nosniff",
      "default-src 'self' data: blob:; script-src 'self' 'wasm-unsafe-eval'; "
      "style-src 'self' 'unsafe-inline'; object-src 'none'; frame-src 'none'; "
      "connect-src 'self' data: blob:; base-uri 'self'; frame-ancestors 'none'",
      "no-store"};
  response->headers = static_cast<Header*>(std::calloc(4, sizeof(Header)));
  if (!response->headers) { resource_release(response); return ERROR(9); }
  response->header_count = 4;
  for (size_t index = 0; index < 4; ++index) {
    response->headers[index] = {strdup(names[index]), strdup(values[index])};
    if (!response->headers[index].name || !response->headers[index].value) {
      resource_release(response); return ERROR(9);
    }
  }
  return 0;
}
}

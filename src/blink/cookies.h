#ifndef UEFI_BROWSER_COOKIES_H
#define UEFI_BROWSER_COOKIES_H
#include <string>
#include <string_view>
#include "url/gurl.h"
struct ResourceResponse;
namespace uefi_browser {
// The renderer owns this bounded session jar on its single task sequence.
std::string CookieHeader(const GURL&, bool script, bool navigation = false);
bool StoreCookie(const GURL&, std::string_view, bool script);
void StoreResponseCookies(const GURL&, const ResourceResponse&);
}
#endif

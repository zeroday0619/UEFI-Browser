#ifndef UEFI_DEVTOOLS_RESOURCES_H
#define UEFI_DEVTOOLS_RESOURCES_H
#include "network/resource.h"
class GURL;
namespace uefi_browser {
inline constexpr char kDevToolsFrontendUrl[] =
    "https://devtools.uefi.invalid/front_end/devtools_app.html";
// The reserved host never falls through to firmware networking.
bool IsDevToolsHost(const GURL& url);
bool IsDevToolsOrigin(const GURL& url);
Status LoadDevToolsResource(const GURL& url, ResourceResponse* response);
}
#endif

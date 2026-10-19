#ifndef UEFI_BROWSER_IDENTITY_H
#define UEFI_BROWSER_IDENTITY_H
#include "blink/browser_build_info.h"
#include "third_party/blink/public/common/user_agent/user_agent_metadata.h"
#include "url/gurl.h"
namespace uefi_browser {
inline bool GoogleCompatibility(const GURL& url) {
  if (!url.SchemeIsHTTPOrHTTPS() || url.has_username() || url.has_password()) return false;
  for (const char* domain : {"google.com", "google.co.kr", "youtube.com", "gmail.com",
                            "googleapis.com", "googleusercontent.com", "gstatic.com"})
    if (url.DomainIs(domain)) return true;
  return false;
}
inline const char* BrowserUserAgent(bool chrome) {
  if (chrome)
    return "Mozilla/5.0 (X11; Linux x86_64) AppleWebKit/537.36 (KHTML, like Gecko) "
           "Chrome/" UEFI_CHROMIUM_VERSION " Safari/537.36";
  return "Mozilla/5.0 (UEFI; x86_64) AppleWebKit/537.36 (KHTML, like Gecko) "
         "Chrome/" UEFI_CHROMIUM_VERSION " Safari/537.36 UEFIBrowser/0.3";
}
inline blink::UserAgentMetadata BrowserUserAgentMetadata(bool chrome = false) {
  blink::UserAgentMetadata metadata;
  const std::string version = UEFI_CHROMIUM_VERSION;
  const std::string major = version.substr(0, version.find('.'));
  metadata.brand_version_list = {{"Chromium", major},
      {chrome ? "Google Chrome" : "UEFIBrowser", chrome ? major : "0"}};
  metadata.brand_full_version_list = {{"Chromium", version},
      {chrome ? "Google Chrome" : "UEFIBrowser", chrome ? version : "0.3"}};
  metadata.full_version = version;
  metadata.platform = chrome ? "Linux" : "UEFI";
  metadata.architecture = "x86";
  metadata.bitness = "64";
  metadata.mobile = false;
  metadata.form_factors = {blink::kDesktopFormFactor};
  return metadata;
}
}
#endif

#include "blink/cookies.h"
#include "network/resource.h"
#include <algorithm>
#include <vector>
#include "base/strings/string_util.h"
#include "base/time/time.h"
#include "net/cookies/canonical_cookie.h"
#include "net/cookies/cookie_access_params.h"
#include "net/cookies/cookie_access_result.h"
#include "net/cookies/cookie_options.h"
#include "net/cookies/parsed_cookie.h"
namespace uefi_browser {
namespace {
std::vector<net::CanonicalCookie> cookies;
net::CookieOptions Options(bool script, bool navigation = false) {
  net::CookieOptions options;
  if (!script) options.set_include_httponly();
  using Context = net::CookieOptions::SameSiteCookieContext;
  options.set_same_site_cookie_context(Context(navigation ? Context::ContextType::SAME_SITE_LAX : Context::ContextType::SAME_SITE_STRICT));
  return options;
}
net::CookieAccessParams Access() {
  return {net::CookieAccessSemantics::NONLEGACY,net::CookieScopeSemantics::NONLEGACY,false};
}
void Expire() {
  auto now=base::Time::Now();
  std::erase_if(cookies,[&](const auto& cookie){return cookie.IsExpired(now);});
}
}
std::string CookieHeader(const GURL& url, bool script, bool navigation) {
  if (!url.SchemeIsHTTPOrHTTPS()) return {};
  Expire();
  net::CookieList selected;
  for (const auto& cookie:cookies)
    if (cookie.IncludeForRequestURL(url,Options(script,navigation),Access()).status.IsInclude()) selected.push_back(cookie);
  std::stable_sort(selected.begin(),selected.end(),[](const auto& left,const auto& right) {
    return left.Path().size()!=right.Path().size() ? left.Path().size()>right.Path().size() : left.CreationDate()<right.CreationDate();
  });
  std::string line;
  for (const auto& cookie:selected) {
    std::string entry=net::CanonicalCookie::BuildCookieLine(net::CookieList{cookie});
    if (line.size()+entry.size()+(line.empty() ? 0 : 2)>8191) break;
    if (!line.empty()) line+="; ";
    line+=entry;
  }
  return line;
}
bool StoreCookie(const GURL& url, std::string_view line, bool script) {
  if (!url.SchemeIsHTTPOrHTTPS() || line.size()>4096) return false;
  // Partitioned cookies require a top-level partition key and remain disabled until that store exists.
  net::ParsedCookie parsed(line);
  if (!parsed.IsValid() || parsed.IsPartitioned()) return false;
  auto cookie=net::CanonicalCookie::Create(url,line,base::Time::Now(),std::nullopt,std::nullopt,
      script ? net::CookieSourceType::kScript : net::CookieSourceType::kHTTP,nullptr);
  if (!cookie || !cookie->IsSetPermittedInContext(url,Options(script),Access(),{"http","https"}).status.IsInclude()) return false;
  Expire();
  for (const auto& existing:cookies) {
    if (script && existing.IsHttpOnly() && cookie->IsEquivalent(existing)) return false;
    if (!url.SchemeIs("https") && existing.SecureAttribute() && cookie->IsEquivalentForSecureCookieMatching(existing)) return false;
  }
  auto existing=std::find_if(cookies.begin(),cookies.end(),[&](const auto& item){return cookie->IsEquivalent(item);});
  if (existing!=cookies.end()) { cookie->SetCreationDate(existing->CreationDate()); cookies.erase(existing); }
  if (cookie->IsExpired(base::Time::Now())) return true;
  // A finite jar prevents untrusted sites from consuming unbounded firmware memory.
  if (cookies.size()>=512) cookies.erase(cookies.begin());
  cookies.push_back(*cookie);
  return true;
}
void StoreResponseCookies(const GURL& url, const ResourceResponse& response) {
  for (size_t index=0;index<response.header_count;++index)
    if (base::EqualsCaseInsensitiveASCII(response.headers[index].name,"set-cookie"))
      StoreCookie(url,response.headers[index].value,false);
}
}

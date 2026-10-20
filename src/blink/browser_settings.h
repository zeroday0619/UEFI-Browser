// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
#ifndef UEFI_BROWSER_SETTINGS_H
#define UEFI_BROWSER_SETTINGS_H
#include <cstdio>
#include <cstring>
#include <string>
#include "network/resource.h"
#include "renderer_runtime.h"
#include "blink/browser_build_info.h"
#include "url/gurl.h"
#include "v8/include/v8-version-string.h"
namespace uefi_browser {
struct BrowserPreferences {
  uint32_t version = 1;
  uint32_t dark = 0;
  uint32_t font_size = 16;
  uint32_t javascript = 1;
  uint32_t images = 1;
  uint32_t startup_settings = 0;
};
inline BrowserPreferences preferences;
inline bool preferences_saved = true;
inline Guid preferences_guid = {0x6156d542, 0x0a46, 0x4ad7, {0x9a,0x16,0xb2,0xc0,0x39,0x68,0xa1,0x4e}};
inline Character preferences_name[] = {'U','e','f','i','B','r','o','w','s','e','r','S','e','t','t','i','n','g','s',0};
inline void LoadPreferences(SystemTable* table) {
  struct Runtime { TableHeader header; void* function[14]; };
  auto* runtime = static_cast<Runtime*>(table->runtime);
  BrowserPreferences stored;
  size_t size = sizeof(stored);
  uint32_t attributes = 0;
  auto get = reinterpret_cast<Status(UEFI_API *)(Character*, Guid*, uint32_t*, size_t*, void*)>(runtime->function[6]);
  Status status = get(preferences_name, &preferences_guid, &attributes, &size, &stored);
  if (!status && size == sizeof(stored) && stored.version == 1 && stored.dark <= 1 &&
      (stored.font_size == 14 || stored.font_size == 16 || stored.font_size == 20) &&
      stored.javascript <= 1 && stored.images <= 1 && stored.startup_settings <= 1)
    preferences = stored;
  std::printf("SETTINGS: loaded theme=%u font=%u javascript=%u images=%u startup=%u\n",
      preferences.dark, preferences.font_size, preferences.javascript, preferences.images, preferences.startup_settings);
}
inline void SavePreferences(SystemTable* table) {
  struct Runtime { TableHeader header; void* function[14]; };
  auto* runtime = static_cast<Runtime*>(table->runtime);
  auto set = reinterpret_cast<Status(UEFI_API *)(Character*, Guid*, uint32_t, size_t, void*)>(runtime->function[8]);
  Status status = set(preferences_name, &preferences_guid, 7, sizeof(preferences), &preferences);
  preferences_saved = !status;
  std::printf("SETTINGS: save status=%llx theme=%u font=%u javascript=%u images=%u startup=%u\n",
      static_cast<unsigned long long>(status), preferences.dark, preferences.font_size,
      preferences.javascript, preferences.images, preferences.startup_settings);
}
inline bool IsBrowserPage(const GURL& url) {
  return url.SchemeIs("about") && (url.path() == "settings" || url.path() == "version");
}
inline bool ChangePreference(std::string_view query, SystemTable* table) {
  if (query == "theme=light") preferences.dark = 0;
  else if (query == "theme=dark") preferences.dark = 1;
  else if (query == "font=14") preferences.font_size = 14;
  else if (query == "font=16") preferences.font_size = 16;
  else if (query == "font=20") preferences.font_size = 20;
  else if (query == "javascript=on") preferences.javascript = 1;
  else if (query == "javascript=off") preferences.javascript = 0;
  else if (query == "images=on") preferences.images = 1;
  else if (query == "images=off") preferences.images = 0;
  else if (query == "startup=blank") preferences.startup_settings = 0;
  else if (query == "startup=settings") preferences.startup_settings = 1;
  else if (query == "reset=confirm") preferences = BrowserPreferences{};
  else return false;
  SavePreferences(table);
  return true;
}
inline std::string BrowserPage(const GURL& url) {
  const bool about = url.path() == "version";
  std::string html = R"HTML(<!doctype html><html lang="en"><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1"><meta http-equiv="Content-Security-Policy" content="default-src 'none'; style-src 'unsafe-inline'; script-src 'unsafe-inline'; base-uri 'none'; form-action 'none'"><title>UEFI BROWSER settings</title><style>
:root{font-family:'Noto Sans',sans-serif;font-size:14px;color:#242424;background:#f5f5f5;--surface:#fff;--muted:#616161;--border:#e0e0e0;--accent:#0f6cbd;--selected:#ebf3fc;--hover:#f0f0f0}
body.dark{color:#f5f5f5;background:#202020;--surface:#292929;--muted:#bdbdbd;--border:#424242;--accent:#75b6e7;--selected:#123554;--hover:#383838}*{box-sizing:border-box}body{margin:0;min-height:100vh}aside{position:fixed;inset:0 auto 0 0;width:232px;padding:32px 16px;border-right:1px solid var(--border)}.brand{font-size:16px;font-weight:600;margin:0 12px 32px;letter-spacing:.5px}.brand span{display:inline-block;background:#0f6cbd;color:white;border-radius:8px;padding:5px 9px;margin-right:8px}nav a{display:block;color:inherit;text-decoration:none;border-radius:6px;padding:12px 16px;margin:4px 0}nav a:hover,button:hover,.choice:hover{background:var(--hover)}nav a.active{background:var(--selected);color:var(--accent);border-left:3px solid var(--accent);padding-left:13px}.caption{position:absolute;bottom:28px;left:28px;color:var(--muted);font-size:12px}main{margin-left:232px;padding:32px 48px 60px;max-width:1120px}h1{font-size:28px;line-height:36px;font-weight:600;margin:24px 0 8px}h2{font-size:20px;font-weight:600;margin:28px 0 12px}p{line-height:22px;color:var(--muted);margin:0 0 20px}.search{width:100%;height:40px;border:1px solid var(--border);border-bottom-color:#616161;border-radius:6px;padding:0 14px;background:var(--surface);color:inherit;font:inherit}.card{background:var(--surface);border:1px solid var(--border);border-radius:8px;overflow:hidden;margin-bottom:16px;box-shadow:0 1px 2px #00000008}.row{padding:20px 24px;display:flex;align-items:center;justify-content:space-between;gap:24px;border-bottom:1px solid var(--border)}.row:last-child{border:0}.label{font-weight:600;line-height:22px}.detail{font-weight:400;font-size:12px;line-height:18px;color:var(--muted);margin-top:4px}.choices{display:flex;gap:6px;flex-shrink:0}.choice,button{display:inline-block;border:1px solid var(--border);border-radius:5px;padding:7px 12px;text-decoration:none;font:inherit;background:var(--surface);color:inherit;cursor:pointer;white-space:nowrap}.choice.chosen{border-color:var(--accent);background:var(--selected);color:var(--accent)}:focus-visible{outline:2px solid var(--accent);outline-offset:3px}.notice{font-size:12px;color:var(--muted);line-height:20px}.logo{display:inline-grid;place-items:center;width:64px;height:64px;border-radius:16px;background:#0f6cbd;color:white;font-size:36px;margin-top:24px}.version{font-family:monospace;font-size:13px;overflow-wrap:anywhere}.hidden{display:none}a{color:var(--accent)}@media(max-width:900px){main{padding:24px}aside{width:200px}main{margin-left:200px}.row{padding:16px;flex-wrap:wrap;gap:12px}}@media(max-width:600px){aside{position:static;width:auto;border-right:0;padding:16px}.brand{margin-bottom:12px}nav{display:flex;flex-wrap:wrap}.caption{display:none}main{margin:0;padding:16px}}
</style><body)HTML";
  if (preferences.dark) html += " class=\"dark\"";
  html += "><aside><div class=\"brand\"><span>U</span>UEFI BROWSER</div><nav aria-label=\"Settings categories\">";
  html += std::string("<a href=\"about:settings\" class=\"") + (!about ? "active" : "") + "\">Settings</a><a href=\"about:settings#appearance\">Appearance</a><a href=\"about:settings#content\">Site settings</a><a href=\"about:settings#startup\">On startup</a><a href=\"about:version\" class=\"" + (about ? "active" : "") + "\">About UEFI BROWSER</a></nav><div class=\"caption\">UEFI x86-64<br>UEFI firmware browser</div></aside><main>";
  if (about) {
    html += "<div class=\"logo\" aria-hidden=\"true\">U</div><h1>About UEFI BROWSER</h1><p>A browser built for UEFI firmware.</p><div class=\"card\"><div class=\"row\"><div><div class=\"label\">UEFI BROWSER</div><div class=\"detail\">Experimental firmware port · Fluent-inspired interface</div></div></div><div class=\"row\"><span>Blink / Chromium source</span><span class=\"version\">" UEFI_CHROMIUM_VERSION "</span></div><div class=\"row\"><span>V8</span><span class=\"version\">";
    html += V8_VERSION_STRING;
    html += "</span></div><div class=\"row\"><span>Rendering</span><span>Skia · Software rasterization</span></div><div class=\"row\"><span>Platform</span><span>UEFI x86-64 · musl 1.2.5</span></div></div><div class=\"notice version\">Chromium revision: " UEFI_CHROMIUM_REVISION "</div><h2>Updates and open source</h2><p>This build has no automatic updater. Install a new EFI build to update the browser.</p><p>Blink, V8 and Skia are Chromium open-source components. This project is not Google Chrome. License texts and third-party notices are included with the distribution.</p><div class=\"notice\">With multiple CPUs, Blink and V8 run in a separate AP address space with private runtime and TLS. Firmware networking and browser controls run on the BSP.";
    html += renderer_runtime_sandboxed() ?
        " The renderer uses CPL3, supervisor-owned page tables and a fixed syscall broker. Tabs share one renderer. JIT pages may be RWX. Cached scrolling uses validated compressed pixel tiles and bounded scroll metadata. This experimental isolation has not received an independent security audit." :
        " The renderer still has CPL0 privileges and has no security sandbox.";
    html += " Chrome profiles, sync, extensions and password management are not available.</div>";
  } else {
    html += "<input class=\"search\" type=\"search\" placeholder=\"Search settings\" aria-label=\"Search settings\"><h1>Settings</h1><p>Make this browser work the way you prefer.</p>";
    auto choice = [](const char* query, const char* label, bool selected) {
      return std::string("<a role=\"button\" aria-pressed=\"") + (selected ? "true" : "false") + "\" class=\"choice" + (selected ? " chosen" : "") + "\" href=\"about:settings?" + query + "\">" + label + "</a>";
    };
    auto row = [&](const char* label, const char* description, std::string controls) {
      html += std::string("<div class=\"row\"><div><div class=\"label\">") + label + "</div><div class=\"detail\">" + description + "</div></div><div class=\"choices\">" + controls + "</div></div>";
    };
    html += "<section id=\"appearance\"><h2>Appearance</h2><div class=\"card\">";
    row("Browser theme", "Applies to the toolbar and settings pages.", choice("theme=light","Light",!preferences.dark)+choice("theme=dark","Dark",preferences.dark));
    row("Default font size", "Used by sites without an explicit font size.", choice("font=14","Small",preferences.font_size==14)+choice("font=16","Medium",preferences.font_size==16)+choice("font=20","Large",preferences.font_size==20));
    html += "</div></section><section id=\"content\"><h2>Site settings</h2><div class=\"card\">";
    row("JavaScript", "Applies when a website is next opened or reloaded.", choice("javascript=on","Allow",preferences.javascript)+choice("javascript=off","Block",!preferences.javascript));
    row("Images", "Applies when a website is next opened or reloaded.", choice("images=on","Show",preferences.images)+choice("images=off","Hide",!preferences.images));
    html += "</div></section><section id=\"startup\"><h2>On startup</h2><div class=\"card\">";
    row("Open on startup", "Choose the first page for this browser.", choice("startup=blank","Blank page",!preferences.startup_settings)+choice("startup=settings","Settings",preferences.startup_settings));
    html += "</div></section><section><h2>Reset settings</h2><div class=\"card\"><div class=\"row\"><div><div class=\"label\">Restore defaults</div><div class=\"detail\">Resets theme, font size, site settings and startup page.</div></div><details><summary class=\"choice\">Reset settings</summary><p>Restore all five settings?</p><a class=\"choice\" href=\"about:settings?reset=confirm\">Confirm reset</a></details></div></div></section>";
    html += preferences_saved ? "<p class=\"notice\" role=\"status\">Settings are stored in this firmware profile. Browsing history is session-only.</p>" : "<p role=\"alert\">Firmware could not save these settings. Changes apply to this session only.</p>";
    html += R"HTML(<p id="empty" hidden>No matching settings.</p><script>document.addEventListener('keydown',function(event){if(event.key===' ' && event.target.matches('a[role=button]')){event.preventDefault();event.target.click();}});document.querySelector('.search').addEventListener('input',function(){let count=0;for(const section of document.querySelectorAll('section')){section.hidden=!section.textContent.toLowerCase().includes(this.value.toLowerCase());if(!section.hidden)count++;}document.querySelector('#empty').hidden=count!==0;});</script>)HTML";
  }
  return html + "</main></body></html>";
}
} // namespace uefi_browser
#endif

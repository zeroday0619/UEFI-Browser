#include "blink/gpu_compositor.h"
#include "third_party/blink/public/platform/web_graphics_context_3d_provider.h"
#include "blink/blob_service.h"
#include "blink/devtools_resources.h"
#include "components/webcrypto/webcrypto_impl.h"
#include "third_party/blink/public/common/thread_safe_browser_interface_broker_proxy.h"
// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
#include <cstdio>
#include "base/memory/memory_pressure_listener_registry.h"
#include <limits>
#include "process/process.h"
#include <cmath>
#include <cstring>
#include "blink/browser_settings.h"
#include "blink/text_composer.h"
#include "blink/ime_popup.h"
#include "blink/browser_identity.h"
#include <algorithm>
#include <vector>
#include "base/strings/utf_string_conversions.h"
#include "skia/ext/font_utils.h"
#include "third_party/skia/include/core/SkFont.h"
#include "third_party/skia/include/core/SkFontMgr.h"
#include "third_party/skia/include/core/SkPaint.h"
#include "url/gurl.h"
#include "base/at_exit.h"
#include "base/check.h"
#include "base/check_op.h"
#include "base/command_line.h"
#include "base/feature_list.h"
#include "base/functional/bind.h"
#include "base/i18n/icu_util.h"
#include "base/run_loop.h"
#include "base/memory/ref_counted_memory.h"
#include "base/memory/discardable_memory_allocator.h"
#include "third_party/brotli/include/brotli/decode.h"
#include "third_party/zlib/google/compression_utils.h"
#include "base/task/thread_pool/thread_pool_instance.h"
#include "blink/browser_frame.h"
#include "blink/remote_frame.h"
#include "renderer_runtime.h"
#include "firmware_audio.h"
#include "firmware_media.h"
#include "blink/workers.h"
#include "blink/gpu_context.h"
#include "blink/webgl_context.h"
#include "blink/scheduler.h"
#include "mojo/core/embedder/embedder.h"
#include "mojo/public/cpp/bindings/binder_map.h"
#include "mojo/public/cpp/bindings/receiver_set.h"
#include "third_party/blink/public/mojom/gpu/gpu.mojom.h"
#include "third_party/blink/public/platform/platform.h"
#include "third_party/blink/public/platform/web_network_state_notifier.h"
#include "third_party/blink/public/web/blink.h"
#include "third_party/skia/include/core/SkCanvas.h"
#include "third_party/skia/include/core/SkColor.h"
#include "third_party/skia/include/core/SkImageInfo.h"
#include "third_party/skia/include/core/SkSurface.h"

extern "C" const char* firmware_browser_user_agent(void) {
  return uefi_browser::BrowserUserAgent(false);
}
extern "C" const char* firmware_browser_user_agent_for_url(const char* url) {
  return uefi_browser::BrowserUserAgent(uefi_browser::GoogleCompatibility(GURL(url)));
}
extern "C" int firmware_browser_user_agent_valid(const char* value) {
  return !std::strcmp(value, uefi_browser::BrowserUserAgent(false)) ||
         !std::strcmp(value, uefi_browser::BrowserUserAgent(true));
}
extern "C" const char* firmware_browser_client_hint_for_url(const char* name, const char* url) {
  bool chrome = uefi_browser::GoogleCompatibility(GURL(url));
  static const std::string native_brands = uefi_browser::BrowserUserAgentMetadata().SerializeBrandMajorVersionList();
  static const std::string chrome_brands = uefi_browser::BrowserUserAgentMetadata(true).SerializeBrandMajorVersionList();
  if (!std::strcmp(name, "Sec-CH-UA")) return chrome ? chrome_brands.c_str() : native_brands.c_str();
  if (!std::strcmp(name, "Sec-CH-UA-Mobile")) return "?0";
  if (!std::strcmp(name, "Sec-CH-UA-Platform")) return chrome ? "\"Linux\"" : "\"UEFI\"";
  return nullptr;
}
extern "C" const char* firmware_browser_client_hint(const char* name) {
  return firmware_browser_client_hint_for_url(name, "about:blank");
}

namespace uefi_browser {
void InstallFonts();
std::unique_ptr<base::DiscardableMemoryAllocator> CreateDiscardableAllocator();
}
extern "C" size_t runtime_memory_used(void);
extern "C" size_t runtime_memory_limit(void);
extern "C" size_t runtime_memory_committed(void);
extern "C" const char firmware_browser_url[];
extern "C" const char firmware_javascript_flags[];
extern "C" const unsigned char* firmware_resource(int identifier, size_t* size);

namespace {
struct Pixel { uint8_t blue, green, red, reserved; };
struct ModeInfo {
  uint32_t version, width, height, format, masks[4], stride;
};
struct Mode {
  uint32_t maximum, current;
  ModeInfo* information;
  size_t size;
  uint64_t framebuffer;
  size_t framebuffer_size;
};
struct Graphics {
  void* query;
  void* set;
  Status(UEFI_API *blit)(Graphics*, Pixel*, uint32_t, size_t, size_t, size_t,
                       size_t, size_t, size_t, size_t);
  Mode* mode;
};
constexpr int kAddressHeight = 48;
constexpr int kToolbarHeight = 80;
constexpr size_t kMaximumTabs = 4;
struct BrowserTab {
  size_t identifier = 0, presentations = 0;
  size_t inspected_identifier = 0;
  std::vector<Pixel> pixels;
  sk_sp<SkSurface> surface;
  std::unique_ptr<uefi_browser::BrowserFrame> frame;
  std::u16string address;
  size_t caret = 0;
  bool focused = true, selected = false, error = false;
};
struct BrowserContext {
  std::unique_ptr<uefi_browser::GpuCompositor> gpu_compositor;
  sk_sp<SkSurface> gpu_surface;
  bool gpu_dirty = false;
  SystemTable* table;
  base::RunLoop* run_loop;
  Graphics* graphics = nullptr;
  std::vector<Pixel> pixels;
  size_t width = 0;
  size_t height = 0;
  size_t frames_presented = 0;
  bool raster_presented = false;
  int64_t presentation_microseconds = 0;
  std::unique_ptr<uefi_browser::FirmwareInput> input;
  uefi_browser::BrowserFrame* frame = nullptr;
  blink::scheduler::WebThreadScheduler* scheduler = nullptr;
  std::vector<std::unique_ptr<BrowserTab>> tabs;
  size_t active_tab = 0, next_tab_identifier = 1;
  bool monitor = false;
  base::TimeTicks monitor_updated;
  void SaveAddress() {
    FinishInput();
    if (tabs.empty()) return;
    auto& tab = *tabs[active_tab];
    tab.address = address; tab.caret = caret; tab.focused = address_focused;
    tab.selected = selected; tab.error = address_error;
  }
  void SelectTab(size_t index, bool save_address = true) {
    if (index >= tabs.size()) return;
    if (save_address) SaveAddress();
    if (frame) frame->SetVisible(false);
    active_tab = index;
    auto& tab = *tabs[index];
    frame = tab.frame.get();
    address = tab.address; caret = tab.caret; address_focused = tab.focused;
    selected = tab.selected; address_error = tab.error;
    page_drag = false;
    frame->SetVisible(!monitor);
    std::printf("TABS: selected id=%zu count=%zu url=%s\n",tab.identifier,tabs.size(),frame->CurrentUrl().c_str());
    Present();
  }
  void OpenTab(std::string url) { NewTab(url.c_str()); }
  bool CanOpenDevTools() const {
    if (!frame || tabs.empty() || tabs[active_tab]->inspected_identifier ||
        (uefi_browser::RemoteRendererConfigured() && !renderer_runtime_available()))
      return false;
    for (const auto& tab : tabs)
      if (tab->inspected_identifier == tabs[active_tab]->identifier) return true;
    return tabs.size() < kMaximumTabs &&
           runtime_memory_used() <= runtime_memory_limit()*3/4;
  }
  void OpenDevTools() {
    if (!CanOpenDevTools()) return;
    if (monitor) ToggleMonitor();
    const size_t target = tabs[active_tab]->identifier;
    for (size_t index = 0; index < tabs.size(); ++index) {
      if (tabs[index]->inspected_identifier == target) { SelectTab(index); return; }
    }
    auto* inspected = frame;
    if (NewTab()) return;
    if (frame->Inspect(*inspected)) { CloseTab(active_tab); return; }
    tabs[active_tab]->inspected_identifier = target;
    address = base::UTF8ToUTF16(std::string_view(uefi_browser::kDevToolsFrontendUrl));
    caret = address.size(); address_focused = false; selected = false;
    std::printf("DEVTOOLS: opened for tab=%zu\n", target);
  }
  Status NewTab(const char* url = "about:blank") {
    if (uefi_browser::RemoteRendererConfigured() && !renderer_runtime_available()) return ERROR(6);
    if (tabs.size() >= kMaximumTabs || runtime_memory_used() > runtime_memory_limit()*3/4) {
      std::puts("TABS: creation refused by tab or tracked-heap limit");
      return 1;
    }
    SaveAddress();
    if (frame) frame->SetVisible(false);
    auto tab = std::make_unique<BrowserTab>();
    tab->identifier = next_tab_identifier++;
    tab->pixels.resize(width*(height-kToolbarHeight));
    tab->surface = gpu_compositor ? gpu_compositor->MakeSurface(width,height-kToolbarHeight) : SkSurfaces::WrapPixels(SkImageInfo::Make(width,height-kToolbarHeight,
        kBGRA_8888_SkColorType,kPremul_SkAlphaType),tab->pixels.data(),width*sizeof(Pixel));
    CHECK(tab->surface);
    tab->surface->getCanvas()->clear(SK_ColorWHITE);
    tab->frame = std::make_unique<uefi_browser::BrowserFrame>(table,*scheduler,
        *tab->surface->getCanvas(),gfx::Size(width,height-kToolbarHeight),
        base::BindRepeating(&BrowserContext::TabPresented,base::Unretained(this),tab->identifier),
        base::BindRepeating(&BrowserContext::OpenTab,base::Unretained(this)));
    tabs.push_back(std::move(tab));
    SelectTab(tabs.size()-1, false);
    Status result = frame->Navigate(url);
    address_focused = std::strcmp(url,"about:blank") == 0;
    address = address_focused ? std::u16string() : base::UTF8ToUTF16(url);
    caret = address.size();
    address_error = result != 0;
    std::printf("TABS: created id=%zu count=%zu\n",tabs[active_tab]->identifier,tabs.size());
    return result;
  }
  void CloseTab(size_t index) {
    if (index >= tabs.size()) return;
    SaveAddress();
    size_t old_active = active_tab;
    size_t identifier = tabs[index]->identifier;
    if (index == active_tab) frame = nullptr;
    tabs.erase(tabs.begin()+index);
    std::printf("TABS: closed id=%zu remaining=%zu\n",identifier,tabs.size());
    if (tabs.empty()) { active_tab = 0; if (NewTab()) run_loop->Quit(); return; }
    active_tab = old_active > index ? old_active-1 : std::min(old_active,tabs.size()-1);
    // The erased tab must not overwrite the surviving tab's saved address state.
    frame = nullptr;
    auto& tab = *tabs[active_tab];
    address = tab.address; caret = tab.caret; address_focused = tab.focused;
    selected = tab.selected; address_error = tab.error;
    SelectTab(active_tab, false);
  }
  void TabPresented(size_t identifier) {
    for (const auto& tab : tabs) if (tab->identifier == identifier) {
      ++tab->presentations;
      if (tab.get() == tabs[active_tab].get() && !monitor) {
        Present();
        if (!raster_presented) std::puts(gpu_compositor ? "BLINK: first frame submitted to GPU compositor" : "BLINK: first raster frame copied to GOP");
        raster_presented = true;
      }
      return;
    }
  }
  void ToggleMonitor() {
    FinishInput();
    monitor = !monitor;
    frame->SetVisible(!monitor);
    page_drag = false;
    std::printf("MONITOR: %s tabs=%zu\n",monitor ? "opened" : "closed",tabs.size());
    RenderProcessSnapshot snapshot{}; render_processes_snapshot(&snapshot);
    std::printf("MONITOR: workers=%zu tracked_heap=%zu\n",snapshot.workers,runtime_memory_used());
    Present();
  }
  int TabWidth() const { return std::max(100,std::min(220,(static_cast<int>(width)-188)/static_cast<int>(std::max<size_t>(1,tabs.size())))); }
  int TabButton(int x, int y) const {
    if (y < kAddressHeight || y >= kToolbarHeight) return -1;
    if (x >= static_cast<int>(width)-128) return 300;
    int index = (x-8)/TabWidth();
    if (x >= 8 && index >= 0 && static_cast<size_t>(index) < tabs.size())
      return ((x-8)%TabWidth() >= TabWidth()-28 ? 200 : 100)+index;
    if (x >= 8+static_cast<int>(tabs.size())*TabWidth() && x < 44+static_cast<int>(tabs.size())*TabWidth()) return 301;
    return -1;
  }
  void TabAction(int button) {
    if (button == 300) ToggleMonitor();
    else if (button == 301) NewTab();
    else if (button >= 200) CloseTab(button-200);
    else if (button >= 100) SelectTab(button-100);
  }
  void DrawTabs() {
    SkPaint paint; paint.setAntiAlias(true);
    const bool dark = uefi_browser::preferences.dark;
    paint.setColor(dark ? SkColorSetRGB(32,32,32) : SkColorSetRGB(232,234,237));
    toolbar->drawRect(SkRect::MakeXYWH(0,kAddressHeight,width,32),paint);
    for (size_t index=0;index<tabs.size();++index) {
      int left=8+index*TabWidth();
      paint.setColor(index==active_tab ? (dark ? SkColorSetRGB(60,60,60) : SK_ColorWHITE) : (dark ? SkColorSetRGB(32,32,32) : SkColorSetRGB(232,234,237)));
      toolbar->drawRoundRect(SkRect::MakeXYWH(left,50,TabWidth()-4,28),4,4,paint);
      paint.setColor(dark ? SK_ColorWHITE : SkColorSetRGB(36,36,36));
      std::string url=tabs[index]->frame->CurrentUrl();
      std::string label=url.empty() || url=="about:blank" ? "New tab" : url;
      if (tabs[index]->frame->NavigationPending()) label="Loading...";
      toolbar->save(); toolbar->clipRect(SkRect::MakeXYWH(left+8,50,TabWidth()-40,28));
      toolbar->drawString(label.c_str(),left+8,70,*font,paint); toolbar->restore();
      toolbar->drawString("x",left+TabWidth()-23,70,*font,paint);
    }
    paint.setColor(dark ? SK_ColorWHITE : SkColorSetRGB(36,36,36));
    toolbar->drawString(tabs.size()<kMaximumTabs ? "+" : "4/4",12+tabs.size()*TabWidth(),70,*font,paint);
    toolbar->drawString(monitor ? "Close monitor" : "Processes",width-120,70,*font,paint);
  }
  void DrawMonitor() {
    if (!monitor) return;
    RenderProcessSnapshot snapshot{}; render_processes_snapshot(&snapshot);
    monitor_updated=base::TimeTicks::Now();
    SkPaint paint; paint.setColor(uefi_browser::preferences.dark ? SkColorSetRGB(32,32,32) : SkColorSetRGB(246,247,249));
    toolbar->drawRect(SkRect::MakeXYWH(0,kToolbarHeight,width,height-kToolbarHeight),paint);
    paint.setColor(uefi_browser::preferences.dark ? SK_ColorWHITE : SkColorSetRGB(36,36,36));
    char line[512]; int y=116;
    auto draw=[&](const char* text) { toolbar->drawString(text,24,y,*font,paint); y+=28; };
    draw("Process monitor - live snapshot (500 ms); Shift+Esc closes");
    draw(renderer_runtime_available() ? "BSP: browser UI and network service | AP: Blink, V8, layout and Skia" :
         uefi_browser::RemoteRendererConfigured() ? "BSP: browser UI and network service | Renderer stopped; retry with End renderer" :
         "BSP: browser UI, Blink, V8, layout and Skia (single CPU fallback)");
    std::snprintf(line,sizeof(line),"BSP C heap: %.1f / %.1f MiB | backing requests: %.1f MiB (excludes VM and AP)",
        runtime_memory_used()/1048576.0,runtime_memory_limit()/1048576.0,runtime_memory_committed()/1048576.0); draw(line);
    std::snprintf(line,sizeof(line),"AP transfers: %zu | BSP fallback: %zu | failures: %zu | starts: %zu | idle stops: %zu",snapshot.frames,snapshot.fallback_frames,snapshot.failures,snapshot.starts,snapshot.idle_stops); draw(line);
    for (size_t index=0;index<snapshot.workers;++index) {
      std::snprintf(line,sizeof(line),"Worker %zu / CPU %zu / CPL3: %s - completed-frame transfer",index+2,snapshot.processor[index],snapshot.disabled ? "disabled" : snapshot.running ? (snapshot.stopping ? "stopping" : "running") : "idle"); draw(line);
    }
    if (renderer_runtime_available()) draw("Renderer AP: private runtime, TLS and page tables; BSP copies completed frames to GOP.");
    else if (!snapshot.workers) draw("No AP workers available; BSP presentation is active.");
    draw(renderer_runtime_sandboxed() ? "CPL3 renderer | supervisor-owned page tables and syscall broker | tabs share one renderer" :
        renderer_runtime_available() ? "Tabs share one renderer address space. CPL0 execution is not a security sandbox." : "Tabs share the main process. CPU percentages and per-tab memory are not measured.");
    for (size_t index=0;index<tabs.size();++index) {
      std::snprintf(line,sizeof(line),"Tab %zu | %s | %s | frames %zu | %.180s",tabs[index]->identifier,index==active_tab ? "selected" : "background",tabs[index]->frame->NavigationPending() ? "navigation pending" : "document committed",tabs[index]->presentations,tabs[index]->frame->CurrentUrl().c_str()); draw(line);
    }
    if (!uefi_browser::RemoteRendererConfigured()) {
      draw("Force termination requires a separate renderer; this session uses the BSP.");
      return;
    }
    draw("End renderer: click the button below or press Delete. All tab documents are discarded.");
    draw("After recovery, reload a tab or enter a URL. Address text and native tabs are retained.");
    paint.setColor(SkColorSetRGB(170,32,32));
    toolbar->drawRect(SkRect::MakeXYWH(24,height-70,230,42),paint);
    paint.setColor(SK_ColorWHITE);
    toolbar->drawString("End renderer",40,height-42,*font,paint);
  }
  int pointer_x = 0, pointer_y = 0;
  bool pointer_visible = false;
  bool cursor_drawn = false;
  int cursor_x = 0, cursor_y = 0;
  size_t cursor_presentations = 0;
  volatile uint32_t* framebuffer = nullptr;
  size_t framebuffer_stride = 0;
  uint32_t cursor_background[16 * 24]{};
  size_t LockCursor() {
    return reinterpret_cast<size_t(UEFI_API *)(size_t)>(table->boot->function[0])(8);
  }
  void UnlockCursor(size_t previous) {
    reinterpret_cast<void(UEFI_API *)(size_t)>(table->boot->function[1])(previous);
  }
  SkCanvas* toolbar = nullptr;
  std::unique_ptr<SkFont> font;
  std::u16string address;
  size_t caret = 0;
  uefi_browser::InputMethod input_method = uefi_browser::InputMethod::Latin;
  uefi_browser::TextComposer address_input;
  uefi_browser::ImePopup candidate_popup;
  bool candidate_pointer = false, candidate_dirty = false;
  size_t composition_start = 0, composition_length = 0;
  void FinishAddressInput(bool cancel = false) {
    candidate_dirty |= candidate_popup.Visible() || !address_input.Candidates().empty();
    auto committed = address_input.Finish(cancel);
    if (composition_length) {
      if (address.size() - composition_length + committed.size() > 4095)
        committed = address.substr(composition_start, composition_length);
      address.replace(composition_start, composition_length, committed);
      caret = composition_start + committed.size();
    }
    candidate_popup.Clear();
    composition_length = 0;
  }
  void FinishInput() {
    FinishAddressInput();
    if (frame) {
      uefi_browser::KeySample sample{};
      sample.input_method = input_method;
      sample.composition_action = uefi_browser::CompositionAction::Finish;
      frame->Keyboard(sample);
    }
  }
  void ToggleInputMethod(bool japanese = false) {
    FinishInput();
#if defined(UEFI_ENABLE_MOZC)
    if (japanese) input_method = input_method == uefi_browser::InputMethod::Japanese
        ? uefi_browser::InputMethod::Latin : uefi_browser::InputMethod::Japanese;
    else
#endif
      input_method = input_method == uefi_browser::InputMethod::Latin
          ? uefi_browser::InputMethod::Korean : uefi_browser::InputMethod::Latin;
    std::printf("IME: mode=%s\n", input_method == uefi_browser::InputMethod::Japanese ? "Japanese" :
        input_method == uefi_browser::InputMethod::Korean ? "Korean" : "Latin");
  }
  bool ComposeAddress(const uefi_browser::KeySample& sample) {
    bool pending = composition_length != 0;
    bool cancel = sample.key.scan == 23 || sample.key.character == 27;
    bool shortcut = (sample.shift & 0x80000000u) && (sample.shift & 252);
    if (cancel || shortcut || input_method == uefi_browser::InputMethod::Latin) {
      FinishAddressInput(cancel);
      return cancel && pending;
    }
    auto update = address_input.Feed(sample);
    if (!update.handled) {
      FinishAddressInput();
      return pending && sample.key.character == 13;
    }
    return ApplyAddressComposition(update);
  }
  bool ApplyAddressComposition(const uefi_browser::CompositionUpdate& update) {
    candidate_dirty |= candidate_popup.Visible() || !address_input.Candidates().empty();
    if (selected) { address.clear(); caret = 0; selected = false; }
    if (!composition_length) composition_start = caret;
    if (address.size() - composition_length + update.committed.size() + update.preedit.size() > 4095) {
      // The engine has already consumed the key; preserve the last accepted editor value.
      address_input.Clear();
      candidate_popup.Clear();
      composition_length = 0;
      return true;
    }
    address.replace(composition_start, composition_length, update.committed + update.preedit);
    composition_start += update.committed.size();
    composition_length = update.preedit.size();
    caret = composition_start + address_input.Cursor();
    address_error = false;
    return true;
  }
  bool address_focused = true, selected = false, address_error = false;
  unsigned pointer_buttons = 0;
  bool page_drag = false;
  int toolbar_pressed = -1;
  int AddressRight() const { return static_cast<int>(width) - 148; }
  int ToolbarButton(int x, int y) const {
    if (y >= 7 && y < 41) {
      if (x >= static_cast<int>(width) - 44 && x < static_cast<int>(width) - 12) return 3;
      if (x >= static_cast<int>(width) - 140 && x < static_cast<int>(width) - 52) return 4;
    }
    if (y < 7 || y >= 41 || x < 164 || x >= 276) return -1;
    int index = (x - 164) / 40;
    return (x - 164) % 40 < 32 ? index : -1;
  }
  void ToolbarAction(int index) {
    FinishInput();
    if (index == 4) {
      if (CanOpenDevTools()) {
        std::puts("UI: toolbar devtools");
        OpenDevTools();
      }
      return;
    }
    if ((index == 0 && !frame->CanGoBack()) || (index == 1 && !frame->CanGoForward())) return;
    address_focused = false;
    selected = false;
    Status result = index == 3 ? frame->Navigate("about:settings") : index == 0 ? frame->GoBack() : index == 1 ? frame->GoForward() : frame->Reload();
    address_error = result != 0;
    std::printf("UI: toolbar %s %s\n", index == 3 ? "settings" : index == 0 ? "back" : index == 1 ? "forward" : "reload", result ? "failed" : "started");
  }
  void FocusAddress() {
    FinishInput();
    address = base::UTF8ToUTF16(frame->CurrentUrl());
    if (address == u"about:blank") address.clear();
    caret = address.size();
    address_focused = true;
    selected = !address.empty();
    address_error = false;
  }
  void AddressKey(const uefi_browser::KeySample& sample) {
    if (!sample.key.scan && !sample.key.character) return;
    if (ComposeAddress(sample)) return;
    auto key = sample.key;
    bool control = (sample.shift & 0x80000000u) && (sample.shift & 12);
    if (key.character == 27 || key.scan == 23) { address_focused = false; return; }
    if (control && (key.character == 1 || key.character == 'a' || key.character == 'A')) {
      selected = true; caret = address.size(); return;
    }
    if (key.character == 13) {
      std::string value = base::UTF16ToUTF8(address);
      auto first = value.find_first_not_of(" \t\r\n");
      value = first == std::string::npos ? "about:blank" : value.substr(first, value.find_last_not_of(" \t\r\n") - first + 1);
      if (!uefi_browser::IsBrowserPage(GURL(value)) && value != "about:blank" && value.find("://") == std::string::npos) value = "https://" + value;
      GURL url(value);
      if (!url.is_valid() || (value != "about:blank" && !uefi_browser::IsBrowserPage(url) && !url.SchemeIsHTTPOrHTTPS()) || url.has_username() || url.has_password()) {
        address_error = true; return;
      }
      address = base::UTF8ToUTF16(url.spec());
      selected = false;
      address_error = false;
      Present();
      Status result = frame->Navigate(url.spec().c_str());
      address_error = result != 0;
      if (!result) address_focused = false;
      std::printf("UI: address navigation %s\n", result ? "failed" : "started");
      return;
    }
    if (key.scan == 3) { caret = selected ? address.size() : std::min(caret + 1, address.size()); selected = false; return; }
    if (key.scan == 4) { caret = selected ? 0 : (caret ? caret - 1 : 0); selected = false; return; }
    if (key.scan == 5 || key.scan == 6) { caret = key.scan == 5 ? 0 : address.size(); selected = false; return; }
    if (key.character == 8 || key.scan == 8 || (!control && key.character >= 32 && key.character != 127)) {
      if (selected) { address.clear(); caret = 0; selected = false; }
      else if (key.character == 8 && caret) { address.erase(--caret, 1); }
      else if (key.scan == 8 && caret < address.size()) address.erase(caret, 1);
      if (!control && key.character >= 32 && key.character != 127 && address.size() < 4095)
        address.insert(caret++, 1, key.character);
      address_error = false;
    }
  }
  float AddressText(const char16_t* text, size_t length, float left, const SkPaint* paint) {
    float advance = 0;
    auto manager = skia::DefaultFontMgr();
    for (size_t index = 0; index < length; ++index) {
      SkUnichar character = text[index];
      if (character >= 0xd800 && character <= 0xdbff && index + 1 < length &&
          text[index+1] >= 0xdc00 && text[index+1] <= 0xdfff)
        character = 0x10000 + ((character-0xd800)<<10) + text[++index]-0xdc00;
      SkFont selected_font(*font);
      if (!selected_font.unicharToGlyph(character)) {
        auto fallback = manager->matchFamilyStyleCharacter(nullptr, SkFontStyle(), nullptr, 0, character);
        if (fallback) selected_font.setTypeface(std::move(fallback));
      }
      if (paint) toolbar->drawSimpleText(&character, sizeof(character), SkTextEncoding::kUTF32,
          left + advance, 30, selected_font, *paint);
      advance += selected_font.measureText(&character, sizeof(character), SkTextEncoding::kUTF32);
    }
    return advance;
  }
  void DrawToolbar() {
    if (!toolbar || !font) return;
    if (!address_focused && frame && !frame->NavigationPending()) {
      address_error = frame->NavigationStatus() != 0;
      if (!address_error) address = base::UTF8ToUTF16(frame->CurrentUrl());
    }
    toolbar->save();
    toolbar->clipRect(SkRect::MakeWH(width, kToolbarHeight));
    SkPaint paint;
    paint.setAntiAlias(true);
    const bool dark = uefi_browser::preferences.dark;
    const SkColor foreground = dark ? SkColorSetRGB(245,245,245) : SkColorSetRGB(36,36,36);
    const SkColor surface = dark ? SkColorSetRGB(41,41,41) : SK_ColorWHITE;
    paint.setColor(dark ? SkColorSetRGB(32,32,32) : SkColorSetRGB(243,243,243));
    toolbar->drawRect(SkRect::MakeWH(width, kToolbarHeight), paint);
    paint.setColor(foreground);
    toolbar->drawString("UEFI BROWSER", 12, 25, *font, paint);
    SkFont mode_font(*font); mode_font.setSize(10);
    toolbar->drawString(input_method == uefi_browser::InputMethod::Japanese ? "JA  Shift+F9" : input_method == uefi_browser::InputMethod::Korean ? "KO  F9" : "EN  F9", 12, 40, mode_font, paint);
    for (int index = 0; index < 4; ++index) {
      bool enabled = frame && (index >= 2 || (index == 0 ? frame->CanGoBack() : frame->CanGoForward()));
      float left = index == 3 ? width - 44 : 164 + index * 40;
      paint.setStyle(SkPaint::kFill_Style);
      paint.setColor(toolbar_pressed == index ? (dark ? SkColorSetRGB(18,53,84) : SkColorSetRGB(235,243,252)) : surface);
      toolbar->drawRoundRect(SkRect::MakeXYWH(left, 7, 32, 34), 5, 5, paint);
      paint.setColor(enabled ? foreground : SkColorSetRGB(180, 185, 195));
      paint.setStyle(SkPaint::kStroke_Style);
      paint.setStrokeWidth(2);
      if (index == 3) {
        toolbar->drawCircle(left + 16, 24, 6, paint);
        toolbar->drawCircle(left + 16, 24, 2, paint);
        for (int tooth = 0; tooth < 8; ++tooth) {
          const float angle = tooth * 3.14159265f / 4;
          toolbar->drawLine(left + 16 + 6 * std::cos(angle), 24 + 6 * std::sin(angle),
              left + 16 + 9 * std::cos(angle), 24 + 9 * std::sin(angle), paint);
        }
      } else if (index < 2) {
        float tip = left + (index == 0 ? 10 : 22);
        float tail = left + (index == 0 ? 22 : 10);
        toolbar->drawLine(tail, 16, tip, 24, paint);
        toolbar->drawLine(tip, 24, tail, 32, paint);
      } else {
        toolbar->drawArc(SkRect::MakeXYWH(left + 8, 16, 16, 16), 30, 300, false, paint);
        toolbar->drawLine(left + 24, 13, left + 24, 21, paint);
        toolbar->drawLine(left + 24, 21, left + 17, 21, paint);
      }
    }
    paint.setStyle(SkPaint::kFill_Style);
    paint.setColor(address_error ? SkColorSetRGB(190, 35, 35) : address_focused ? SkColorSetRGB(40, 100, 200) : SkColorSetRGB(160, 165, 175));
    toolbar->drawRoundRect(SkRect::MakeXYWH(292, 7, AddressRight() - 292, 34), 6, 6, paint);
    paint.setColor(surface);
    toolbar->drawRoundRect(SkRect::MakeXYWH(294, 9, AddressRight() - 298, 30), 4, 4, paint);
    const bool tools_enabled = CanOpenDevTools();
    paint.setColor(tools_enabled && toolbar_pressed == 4
        ? (dark ? SkColorSetRGB(18,53,84) : SkColorSetRGB(235,243,252))
        : surface);
    toolbar->drawRoundRect(SkRect::MakeXYWH(width - 140, 7, 88, 34), 5, 5, paint);
    paint.setColor(tools_enabled ? foreground : SkColorSetRGB(180,185,195));
    toolbar->drawString("DevTools", width - 128, 29, *font, paint);
    toolbar->clipRect(SkRect::MakeXYWH(300, 10, AddressRight() - 308, 28));
    std::u16string display = address.empty() ? u"Enter URL (Ctrl+L)" : address;
    float offset = 0;
    if (address_focused && !address.empty()) {
      float advance = AddressText(address.data(), caret, 0, nullptr);
      offset = std::max(0.0f, advance - static_cast<float>(AddressRight() - 324));
      if (selected) { paint.setColor(SkColorSetRGB(195, 215, 250)); toolbar->drawRect(SkRect::MakeXYWH(300, 10, AddressRight() - 308, 28), paint); }
      paint.setColor(foreground);
      toolbar->drawRect(SkRect::MakeXYWH(302 + advance - offset, 13, 1, 22), paint);
    }
    paint.setColor(address.empty() ? SkColorSetRGB(110, 115, 125) : foreground);
    AddressText(display.data(), display.size(), 302 - offset, &paint);
    if (address_focused && composition_length) {
      float start = AddressText(address.data(), composition_start, 0, nullptr);
      float end = AddressText(address.data(), composition_start + composition_length, 0, nullptr);
      toolbar->drawLine(302 + start - offset, 33, 302 + end - offset, 33, paint);
    }
    toolbar->restore();
    DrawTabs();
  }
  void Present() {
    if (gpu_compositor && !gpu_compositor->CanPresent()) {
      // Recompose the latest state after completion instead of recording discarded frames.
      gpu_dirty = true;
      return;
    }
    auto started = base::TimeTicks::Now();
    if (gpu_compositor) {
      toolbar->clear(SK_ColorWHITE);
      if (!tabs.empty() && !monitor)
        toolbar->drawImage(tabs[active_tab]->surface->makeImageSnapshot(),0,kToolbarHeight);
    }
    if (!gpu_compositor && !tabs.empty() && !monitor)
      std::memcpy(pixels.data()+width*kToolbarHeight,tabs[active_tab]->pixels.data(),width*(height-kToolbarHeight)*sizeof(Pixel));
    DrawToolbar();
    if (!monitor && frame && frame->NavigationStatus()==ERROR(21)) {
      SkPaint message; message.setColor(SkColorSetRGB(48,48,48));
      toolbar->drawString("Renderer stopped. Reload this tab or enter a URL to continue.",32,150,*font,message);
      toolbar->drawString("The document and its session history were discarded. Other native tabs remain open.",32,180,*font,message);
    }
    DrawMonitor();
    if (address_focused) candidate_popup.Draw(*toolbar, address_input.Candidates(), 302, 48, width, height);
    else candidate_popup.Clear();
    candidate_dirty = false;
    if (gpu_compositor) {
      if (pointer_visible) {
        SkPaint cursor;
        for (int y=0;y<20;++y) {
          cursor.setColor(SK_ColorBLACK);
          toolbar->drawRect(SkRect::MakeXYWH(pointer_x,pointer_y+y,y/2+1,1),cursor);
          if (y>3 && y<19) {
            cursor.setColor(SK_ColorWHITE);
            toolbar->drawRect(SkRect::MakeXYWH(pointer_x+1,pointer_y+y,y/2-1,1),cursor);
          }
        }
      }
      gpu_dirty = !gpu_compositor->Present(*gpu_surface);
      presentation_microseconds += (base::TimeTicks::Now()-started).InMicroseconds();
      ++frames_presented;
      return;
    }
    size_t previous = LockCursor();
    RestoreCursor();
    Status result = 0;
    bool parallel = framebuffer && render_processes_present(pixels.data(),
        const_cast<uint32_t*>(framebuffer), width, height, framebuffer_stride, graphics->mode->information->format == 0);
    if (!parallel) result = graphics->blit(graphics, pixels.data(), 2, 0, 0, 0, 0,
                                   width, height, width * sizeof(Pixel));
    CHECK_EQ(result, 0u);
    DrawCursor();
    UnlockCursor(previous);
    presentation_microseconds += (base::TimeTicks::Now() - started).InMicroseconds();
    ++frames_presented;
  }
  void DrawCursor() {
    if (framebuffer && pointer_visible) {
      for (size_t y = 0; y < std::min<size_t>(24, height - pointer_y); ++y) {
        for (size_t x = 0; x < std::min<size_t>(16, width - pointer_x); ++x) {
          size_t offset = (pointer_y + y) * framebuffer_stride + pointer_x + x;
          cursor_background[y * 16 + x] = framebuffer[offset];
          if (y < 20 && x <= y / 2) {
            bool edge = x == 0 || x == y / 2 || y == 19;
            framebuffer[offset] = edge ? 0 : 0x00ffffff;
          }
        }
      }
      cursor_drawn = true;
      cursor_x = pointer_x;
      cursor_y = pointer_y;
      return;
    }
    if (pointer_visible) {
      // The cursor is composed only into the GOP copy, preserving page pixels.
      Pixel cursor[16 * 24];
      size_t cursor_width = std::min<size_t>(16, width - pointer_x);
      size_t cursor_height = std::min<size_t>(24, height - pointer_y);
      for (size_t y = 0; y < cursor_height; ++y) {
        for (size_t x = 0; x < cursor_width; ++x) {
          Pixel color = pixels[(pointer_y + y) * width + pointer_x + x];
          if (y < 20 && x <= y / 2) {
            uint8_t shade = x == 0 || x == y / 2 || y == 19 ? 0 : 255;
            color = {shade, shade, shade, 0};
          }
          cursor[y * 16 + x] = color;
        }
      }
      graphics->blit(graphics, cursor, 2, 0, 0, pointer_x, pointer_y,
          cursor_width, cursor_height, 16 * sizeof(Pixel));
    }
    cursor_drawn = pointer_visible;
    cursor_x = pointer_x;
    cursor_y = pointer_y;
  }
  void RestoreCursor() {
    if (!cursor_drawn) return;
    if (framebuffer) {
      for (size_t y = 0; y < std::min<size_t>(24, height - cursor_y); ++y)
        for (size_t x = 0; x < std::min<size_t>(16, width - cursor_x); ++x)
          framebuffer[(cursor_y + y) * framebuffer_stride + cursor_x + x] = cursor_background[y * 16 + x];
      cursor_drawn = false;
      return;
    }
    CHECK_EQ(graphics->blit(graphics, pixels.data(), 2, cursor_x, cursor_y,
        cursor_x, cursor_y, std::min<size_t>(16, width - cursor_x),
        std::min<size_t>(24, height - cursor_y), width * sizeof(Pixel)), 0u);
  }
  void PresentInput(bool toolbar_changed) {
    if (gpu_compositor) { ++cursor_presentations; Present(); return; }
    if (candidate_dirty || (toolbar_changed && !address_input.Candidates().empty())) { Present(); return; }
    if (toolbar_changed) DrawToolbar();
    size_t previous = LockCursor();
    RestoreCursor();
    if (toolbar_changed) {
      CHECK_EQ(graphics->blit(graphics, pixels.data(), 2, 0, 0, 0, 0,
          width, kToolbarHeight, width * sizeof(Pixel)), 0u);
    }
    DrawCursor();
    ++cursor_presentations;
    UnlockCursor(previous);
  }
  static void ObservePointer(void* pointer, int x, int y) {
    auto& context = *static_cast<BrowserContext*>(pointer);
    if (context.pointer_visible && context.pointer_x == x && context.pointer_y == y) return;
    // At TPL_CALLBACK only framebuffer memory is touched; firmware and Blink are not called.
    context.RestoreCursor();
    context.pointer_x = x;
    context.pointer_y = y;
    context.pointer_visible = true;
    context.DrawCursor();
    ++context.cursor_presentations;
  }
  static bool Poll(void* pointer) {
    firmware_audio_poll();
    firmware_media_poll();
    auto& context = *static_cast<BrowserContext*>(pointer);
    if (!context.input || !context.frame) return false;
    if (context.gpu_compositor) {
      context.gpu_compositor->Poll();
      if (context.gpu_dirty) context.Present();
    }
    uefi_browser::PollRemoteFrames();
    bool handled = false;
    bool toolbar_changed = false;
    uefi_browser::PointerSample pointer_sample{};
    const auto pointer_deadline = base::TimeTicks::Now() + base::Milliseconds(4);
    for (int count = 0; count < 8 && context.frame && (renderer_runtime_available() || context.frame->CanAcceptInput()) && context.input->ReadPointer(pointer_sample); ++count) {
      uefi_browser::PointerSample latest = pointer_sample;
      context.input->PeekPointer(latest);
      if (!context.framebuffer) {
        context.pointer_x = latest.x;
        context.pointer_y = latest.y;
      }
      toolbar_changed |= pointer_sample.buttons != context.pointer_buttons;
      context.pointer_visible = true;
      bool pressed = (pointer_sample.buttons & 1) && !(context.pointer_buttons & 1);
      int candidate = context.candidate_popup.Hit(pointer_sample.x, pointer_sample.y);
      if (context.candidate_pointer || (pressed && candidate >= 0)) {
        if (pressed && candidate >= 0) context.ApplyAddressComposition(context.address_input.Select(candidate));
        context.candidate_pointer = (pointer_sample.buttons & 1) != 0;
        context.pointer_buttons = pointer_sample.buttons;
        handled = toolbar_changed = true;
        continue;
      }
      if (pressed) {
        context.FinishAddressInput();
        if (pointer_sample.y < kAddressHeight) {
          context.toolbar_pressed = context.ToolbarButton(pointer_sample.x, pointer_sample.y);
          if (pointer_sample.x >= 292 && pointer_sample.x < context.AddressRight()) context.FocusAddress();
        }
        else if (pointer_sample.y < kToolbarHeight) context.toolbar_pressed = context.TabButton(pointer_sample.x,pointer_sample.y);
        else if (context.monitor && pointer_sample.x>=24 && pointer_sample.x<254 &&
                 pointer_sample.y>=static_cast<int>(context.height)-70 && pointer_sample.y<static_cast<int>(context.height)-28)
          context.toolbar_pressed=400;
        else if (!context.monitor) { context.address_focused = false; context.page_drag = true; }
      }
      if (!context.monitor && (pointer_sample.y >= kToolbarHeight || context.page_drag)) {
        auto page = pointer_sample;
        page.y -= kToolbarHeight;
        context.frame->Pointer(page);
      } else context.frame->PointerLeave();
      if (!(pointer_sample.buttons & 1) && (context.pointer_buttons & 1)) {
        int button = context.toolbar_pressed;
        context.toolbar_pressed = -1;
        if (button==400 && context.monitor && pointer_sample.x>=24 && pointer_sample.x<254 &&
            pointer_sample.y>=static_cast<int>(context.height)-70 && pointer_sample.y<static_cast<int>(context.height)-28)
          uefi_browser::TerminateRenderer();
        else if (button >= 100 && context.TabButton(pointer_sample.x,pointer_sample.y) == button)
          context.TabAction(button);
        else if (button >= 0 && button < 100 && context.ToolbarButton(pointer_sample.x, pointer_sample.y) == button)
          context.ToolbarAction(button);
      }
      if (!pointer_sample.buttons) context.page_drag = false;
      context.pointer_buttons = pointer_sample.buttons;
      handled = true;
      if (base::TimeTicks::Now() >= pointer_deadline) break;
    }
    uefi_browser::KeySample key{};
    const auto key_deadline = base::TimeTicks::Now() + base::Milliseconds(4);
    for (int count = 0; count < 8 && context.frame && (renderer_runtime_available() || context.frame->CanAcceptInput()) && context.input->ReadKey(key); ++count) {
      toolbar_changed = true;
      bool control = (key.shift & 0x80000000u) && (key.shift & 12);
      bool shift = (key.shift & 0x80000000u) && (key.shift & 3);
      auto character = key.key.character;
      key.input_method = context.input_method;
      if (key.key.scan == 17 && firmware_media_probe()) {}
      else if (key.key.scan == 19 || (control && character == ' ')) context.ToggleInputMethod(shift && key.key.scan == 19);
      else if (shift && (key.key.scan == 23 || character == 27)) context.ToggleMonitor();
      else if ((key.key.scan == 20 && !context.tabs.empty() &&
                !context.tabs[context.active_tab]->inspected_identifier) ||
               (control && shift && (character == 'i' || character == 'I'))) context.OpenDevTools();
      else if (context.monitor && (key.key.scan == 23 || character == 27)) context.ToggleMonitor();
      else if (context.monitor && key.key.scan==8) uefi_browser::TerminateRenderer();
      else if (control && (character == 20 || character == 't' || character == 'T')) context.NewTab();
      else if (control && (character == 23 || character == 'w' || character == 'W')) context.CloseTab(context.active_tab);
      else if (control && character == 9) context.SelectTab((context.active_tab+context.tabs.size()+(shift ? -1 : 1))%context.tabs.size());
      else if (control && character >= '1' && character <= '9') context.SelectTab(character == '9' ? context.tabs.size()-1 : character-'1');
      else if (key.key.scan == 22) context.run_loop->Quit();
      else if ((key.shift & 0x80000000u) && (key.shift & 12) &&
               (key.key.character == 12 || key.key.character == 'l' || key.key.character == 'L')) context.FocusAddress();
      else if ((key.shift & 0x80000000u) && (key.shift & 12) && key.key.character == ',') context.ToolbarAction(3);
      else if (context.address_focused) context.AddressKey(key);
      else if (!context.monitor) context.frame->Keyboard(key);
      handled = true;
      if (base::TimeTicks::Now() >= key_deadline) break;
    }
    if (handled) context.PresentInput(toolbar_changed);
    renderer_runtime_service();
    resource_async_process();
    render_processes_idle();
    if (context.monitor && base::TimeTicks::Now()-context.monitor_updated >= base::Milliseconds(500)) context.Present();
    return handled;
  }
};
class FirmwareGPUAccess final : public blink::mojom::GpuDataManager {
 public:
  void Bind(mojo::PendingReceiver<blink::mojom::GpuDataManager> receiver) {
    receivers_.Add(this, std::move(receiver));
  }
  void Are3DAPIsBlockedForUrl(const GURL& url,
      Are3DAPIsBlockedForUrlCallback callback) override {
    std::move(callback).Run(!url.SchemeIsHTTPOrHTTPS());
  }
 private:
  mojo::ReceiverSet<blink::mojom::GpuDataManager> receivers_;
};
class FirmwareProcessBroker final : public blink::ThreadSafeBrowserInterfaceBrokerProxy {
 public:
  void GetInterfaceImpl(mojo::GenericPendingReceiver receiver) override {
    if (auto gpu = receiver.As<blink::mojom::GpuDataManager>()) {
      static FirmwareGPUAccess access;
      access.Bind(std::move(gpu));
      return;
    }
    if (auto blob = receiver.As<blink::mojom::BlobRegistry>()) {
      uefi_browser::BindBlobRegistry(std::move(blob));
      return;
    }
    if (receiver.interface_name()) std::printf("PROCESS BROKER: unsupported interface %s\n", receiver.interface_name()->c_str());
  }
};
class FirmwarePlatform final : public blink::Platform {
 public:
  blink::WebCrypto* Crypto() override { return &crypto_; }
  std::unique_ptr<blink::WebGraphicsContext3DProvider>
  CreateWebGLGraphicsContextProvider(bool, bool fail_if_major_performance_caveat,
      WebGLContextType type, const blink::WebURL&, WebGLContextInfo* information) override {
    if (fail_if_major_performance_caveat) return nullptr;
    return uefi_browser::CreateWebGLProvider(type, information);
  }
  blink::ThreadSafeBrowserInterfaceBrokerProxy* GetBrowserInterfaceBroker() override {
    static scoped_refptr<FirmwareProcessBroker> broker = base::MakeRefCounted<FirmwareProcessBroker>();
    return broker.get();
  }
  std::unique_ptr<blink::WebDedicatedWorkerHostFactoryClient>
  CreateDedicatedWorkerHostFactoryClient(blink::WebDedicatedWorker* worker,
      const blink::BrowserInterfaceBrokerProxy&) override {
    return uefi_browser::CreateWorkerHost(worker);
  }
  void CreateWebGPUGraphicsContext3DProviderAsync(
      const blink::WebURL&, WebGPUReplyThread,
      base::OnceCallback<void(std::unique_ptr<blink::WebGraphicsContext3DProvider>)> callback) override {
    base::SingleThreadTaskRunner::GetCurrentDefault()->PostTask(
        FROM_HERE, base::BindOnce(std::move(callback),
            uefi_browser::CreateWebGPUProvider()));
  }
  scoped_refptr<base::SingleThreadTaskRunner> GetIOTaskRunner() const override {
    // Firmware I/O and renderer work share the single main-thread event loop.
    return base::SingleThreadTaskRunner::GetCurrentDefault();
  }
  std::unique_ptr<NestedMessageLoopRunner> CreateNestedMessageLoopRunner() const override {
    class Runner final : public NestedMessageLoopRunner {
     public:
      void Run() override {
        CHECK(!active_);
        base::RunLoop loop(base::RunLoop::Type::kNestableTasksAllowed);
        active_ = &loop;
        loop.Run();
        active_ = nullptr;
      }
      void QuitNow() override {
        if (active_) active_->Quit();
      }
     private:
      base::RunLoop* active_ = nullptr;
    };
    return std::make_unique<Runner>();
  }
  bool HasDataResource(int identifier) const override {
    size_t size;
    return firmware_resource(identifier, &size) != nullptr;
  }
  blink::WebData GetDataResource(int identifier,
      ui::ResourceScaleFactor) override {
    size_t size;
    const auto* data = firmware_resource(identifier, &size);
    return data ? blink::WebData(base::span(data, size)) : blink::WebData();
  }
  scoped_refptr<base::RefCountedMemory> GetDataResourceBytes(int identifier) override {
    size_t size;
    const auto* data = firmware_resource(identifier, &size);
    if (!data)
      return nullptr;
    return base::MakeRefCounted<base::RefCountedStaticMemory>(base::span(data, size));
  }
  std::string GetDataResourceString(int identifier) override {
    size_t size;
    const auto* data = firmware_resource(identifier, &size);
    CHECK(data) << "Missing embedded Blink resource " << identifier;
    if (size >= 8 && data[0] == 0x1e && data[1] == 0x9b) {
      size_t decoded_size = 0;
      for (size_t index = 0; index < 6; ++index)
        decoded_size |= static_cast<size_t>(data[index + 2]) << (8 * index);
      CHECK_LE(decoded_size, 16u * 1024 * 1024);
      std::string result(decoded_size, '\0');
      CHECK_EQ(BrotliDecoderDecompress(size - 8, data + 8, &decoded_size,
          reinterpret_cast<uint8_t*>(result.data())), BROTLI_DECODER_RESULT_SUCCESS);
      CHECK_EQ(decoded_size, result.size());
      return result;
    }
    if (size >= 2 && data[0] == 0x1f && data[1] == 0x8b) {
      std::string result;
      CHECK(compression::GzipUncompress(base::span(data, size), &result));
      return result;
    }
    return std::string(reinterpret_cast<const char*>(data), size);
  }
  blink::UserAgentMetadata UserAgentMetadata() override {
    return uefi_browser::BrowserUserAgentMetadata();
  }
  blink::WebString UserAgent() override {
    return blink::WebString::FromUtf8(firmware_browser_user_agent());
  }
  blink::WebString DefaultLocale() override {
    return blink::WebString::FromAscii("ko-KR");
  }
 private:
  webcrypto::WebCryptoImpl crypto_;
};
}

static void InitializeNetworkState() {
  // Firmware does not report link changes; unknown connectivity must still initialize Blink's state.
  blink::WebNetworkStateNotifier::SetOnLine(true);
  blink::WebNetworkStateNotifier::SetWebConnection(
      blink::kWebConnectionTypeUnknown, std::numeric_limits<double>::infinity());
  blink::WebNetworkStateNotifier::SetSaveDataEnabled(false);
}

extern "C" int run_blink_browser(SystemTable* table) {
  fflush(stdout);
  setvbuf(stdout, nullptr, _IONBF, 0);
  auto discardable_allocator = uefi_browser::CreateDiscardableAllocator();
  {
    auto memory = discardable_allocator->AllocateLockedDiscardableMemory(64);
    CHECK(memory);
    *static_cast<unsigned char*>(memory->data()) = 0x5a;
    discardable_allocator->ReleaseFreeMemory();
    CHECK_EQ(discardable_allocator->GetBytesAllocated(), 64u);
    memory->Unlock();
    CHECK(memory->Lock());
    CHECK_EQ(*static_cast<unsigned char*>(memory->data()), 0x5a);
    memory->Unlock();
    discardable_allocator->ReleaseFreeMemory();
    CHECK(!memory->Lock());
    CHECK_EQ(discardable_allocator->GetBytesAllocated(), 0u);
    std::puts("PASS: discardable memory lock, retention, purge and accounting");
  }
  FirmwarePlatform platform;
  base::AtExitManager at_exit;
  const std::string javascript_argument =
      std::string("--js-flags=") + firmware_javascript_flags;
  const char* arguments[] = {"uefi-browser", javascript_argument.c_str()};
  std::printf("BLINK: V8 flags %s\n", firmware_javascript_flags);
  std::puts("BLINK: initialize command line");
  CHECK(base::CommandLine::Init(2, arguments));
  // Worker-dependent parsing is disabled; batching must not bypass native input polls.
  std::puts("BLINK: initialize feature list");
  // The inspector frontend has its own browsing context group and must remain
  // runnable while its inspected page is paused in the shared renderer.
  base::FeatureList::InitInstance("PausePagesPerBrowsingContextGroup",
      "BackgroundResourceFetch,ThreadedPreloadScanner,PrecompileInlineScripts,"
      "ScriptStreaming,ThreadedBodyLoader,ConsumeCodeCacheOffThread,RunTasksByBatches");
  base::MemoryPressureListenerRegistry memory_pressure_registry;
  std::puts("BLINK: initialize thread pool");
  base::ThreadPoolInstance::Create("UEFIBrowser");
  std::puts("BLINK: initialize ICU");
  CHECK(base::i18n::InitializeICU());
  std::puts("BLINK: initialize Mojo");
  mojo::core::Init();
  std::puts("BLINK: initialize fonts");
  uefi_browser::InstallFonts();
  uefi_browser::LoadPreferences(table);
  if (renderer_runtime_is_child()) {
    mojo::BinderMap binders;
    blink::Platform::InitializeBlink();
    auto scheduler=uefi_browser::CreateScheduler(table,uefi_browser::PollRenderer,nullptr);
    uefi_browser::TaskDiagnostics task_diagnostics;
    blink::Initialize(&platform,&binders,scheduler.get());
    InitializeNetworkState();
    int result=uefi_browser::RunRenderer(table,*scheduler);
    task_diagnostics.Stop();
    scheduler->Shutdown();
    return result;
  }
  BrowserContext context{};
  context.table = table;
  Guid graphics_guid = {0x9042a9de, 0x23dc, 0x4a38,
                         {0x96, 0xfb, 0x7a, 0xde, 0xd0, 0x80, 0x51, 0x6a}};
  auto locate = reinterpret_cast<Status(UEFI_API *)(Guid*, void*, void**)>(
      table->boot->function[37]);
  CHECK_EQ(locate(&graphics_guid, nullptr,
                  reinterpret_cast<void**>(&context.graphics)), 0u);
  CHECK(context.graphics);
  context.width = context.graphics->mode->information->width;
  context.height = context.graphics->mode->information->height;
  CHECK_GT(context.width, 0u);
  CHECK_GT(context.height, 400u);
  CHECK_GE(context.width, 640u);
  CHECK_LE(context.width, 4096u);
  CHECK_LE(context.height, 4096u);
  context.pixels.resize(context.width * context.height);
  auto toolbar_surface = SkSurfaces::WrapPixels(
      SkImageInfo::Make(static_cast<int>(context.width), static_cast<int>(context.height),
          kBGRA_8888_SkColorType, kPremul_SkAlphaType), context.pixels.data(), context.width * sizeof(Pixel));
  CHECK(toolbar_surface);
  context.toolbar = toolbar_surface->getCanvas();
  context.font = std::make_unique<SkFont>(skia::DefaultFontMgr()->matchFamilyStyle("Noto Sans", SkFontStyle()), 15);
  toolbar_surface->getCanvas()->clear(SK_ColorWHITE);
  mojo::BinderMap binders;
  std::puts("BLINK: initialize platform heap");
  blink::Platform::InitializeBlink();
  auto scheduler = uefi_browser::CreateScheduler(table, BrowserContext::Poll,
                                                 &context);
  uefi_browser::TaskDiagnostics task_diagnostics;
  base::RunLoop run_loop;
  context.run_loop = &run_loop;
  std::puts("BLINK: initialize");
  blink::Initialize(&platform, &binders, scheduler.get());
  InitializeNetworkState();
  int navigation_result = 0;
  {
    context.scheduler = scheduler.get();
    context.input = std::make_unique<uefi_browser::FirmwareInput>(table, context.width, context.height);
    context.address = base::UTF8ToUTF16(firmware_browser_url);
    context.address_focused = context.address == u"about:blank";
    if (context.address_focused) context.address.clear();
    const auto* mode = context.graphics->mode;
    if (mode->information->format <= 1 && mode->framebuffer &&
        mode->information->stride >= context.width &&
        mode->framebuffer_size / sizeof(uint32_t) / mode->information->stride >= context.height) {
      context.framebuffer = reinterpret_cast<volatile uint32_t*>(mode->framebuffer);
      context.framebuffer_stride = mode->information->stride;
      context.input->SetPointerObserver(BrowserContext::ObservePointer, &context);
      std::puts("INPUT: timer framebuffer cursor enabled");
    }
    CHECK_EQ(resource_async_initialize(table), 0u);
    bool remote=uefi_browser::StartRenderer(table,context.width,context.height-kToolbarHeight);
    if (!remote && renderer_runtime_sandboxed()) {
      std::puts("FAIL: CPL3 renderer unavailable; privileged rendering is disabled in this build");
    }
    bool permitted=remote || !renderer_runtime_sandboxed();
    if (permitted) {
      if (uefi_browser::GpuCompositor::Requested()) {
        // Disconnecting the display driver invalidates its GOP protocol and cursor observer.
        context.input->SetPointerObserver(nullptr,nullptr);
        context.framebuffer=nullptr;
      }
      context.gpu_compositor=uefi_browser::GpuCompositor::Create(context.width,context.height);
      if (uefi_browser::GpuCompositor::Requested() && !context.gpu_compositor) {
        permitted=false;
        std::puts("FAIL: GPU compositor unavailable; stopped before using a disconnected GOP");
      }
      if (context.gpu_compositor) {
        context.gpu_surface=context.gpu_compositor->MakeSurface(context.width,context.height);
        CHECK(context.gpu_surface);
        context.toolbar=context.gpu_surface->getCanvas();
        context.input->SetPointerObserver(nullptr,nullptr);
        context.framebuffer=nullptr;
      }
    }
    if (permitted && !remote && context.framebuffer) render_processes_initialize(table, context.width * context.height);
    Status result = permitted ? context.NewTab(uefi_browser::preferences.startup_settings ? "about:settings" : firmware_browser_url) : ERROR(3);
    if (result) {
      std::printf("FAIL: navigation transport status 0x%llx\n",
                  static_cast<unsigned long long>(result));
      navigation_result = 1;
    } else {
      run_loop.Run();
    }
    std::printf("PERF: full_presentations=%zu presentation_us=%lld\n", context.frames_presented, static_cast<long long>(context.presentation_microseconds));
    std::printf("PERF: cursor_presentations=%zu\n", context.cursor_presentations);
    context.input->SetPointerObserver(nullptr, nullptr);
    uefi_browser::RequestRendererStop();
    context.frame = nullptr;
    context.tabs.clear();
    context.input.reset();
  }
  uefi_browser::StopRenderer();
  context.gpu_surface.reset();
  context.gpu_compositor.reset();
  firmware_media_shutdown();
  render_processes_shutdown();
  resource_async_shutdown(table);
  task_diagnostics.Stop();
  scheduler->Shutdown();
  return navigation_result || !context.frames_presented ? 1 : 0;
}

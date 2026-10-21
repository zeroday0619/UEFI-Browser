#include "blink/remote_frame.h"
#include "blink/scroll_snapshot.h"
#include "blink/scroll_scene.h"
#include "blink/scroll_wire.h"
#include "third_party/skia/include/core/SkImage.h"
#include "third_party/skia/include/core/SkPixmap.h"
#include "renderer_runtime.h"
extern "C" {
#include "platform/runtime.h"
}
#include "base/memory/memory_pressure_listener_registry.h"
#include "third_party/blink/public/platform/web_cache.h"
#include "third_party/blink/public/web/blink.h"
#include "third_party/skia/include/core/SkGraphics.h"
#include "blink/browser_settings.h"
#include "base/run_loop.h"
#include "base/functional/bind.h"
#include "base/check.h"
#include "base/time/time.h"
#include "third_party/skia/include/core/SkCanvas.h"
#include "third_party/skia/include/core/SkSurface.h"
#include <algorithm>
#include <array>
#include <cstdio>
#include <cstddef>
#include <cstring>
#include <vector>
#include <deque>
namespace uefi_browser {
namespace {
constexpr unsigned kSlots=8, kCommands=256;
enum Operation { Create, Close, Navigate, Back, Forward, Reload, Visible, Pointer, Keyboard, Scroll, Gutter, Quit, Inspect };
struct Command {
  unsigned operation, slot;
  uint64_t generation, navigation, scroll_sequence, scroll_element, focus_sequence;
  uint64_t target_generation;
  unsigned target_slot;
  PointerSample pointer;
  KeySample key;
  bool visible;
  char url[4096];
};
struct Publication {
  uint32_t ready, closed;
  uint64_t generation, frame, navigation, focus_acknowledgement, keyboard_element;
  bool pending, back, forward;
  Status status;
  char url[4096], opened_url[4096];
  BrowserPreferences preferences;
  uint32_t *pixels;
  uint64_t scroll_acknowledgement;
  const ScrollScene* scroll_scene;
  uint32_t scroll_bytes;
};
struct Channel {
  uint32_t head, tail, ready, stopping;
  unsigned width, height;
  Command commands[kCommands];
  Publication slots[kSlots];
  uint32_t scroll_owner;
};
static_assert(offsetof(Channel,width)==4*sizeof(uint32_t) &&
              offsetof(Channel,height)==5*sizeof(uint32_t),
              "The boundary probe exercises the shared command header layout.");
static Channel *channel;
static size_t channel_bytes;
static unsigned viewport_width,viewport_height;
static uint32_t *trusted_pixels[kSlots];
static uint8_t *trusted_scroll_bytes;
static void ReleaseChannel() {
  if (renderer_runtime_sandboxed()) renderer_runtime_shared_free(channel,channel_bytes);
  else { for (auto& slot:channel->slots) delete[] slot.pixels; delete channel; }
  channel=nullptr;
}
static std::array<RemoteFrame*,kSlots> clients{};
static uint64_t next_generation=1;
static bool termination_requested=false;
static bool Capacity() {
  return channel && __atomic_load_n(&channel->head,__ATOMIC_RELAXED)-
      __atomic_load_n(&channel->tail,__ATOMIC_ACQUIRE)<kCommands;
}
static bool InputCapacity() {
  return channel && __atomic_load_n(&channel->head,__ATOMIC_RELAXED)-
      __atomic_load_n(&channel->tail,__ATOMIC_ACQUIRE)<kCommands-32;
}
static bool Send(const Command& command) {
  if (!Capacity()) return false;
  uint32_t head=__atomic_load_n(&channel->head,__ATOMIC_RELAXED);
  channel->commands[head%kCommands]=command;
  __atomic_store_n(&channel->head,head+1,__ATOMIC_RELEASE);
  return true;
}
struct RendererTab {
  uint64_t generation=0, frame=0, navigation=0, focus_sequence=0;
  std::vector<uint32_t> pixels;
  std::shared_ptr<ScrollScene> published_scene;
  std::shared_ptr<const std::vector<uint8_t>> serialized_scene;
  sk_sp<SkSurface> surface;
  std::unique_ptr<BrowserFrame> browser;
  std::string opened_url;
  void Presented() { ++frame; }
  void Open(std::string url) { opened_url=std::move(url); }
};
static std::array<RendererTab,kSlots> server;
static SystemTable *server_table;
static blink::scheduler::WebThreadScheduler *server_scheduler;
static base::RunLoop *server_loop;
static void Publish(unsigned index) {
  auto& tab=server[index];
  auto& output=channel->slots[index];
  if (!tab.browser || __atomic_load_n(&output.ready,__ATOMIC_ACQUIRE)) return;
  if (renderer_runtime_sandboxed() && __atomic_load_n(&channel->scroll_owner,__ATOMIC_ACQUIRE)) return;
  std::string url=tab.browser->CurrentUrl();
  uint64_t keyboard_element=tab.browser->KeyboardScrollTarget();
  if (output.generation==tab.generation && output.navigation==tab.navigation && output.frame==tab.frame &&
      output.pending==tab.browser->NavigationPending() && output.status==tab.browser->NavigationStatus() &&
      output.back==tab.browser->CanGoBack() && output.forward==tab.browser->CanGoForward() &&
      output.focus_acknowledgement==tab.focus_sequence && output.keyboard_element==keyboard_element &&
      url==output.url && tab.opened_url.empty() && !std::memcmp(&output.preferences,&preferences,sizeof(preferences))) return;
  output.generation=tab.generation;
  output.focus_acknowledgement=tab.focus_sequence; output.keyboard_element=keyboard_element;
  output.frame=tab.frame; output.navigation=tab.navigation;
  output.pending=tab.browser->NavigationPending();
  output.status=tab.browser->NavigationStatus();
  output.back=tab.browser->CanGoBack(); output.forward=tab.browser->CanGoForward();
  std::snprintf(output.url,sizeof(output.url),"%s",url.c_str());
  std::snprintf(output.opened_url,sizeof(output.opened_url),"%s",tab.opened_url.c_str());
  tab.opened_url.clear();
  output.preferences=preferences;
  std::memcpy(output.pixels,tab.pixels.data(),tab.pixels.size()*sizeof(uint32_t));
  const auto& snapshot=tab.browser->Snapshot();
  output.scroll_acknowledgement=snapshot.acknowledgement;
  if (renderer_runtime_sandboxed()) {
    if (tab.serialized_scene!=snapshot.encoded) {
      tab.serialized_scene=snapshot.encoded;
      if (snapshot.scene) std::printf("COMPOSITOR: wire encoded bytes=%zu pictures=%zu\n",
          tab.serialized_scene ? tab.serialized_scene->size() : 0,snapshot.scene->pictures.size());
    }
    output.scroll_bytes=tab.serialized_scene ? static_cast<uint32_t>(tab.serialized_scene->size()) : 0;
    // The fixed mailbox is part of the application mapping; its address is never supplied by a publication.
    auto* destination=reinterpret_cast<uint8_t*>(channel+1)+kSlots*size_t(channel->width)*channel->height*sizeof(uint32_t);
    if (output.scroll_bytes) std::memcpy(destination,tab.serialized_scene->data(),output.scroll_bytes);
    __atomic_store_n(&channel->scroll_owner,index+1,__ATOMIC_RELEASE);
  }
  tab.published_scene=snapshot.scene;
  output.scroll_scene=renderer_runtime_sandboxed() ? nullptr : tab.published_scene.get();
  __atomic_store_n(&output.ready,1,__ATOMIC_RELEASE);
}
}
// The probe is linked only when the CPL3 boundary test wrapper references it.
extern "C" __attribute__((noinline)) void renderer_scroll_wire_probe(void) {
  unsigned index=0;
  while (index<kSlots && (!server[index].browser || !server[index].serialized_scene || server[index].serialized_scene->empty())) ++index;
  if (index==kSlots) for (;;) __asm__ volatile("pause");
  auto& output=channel->slots[index];
  const auto& source=*server[index].serialized_scene;
  auto* bytes=reinterpret_cast<uint8_t*>(channel+1)+kSlots*size_t(channel->width)*channel->height*sizeof(uint32_t);
  for (unsigned test=0;test<6;++test) {
    while (__atomic_load_n(&output.ready,__ATOMIC_ACQUIRE) || __atomic_load_n(&channel->scroll_owner,__ATOMIC_ACQUIRE)) __asm__ volatile("pause");
    std::memcpy(bytes,source.data(),source.size());
    output.scroll_bytes=static_cast<uint32_t>(source.size());
    if (test==0) output.scroll_bytes=kScrollWireCapacity+1;
    if (test==1) --output.scroll_bytes;
    if (test==2) bytes[4]=0xff;
    if (test==3) { bytes[28]=0; bytes[29]=0; bytes[30]=0xc0; bytes[31]=0x7f; }
    if (test==4) bytes[source.size()-1]^=1;
    output.scroll_scene=reinterpret_cast<const ScrollScene*>(uintptr_t(0xffff800000001000));
    output.pixels=reinterpret_cast<uint32_t*>(uintptr_t(0xffff800000001000));
    ++output.frame;
    __atomic_store_n(&channel->scroll_owner,index+1,__ATOMIC_RELEASE);
    __atomic_store_n(&output.ready,1,__ATOMIC_RELEASE);
  }
  // The BSP must accept the final valid cache and scroll while this user thread remains busy.
  for (;;) __asm__ volatile("pause");
}
class RemoteFrame::State {
 public:
  State(SkCanvas& target, base::RepeatingClosure presented,
        base::RepeatingCallback<void(std::string)> opened)
      : canvas(target), present(std::move(presented)), open(std::move(opened)) {}
  SkCanvas& canvas;
  base::RepeatingClosure present;
  base::RepeatingCallback<void(std::string)> open;
  unsigned slot=kSlots;
  uint64_t generation=0, frame=0, navigation=0, focus_sequence=0;
  struct DeferredInput { unsigned operation; PointerSample pointer; KeySample key; bool motion=false; uint64_t sequence=0, element=0, focus=0; };
  uint64_t scroll_sequence=0, focus_acknowledgement=0, keyboard_element=0;
  bool pointer_drag=false, pointer_outside=true;
  ScrollCompositor compositor;
  void InvalidateScroll() { compositor.Clear(); keyboard_element=0; pointer_drag=false; }
  void CompositeScroll() { compositor.Draw(canvas,viewport_width,viewport_height); present.Run(); }
  unsigned last_buttons=0;
  std::deque<DeferredInput> inputs;
  size_t maximum_deferred=0, overflow=0;
  void Flush() {
    while (!inputs.empty() && InputCapacity()) {
      Command command{}; command.operation=inputs.front().operation; command.slot=slot;
      command.generation=generation; command.navigation=navigation; command.pointer=inputs.front().pointer; command.key=inputs.front().key; command.scroll_sequence=inputs.front().sequence; command.scroll_element=inputs.front().element; command.focus_sequence=inputs.front().focus;
      CHECK(Send(command)); inputs.pop_front();
    }
  }
  void Enqueue(DeferredInput input) {
    if (input.operation==::uefi_browser::Keyboard ||
        (input.operation==::uefi_browser::Pointer && input.pointer.buttons!=last_buttons))
      input.focus=++focus_sequence;
    if (input.operation==::uefi_browser::Pointer) {
      input.motion=input.pointer.buttons==last_buttons && !input.pointer.buttons && !input.pointer.wheel;
      last_buttons=input.pointer.buttons;
    }
    if (input.motion && !inputs.empty() && inputs.back().motion) inputs.back()=input;
    else if (inputs.size()<8192) inputs.push_back(input);
    else { ++overflow; return; }
    maximum_deferred=std::max(maximum_deferred,inputs.size());
    Flush();
  }
  bool Reflowed() {
    auto states=compositor.TakeReflowStates();
    for (const auto& state:states) {
      PointerSample pointer{}; pointer.x=state.hover; pointer.y=state.press; pointer.buttons=state.horizontal;
      Enqueue({Gutter,pointer,{},false,++scroll_sequence,state.element});
    }
    return !states.empty();
  }
  void Scrolled(uint64_t element,ScrollPosition position,const char* source) {
    PointerSample pointer{}; pointer.x=position.x; pointer.y=position.y;
    Enqueue({Scroll,pointer,{},false,++scroll_sequence,element});
    CompositeScroll();
    std::printf("COMPOSITOR: async %s sequence=%llu position=%d element=%llu x=%d\n",
        source,(unsigned long long)scroll_sequence,position.y,(unsigned long long)element,position.x);
  }
  bool pending=true, back=false, forward=false, recovered=false, visible=true;
  Status status=0;
  std::string url;
  bool Submit(unsigned operation) {
    Command command{}; command.operation=operation; command.slot=slot; command.generation=generation;
    bool navigation_command=operation==Back || operation==Forward || operation==::uefi_browser::Reload;
    if (navigation_command) command.navigation=navigation+1;
    if (!Send(command)) return false;
    if (navigation_command) { InvalidateScroll(); ++navigation; inputs.clear(); last_buttons=0; pending=true; }
    return true;
  }
};
RemoteFrame::RemoteFrame(SkCanvas& canvas, gfx::Size,
    base::RepeatingClosure present, base::RepeatingCallback<void(std::string)> open)
    : state_(std::make_unique<State>(canvas,std::move(present),std::move(open))) {
  if (!Capacity()) { state_->status=ERROR(6); return; }
  for (unsigned index=0; index<kSlots; ++index) {
    if (clients[index] || !__atomic_load_n(&channel->slots[index].closed,__ATOMIC_ACQUIRE)) continue;
    state_->slot=index; state_->generation=next_generation++;
    if (renderer_runtime_sandboxed()) channel->slots[index].pixels=trusted_pixels[index];
    else if (!channel->slots[index].pixels) channel->slots[index].pixels=new uint32_t[viewport_width*viewport_height]{};
    clients[index]=this;
    channel->slots[index].ready=0;
    __atomic_store_n(&channel->slots[index].closed,0,__ATOMIC_RELEASE);
    CHECK(state_->Submit(Create));
    return;
  }
  state_->status=ERROR(9);
}
RemoteFrame::~RemoteFrame() {
  std::printf("RENDERER: deferred input maximum=%zu overflow=%zu\n",state_->maximum_deferred,state_->overflow);
  if (state_->slot<kSlots) {
    clients[state_->slot]=nullptr;
    /* Closing never waits for JavaScript; the renderer acknowledges lifetime completion. */
    __atomic_store_n(&channel->slots[state_->slot].closed,2,__ATOMIC_RELEASE);
  }
}
Status RemoteFrame::Navigate(const char* url) {
  if (!renderer_runtime_available()) return ERROR(6);
  if (state_->slot==kSlots || std::strlen(url)>=4096) return ERROR(9);
  Command command{}; command.operation=::uefi_browser::Navigate; command.slot=state_->slot;
  command.generation=state_->generation; command.navigation=state_->navigation+1; std::strcpy(command.url,url);
  if (!Send(command)) return ERROR(6);
  state_->recovered=false; state_->InvalidateScroll();
  ++state_->navigation; state_->inputs.clear(); state_->last_buttons=0; state_->pending=true; state_->url=url; return 0;
}
bool RemoteFrame::NavigationPending() const { return state_->pending; }
Status RemoteFrame::NavigationStatus() const { return state_->status; }
std::string RemoteFrame::CurrentUrl() const { return state_->url; }
bool RemoteFrame::CanGoBack() const { return state_->back; }
bool RemoteFrame::CanGoForward() const { return state_->forward; }
Status RemoteFrame::GoBack() { return state_->Submit(Back) ? 0 : ERROR(6); }
Status RemoteFrame::Inspect(RemoteFrame& target) {
  if (state_->slot >= kSlots || target.state_->slot >= kSlots || this == &target) return ERROR(2);
  Command command{};
  command.operation = ::uefi_browser::Inspect;
  command.slot = state_->slot; command.generation = state_->generation;
  command.target_slot = target.state_->slot; command.target_generation = target.state_->generation;
  return Send(command) ? 0 : ERROR(6);
}
Status RemoteFrame::GoForward() { return state_->Submit(Forward) ? 0 : ERROR(6); }
Status RemoteFrame::Reload() { return state_->recovered ? Navigate(state_->url.c_str()) : state_->Submit(::uefi_browser::Reload) ? 0 : ERROR(6); }
bool RemoteFrame::CanAcceptInput() const { return state_->inputs.size()<8192; }
void RemoteFrame::Pointer(const PointerSample& sample) {
  if (!renderer_runtime_available() || state_->recovered) return;
  state_->pointer_outside=false;
  // A reflow can enqueue several gutter states before the matching pointer or scroll command.
  bool eligible=state_->compositor.Ready() && !state_->pending && state_->inputs.size()<8176;
  bool appearance_changed=eligible && state_->compositor.Hover(sample.x,sample.y);
  if (eligible) state_->Reflowed();
  if (eligible && (state_->pointer_drag || ((sample.buttons&1) && !(state_->last_buttons&1)))) {
    uint64_t element=0; ScrollPosition position;
    if (state_->compositor.Pointer(sample.x,sample.y,sample.buttons,&element,&position)) {
      state_->Reflowed();
      state_->pointer_drag=(sample.buttons&1)!=0;
      if (element) state_->Scrolled(element,position,"drag");
      else state_->CompositeScroll();
      return;
    }
  }
  if (eligible && sample.wheel && !sample.buttons && state_->compositor.WheelAllowed(sample.x,sample.y)) {
    ScrollPosition position;
    uint64_t element=state_->compositor.Scroll(sample.x,sample.y,-sample.wheel*80,&position);
    if (element) {
      auto pointer=sample;
      pointer.wheel_consumed=true;
      if (!state_->compositor.WheelListeners()) pointer.wheel=0;
      state_->Enqueue({::uefi_browser::Pointer,pointer,{}});
      state_->Scrolled(element,position,"scroll");
      return;
    }
  }
  if (appearance_changed) state_->CompositeScroll();
  state_->Enqueue({::uefi_browser::Pointer,sample,{}});
}
void RemoteFrame::PointerLeave() {
  if (state_->pointer_outside || state_->recovered) return;
  state_->pointer_outside=true;
  if (state_->compositor.Ready() && state_->compositor.Hover(-1,-1)) { state_->Reflowed(); state_->CompositeScroll(); }
  PointerSample sample{}; sample.x=sample.y=-1;
  state_->Enqueue({::uefi_browser::Pointer,sample,{}});
}
void RemoteFrame::Keyboard(const KeySample& sample) {
  if (!renderer_runtime_available() || state_->recovered) return;
  bool modifiers=(sample.shift&0x80000000u)!=0;
  bool shift=modifiers && (sample.shift&3);
  if (sample.input_method == InputMethod::Latin && sample.composition_action == CompositionAction::None && !(modifiers && (sample.shift&252)) && !(shift && sample.key.scan) && state_->focus_sequence==state_->focus_acknowledgement &&
      !state_->pointer_drag && !state_->pending && state_->compositor.Ready() && state_->inputs.size()<8190) {
    ScrollPosition position;
    uint64_t element=state_->compositor.Keyboard(state_->keyboard_element,sample.key.scan,
        !sample.key.scan && sample.key.character==' ',shift,&position);
    if (element) { state_->Scrolled(element,position,"key"); return; }
  }
  state_->Enqueue({::uefi_browser::Keyboard,{},sample});
}
void RemoteFrame::SetVisible(bool visible) {
  state_->visible=visible;
  if (!visible) { state_->compositor.ReleaseRasterTiles(); state_->compositor.CancelDrag(); state_->compositor.Hover(-1,-1); state_->pointer_drag=false; state_->Reflowed(); }
  Command command{}; command.operation=Visible; command.slot=state_->slot;
  command.generation=state_->generation; command.visible=visible; Send(command);
}
void RemoteFrame::Poll() {
  if (state_->slot>=kSlots) return;
  state_->Flush();
  if (state_->visible && !state_->pending && state_->pointer_drag && state_->inputs.size()<8190) {
    uint64_t element=0; ScrollPosition position;
    bool moved=state_->compositor.Tick(base::TimeTicks::Now().since_origin().InMilliseconds(),&element,&position);
    bool reflowed=state_->Reflowed();
    if (moved && element) state_->Scrolled(element,position,"repeat");
    else if (reflowed) state_->CompositeScroll();
  }
  auto& input=channel->slots[state_->slot];
  if (!__atomic_load_n(&input.ready,__ATOMIC_ACQUIRE)) return;
  auto release=[&] {
    __atomic_store_n(&input.ready,0,__ATOMIC_RELEASE);
    if (renderer_runtime_sandboxed()) __atomic_store_n(&channel->scroll_owner,0,__ATOMIC_RELEASE);
  };
  if (input.generation!=state_->generation || input.navigation<state_->navigation) { release(); return; }
  if (state_->recovered) {
    // The scroll mailbox is shared by all tabs. A recovery placeholder must
    // release its publication without replacing the URL needed for Reload.
    release();
    std::printf("RENDERER: recovery publication discarded slot=%u\n", state_->slot);
    return;
  }
  bool changed=state_->frame!=input.frame || state_->pending!=input.pending || state_->url!=std::string(input.url,strnlen(input.url,sizeof(input.url)));
  state_->pending=input.pending; state_->status=input.status;
  state_->focus_acknowledgement=input.focus_acknowledgement; state_->keyboard_element=input.keyboard_element;
  state_->back=input.back; state_->forward=input.forward;
  state_->url.assign(input.url,strnlen(input.url,sizeof(input.url)));
  BrowserPreferences received=input.preferences;
  if (received.version==1 && received.dark<=1 &&
      (received.font_size==14 || received.font_size==16 || received.font_size==20) &&
      received.javascript<=1 && received.images<=1 && received.startup_settings<=1) preferences=received;
  std::string opened(input.opened_url,strnlen(input.opened_url,sizeof(input.opened_url)));
  if (state_->frame!=input.frame && input.scroll_acknowledgement>=state_->scroll_sequence) {
    if (renderer_runtime_sandboxed()) {
      const uint32_t length=__atomic_load_n(&input.scroll_bytes,__ATOMIC_RELAXED);
      std::shared_ptr<ScrollScene> decoded;
      if (length && length<=kScrollWireCapacity && ScrollWireMemoryAvailable(2*size_t(length)+16*1024*1024)) {
        // Copy once from the trusted fixed mapping. Validation and rendering never read shared bytes again.
        std::vector<uint8_t> owned(length);
        std::memcpy(owned.data(),trusted_scroll_bytes,length);
        decoded=DecodeScrollScene(owned.data(),owned.size());
      }
      if (!decoded || !state_->compositor.Install(*decoded,viewport_width,viewport_height)) {
        state_->InvalidateScroll();
        if (length) std::printf("COMPOSITOR: wire rejected bytes=%u\n",length);
      } else std::printf("COMPOSITOR: scene pictures=%zu transforms=%zu acknowledgement=%llu wire=%u cpl3=1\n",
          decoded->pictures.size(),decoded->transforms.size(),(unsigned long long)input.scroll_acknowledgement,length);
    } else if (!input.scroll_scene) state_->InvalidateScroll();
    if (!renderer_runtime_sandboxed() && input.scroll_scene && state_->compositor.Install(*input.scroll_scene,viewport_width,viewport_height))
      std::printf("COMPOSITOR: scene pictures=%zu transforms=%zu acknowledgement=%llu\n",
          input.scroll_scene->pictures.size(),input.scroll_scene->transforms.size(),(unsigned long long)input.scroll_acknowledgement);
    const bool gpu_scene = state_->canvas.recordingContext() && state_->compositor.Ready();
    if (!gpu_scene) state_->canvas.writePixels(SkImageInfo::Make(viewport_width,viewport_height,
        kBGRA_8888_SkColorType,kPremul_SkAlphaType),renderer_runtime_sandboxed() ? trusted_pixels[state_->slot] : input.pixels,viewport_width*4,0,0);
    if (state_->visible && state_->compositor.Ready() && (gpu_scene || state_->compositor.AppearanceChanged())) state_->compositor.Draw(state_->canvas,viewport_width,viewport_height);
    state_->frame=input.frame;
  }
  release();
  auto present=state_->present; auto open=state_->open;
  if (changed) present.Run();
  if (!opened.empty()) open.Run(std::move(opened));
}
void RemoteFrame::Recover() {
  state_->inputs.clear(); state_->last_buttons=0; state_->InvalidateScroll(); state_->scroll_sequence=0; state_->focus_sequence=state_->focus_acknowledgement=0;
  state_->generation=next_generation++; state_->frame=0; state_->navigation=0;
  state_->pending=false; state_->status=ERROR(21); state_->back=state_->forward=false;
  state_->recovered=true;
  auto& output=channel->slots[state_->slot];
  output.ready=0; output.closed=0;
  if (renderer_runtime_available()) { CHECK(state_->Submit(Create)); SetVisible(state_->visible); }
  state_->canvas.clear(SkColorSetRGB(246,247,249));
  state_->present.Run();
}
void TerminateRenderer() {
  if (channel) { termination_requested=true; std::puts("RENDERER: termination requested"); }
}
bool RemoteRendererConfigured() { return channel && !renderer_runtime_is_child(); }
static void RecoverRenderer() {
  if (renderer_runtime_sandboxed()) {
    /* A stopped renderer may have corrupted every byte of the publication channel. */
    std::memset(channel,0,sizeof(*channel));
    channel->width=viewport_width; channel->height=viewport_height;
    for (unsigned index=0;index<kSlots;++index) channel->slots[index].pixels=trusted_pixels[index];
  }
  channel->head=channel->tail=channel->ready=channel->stopping=0;
  for (auto& slot:channel->slots) { slot.ready=0; slot.closed=1; }
  Status status=renderer_runtime_start(channel);
  for (auto* client:clients) if (client) client->Recover();
  if (status) std::printf("RENDERER: restart failed status=%llx; use End renderer to retry\n",(unsigned long long)status);
  else std::puts("RENDERER: recovery launched; reload or navigate to restore discarded documents");
}
bool PollRemoteFrames() {
  if (!channel) return false;
  if (!renderer_runtime_available()) {
    if (termination_requested) { termination_requested=false; RecoverRenderer(); }
    return false;
  }
  if (termination_requested) {
    Status result=renderer_runtime_terminate();
    if (result!=ERROR(6)) termination_requested=false;
  }
  Status result=renderer_runtime_join();
  if (result!=ERROR(6)) {
    /* No old producer remains when generations and shared buffers are reused. */
    RecoverRenderer();
  }
  renderer_runtime_service();
  for (auto* client:clients) if (client) client->Poll();
  return false;
}
bool StartRenderer(SystemTable*, int width, int height) {
  if (width<=0 || height<=0 || width>4096 || height>4096) return false;
  viewport_width=width; viewport_height=height;
  if (renderer_runtime_sandboxed()) {
    if (!TestScrollWire()) { std::puts("COMPOSITOR: wire validation self-test failed"); return false; }
    std::puts("COMPOSITOR: wire validation self-test passed");
  }
  channel_bytes=sizeof(Channel)+kSlots*size_t(width)*height*sizeof(uint32_t)+kScrollWireCapacity;
  channel=renderer_runtime_sandboxed() ? static_cast<Channel*>(renderer_runtime_shared_allocate(channel_bytes)) : new Channel{};
  if (!channel) return false;
  channel->width=width; channel->height=height;
  if (renderer_runtime_sandboxed()) {
    auto* pixels=reinterpret_cast<uint32_t*>(channel+1);
    for (unsigned index=0;index<kSlots;++index)
      channel->slots[index].pixels=trusted_pixels[index]=pixels+index*size_t(width)*height;
    trusted_scroll_bytes=reinterpret_cast<uint8_t*>(pixels+kSlots*size_t(width)*height);
    renderer_runtime_application_size(channel_bytes);
  }
  for (auto& slot:channel->slots) slot.closed=1;
  Status result=renderer_runtime_start(channel);
  if (result) {
    ReleaseChannel();
    std::printf("RENDERER: separate address space unavailable status=%llx; %s\n",(unsigned long long)result,
        renderer_runtime_sandboxed() ? "privileged fallback disabled" : "local fallback");
    return false;
  }
  std::puts(renderer_runtime_sandboxed() ?
      "RENDERER: AP process launched with protected page tables and CPL3 entry" :
      "RENDERER: AP process launched with private image data, stack, TLS and page tables (CPL0)");
  return true;
}
void RequestRendererStop() {
  if (!channel) return;
  uint32_t expected = 0;
  // Repeated requests must preserve an acknowledgement already published by the AP.
  __atomic_compare_exchange_n(&channel->stopping, &expected, 1, false,
                              __ATOMIC_RELEASE, __ATOMIC_RELAXED);
}
void StopRenderer() {
  if (!channel) return;
  if (!renderer_runtime_available()) {
    ReleaseChannel(); return;
  }
  RequestRendererStop();
  Status result;
  const auto started = base::TimeTicks::Now();
  auto deadline = started + base::Seconds(2);
  bool acknowledged = false, forced = false;
  do {
    // Acknowledgement permits bounded teardown, not an untrusted rolling lease.
    if (!acknowledged && !forced &&
        __atomic_load_n(&channel->stopping, __ATOMIC_ACQUIRE) == 2) {
      acknowledged = true;
      deadline = started + base::Seconds(15);
      std::puts("RENDERER: shutdown acknowledged; bounded teardown started");
    }
    if (!forced && base::TimeTicks::Now() >= deadline &&
        renderer_runtime_terminate() == 0) {
      forced = true;
      std::printf("RENDERER: shutdown deadline exceeded acknowledged=%u; forcing termination\n",
                  unsigned(acknowledged));
    }
    result=renderer_runtime_join();
    if (result==ERROR(6)) { renderer_runtime_service(); resource_async_process(); }
  } while (result==ERROR(6));
  std::printf("RENDERER: shutdown completed acknowledged=%u forced=%u elapsed_us=%lld\n",
              unsigned(acknowledged), unsigned(forced),
              static_cast<long long>((base::TimeTicks::Now()-started).InMicroseconds()));
  ReleaseChannel();
  std::puts("RENDERER: joined and released private address space");
}
static void PollMemoryPressure() {
  static base::TimeTicks sampled, notified;
  static base::MemoryPressureLevel previous=base::MEMORY_PRESSURE_LEVEL_NONE;
  static bool notifying=false;
  const auto now=base::TimeTicks::Now();
  if (notifying || (!sampled.is_null() && now-sampled<base::Milliseconds(250))) return;
  sampled=now;
  const uint64_t charged=renderer_runtime_memory_statistic(0);
  const uint64_t quota=renderer_runtime_memory_statistic(1);
  const size_t used=runtime_memory_used(), limit=runtime_memory_limit();
  if (!quota || charged==UINT64_MAX || quota==UINT64_MAX || !limit) return;
  auto level=base::MEMORY_PRESSURE_LEVEL_NONE;
  if (charged>=quota-quota/5 || used>=limit-limit/5)
    level=base::MEMORY_PRESSURE_LEVEL_CRITICAL;
  else if (charged>=quota/20*13 || used>=limit/20*13)
    level=base::MEMORY_PRESSURE_LEVEL_MODERATE;
  if (level==base::MEMORY_PRESSURE_LEVEL_NONE && previous==level) return;
  if (level<=previous && !notified.is_null() && now-notified<base::Seconds(5)) return;
  /* GC can enter nested event loops; notifications cannot recursively trigger another GC. */
  notifying=true;
  previous=level; notified=now;
  base::MemoryPressureListenerRegistry::NotifyMemoryPressure(level);
  if (level==base::MEMORY_PRESSURE_LEVEL_CRITICAL) {
    blink::WebCache::Clear();
    SkGraphics::PurgeAllCaches();
  }
  blink::MemoryPressureNotificationToAllIsolates(
      level==base::MEMORY_PRESSURE_LEVEL_CRITICAL ? v8::MemoryPressureLevel::kCritical :
      level==base::MEMORY_PRESSURE_LEVEL_MODERATE ? v8::MemoryPressureLevel::kModerate :
      v8::MemoryPressureLevel::kNone);
  const uint64_t after=renderer_runtime_memory_statistic(0);
  std::printf("MEMORY PRESSURE: level=%d charged_before=%llu charged_after=%llu quota=%llu heap_before=%zu heap_after=%zu duration_us=%lld\n",
              static_cast<int>(level),static_cast<unsigned long long>(charged),
              static_cast<unsigned long long>(after),static_cast<unsigned long long>(quota),
              used,runtime_memory_used(),static_cast<long long>((base::TimeTicks::Now()-now).InMicroseconds()));
  notifying=false;
}
bool PollRenderer(void*) {
  if (!server_loop) return false;
  if (__atomic_load_n(&channel->stopping, __ATOMIC_ACQUIRE)) {
    __atomic_store_n(&channel->stopping, 2, __ATOMIC_RELEASE);
    server_loop->Quit();
    return false;
  }
  uint32_t tail=__atomic_load_n(&channel->tail,__ATOMIC_RELAXED);
  for (unsigned count=0; count<8 && tail!=__atomic_load_n(&channel->head,__ATOMIC_ACQUIRE); ++count) {
    Command command=channel->commands[tail%kCommands];
    if (command.slot<kSlots && (command.operation==Pointer || command.operation==Keyboard ||
        command.operation==Scroll || command.operation==Gutter)) {
      auto& target=server[command.slot];
      // Leave the command in the bounded ring until the scheduler drains its input queue.
      if (target.browser && target.generation==command.generation && !target.browser->CanAcceptInput()) break;
    }
    __atomic_store_n(&channel->tail,++tail,__ATOMIC_RELEASE);
    if (command.slot>=kSlots) continue;
    auto& tab=server[command.slot];
    if (command.operation==Create) {
      tab.generation=command.generation; tab.frame=0; tab.navigation=0; tab.focus_sequence=0;
      tab.pixels.resize(channel->width*channel->height);
      tab.surface=SkSurfaces::WrapPixels(SkImageInfo::Make(channel->width,channel->height,
          kBGRA_8888_SkColorType,kPremul_SkAlphaType),tab.pixels.data(),channel->width*4);
      CHECK(tab.surface); tab.surface->getCanvas()->clear(SK_ColorWHITE);
      tab.browser=std::make_unique<BrowserFrame>(server_table,*server_scheduler,*tab.surface->getCanvas(),
          gfx::Size(channel->width,channel->height),base::BindRepeating(&RendererTab::Presented,base::Unretained(&tab)),
          base::BindRepeating(&RendererTab::Open,base::Unretained(&tab)));
    }
    if (!tab.browser || tab.generation!=command.generation) continue;
    if (command.operation==Navigate || command.operation==Back || command.operation==Forward || command.operation==Reload)
      tab.navigation=command.navigation;
    switch (command.operation) {
      case Navigate: tab.browser->Navigate(command.url); break;
      case Inspect:
        if (command.target_slot < kSlots && command.target_slot != command.slot) {
          auto& target = server[command.target_slot];
          if (target.browser && target.generation == command.target_generation) {
            Status result = tab.browser->Inspect(*target.browser);
            std::printf("DEVTOOLS: attach frontend=%u target=%u status=%llu\n",
                command.slot, command.target_slot, static_cast<unsigned long long>(result));
          }
        }
        break;
      case Back: tab.browser->GoBack(); break;
      case Forward: tab.browser->GoForward(); break;
      case Reload: tab.browser->Reload(); break;
      case Visible: tab.browser->SetVisible(command.visible); break;
      case Pointer: tab.browser->Pointer(command.pointer); break;
      case Keyboard: tab.browser->Keyboard(command.key); break;
      case Gutter: tab.browser->ScrollbarState(command.scroll_element,command.pointer.buttons!=0,command.pointer.x,command.pointer.y,command.scroll_sequence); break;
      case Scroll: tab.browser->ScrollTo(command.scroll_element,command.pointer.x,command.pointer.y,command.scroll_sequence); break;
      default: break;
    }
    tab.focus_sequence=std::max(tab.focus_sequence,command.focus_sequence);
  }
  for (unsigned index=0; index<kSlots; ++index) {
    if (__atomic_load_n(&channel->slots[index].closed,__ATOMIC_ACQUIRE)==2) {
      server[index].browser.reset(); server[index].published_scene.reset(); server[index].serialized_scene.reset(); server[index].surface.reset(); server[index].pixels.clear();
      __atomic_store_n(&channel->slots[index].ready,0,__ATOMIC_RELEASE);
      if (renderer_runtime_sandboxed() && __atomic_load_n(&channel->scroll_owner,__ATOMIC_ACQUIRE)==index+1)
        __atomic_store_n(&channel->scroll_owner,0,__ATOMIC_RELEASE);
      __atomic_store_n(&channel->slots[index].closed,1,__ATOMIC_RELEASE);
    } else Publish(index);
  }
  if (__atomic_load_n(&channel->stopping,__ATOMIC_ACQUIRE)) server_loop->Quit();
  else PollMemoryPressure();
  return false;
}
int RunRenderer(SystemTable* table, blink::scheduler::WebThreadScheduler& scheduler) {
  channel=static_cast<Channel*>(renderer_runtime_channel());
  server_table=table; server_scheduler=&scheduler;
  base::RunLoop loop; server_loop=&loop;
  uintptr_t root=0, thread; uint16_t selector;
  __asm__ volatile("mov %%cs,%0; mov %%fs:0,%1" : "=r"(selector),"=r"(thread));
  if (!(selector&3)) __asm__ volatile("mov %%cr3,%0" : "=r"(root));
  std::printf("RENDERER: Blink/V8 event loop cr3=%llx tls=%llx cpl=%u\n",
      (unsigned long long)root,(unsigned long long)thread,selector&3);
  __atomic_store_n(&channel->ready,1,__ATOMIC_RELEASE);
  loop.Run();
  if (__atomic_load_n(&channel->stopping, __ATOMIC_ACQUIRE))
    __atomic_store_n(&channel->stopping, 2, __ATOMIC_RELEASE);
  // Teardown can enter nested loops; they must not publish partially destroyed tabs.
  server_loop=nullptr;
  for (auto& tab:server) { tab.browser.reset(); tab.published_scene.reset(); tab.serialized_scene.reset(); tab.surface.reset(); tab.pixels.clear(); }
  std::puts("RENDERER: browsing contexts released");
  return 0;
}
}

#include "third_party/blink/renderer/core/frame/web_local_frame_impl.h"
#include "third_party/blink/renderer/core/frame/local_frame.h"
#include "third_party/blink/renderer/core/frame/local_frame_view.h"
#include "third_party/blink/renderer/core/scroll/scrollable_area.h"
// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
#include "blink/browser_frame.h"
#include "blink/devtools.h"
#include "blink/devtools_resources.h"
#include "blink/remote_frame.h"
#include "blink/scroll_scene.h"
#include "renderer_runtime.h"
#include "blink/browser_identity.h"
#include "blink/browser_broker.h"
#include "blink/web_storage.h"
#include "blink/text_composer.h"
#include "blink/ime_popup.h"
#include "third_party/skia/include/core/SkCanvas.h"
#include "third_party/blink/public/web/web_input_method_controller.h"
#include "process/renderer_protocol.h"
#include "third_party/blink/public/platform/task_type.h"
#include <deque>
#include "blink/browser_settings.h"
#include <cstdio>
#include <utility>
#include "base/check.h"
#include "base/notreached.h"
#include "services/network/public/mojom/content_security_policy.mojom.h"
#include "services/network/public/cpp/web_sandbox_flags.h"
#include "third_party/blink/public/mojom/frame/policy_container.mojom.h"
#include "third_party/blink/public/mojom/input/touch_event.mojom.h"
#include "third_party/blink/public/mojom/frame/intrinsic_sizing_info.mojom.h"
#include "ui/base/ime/mojom/text_input_state.mojom.h"
#include "mojo/public/cpp/bindings/associated_receiver.h"
#include "third_party/blink/public/mojom/page/widget.mojom.h"
#include "third_party/blink/public/mojom/widget/platform_widget.mojom.h"
#include "base/unguessable_token.h"
#include "blink/frame_loop.h"
#include "blink/media_player.h"
#include "blink/resource_loader.h"
#include "blink/workers.h"
#include "components/viz/common/surfaces/frame_sink_id.h"
#include "mojo/public/cpp/bindings/pending_associated_receiver.h"
#include "mojo/public/cpp/bindings/pending_associated_remote.h"
#include "mojo/public/cpp/bindings/pending_remote.h"
#include "third_party/blink/public/common/page/color_provider_color_maps.h"
#include "third_party/blink/public/mojom/page/prerender_page_param.mojom.h"
#include "third_party/blink/public/platform/scheduler/web_agent_group_scheduler.h"
#include "third_party/blink/public/platform/scheduler/web_thread_scheduler.h"
#include "third_party/blink/public/web/web_frame_widget.h"
#include "third_party/blink/public/web/web_console_message.h"
#include "third_party/blink/public/web/web_local_frame_client.h"
#include "third_party/blink/public/web/web_navigation_control.h"
#include "third_party/blink/public/web/web_non_composited_widget_client.h"
#include "third_party/blink/public/platform/web_policy_container.h"
#include "third_party/blink/public/web/web_settings.h"
#include "third_party/blink/public/web/web_view.h"
#include "third_party/blink/public/web/web_view_client.h"

#include "base/functional/bind.h"
#include "base/memory/weak_ptr.h"
#include "base/location.h"
#include "third_party/blink/public/common/input/web_coalesced_input_event.h"
#include "third_party/blink/public/common/input/web_mouse_event.h"
#include "third_party/blink/public/common/input/web_mouse_wheel_event.h"
#include "third_party/blink/public/common/input/web_gesture_event.h"
#include "third_party/blink/public/common/input/web_keyboard_event.h"
#include "third_party/blink/public/web/web_navigation_params.h"
#include "ui/events/keycodes/keyboard_codes.h"
#include "ui/events/keycodes/dom/dom_key.h"
#include "ui/events/keycodes/keyboard_code_conversion.h"
#include "url/gurl.h"
#include "third_party/blink/renderer/core/exported/web_view_impl.h"
#include "third_party/blink/renderer/core/frame/web_local_frame_impl.h"
#include "third_party/blink/renderer/core/frame/local_frame.h"
#include "third_party/blink/renderer/core/frame/local_frame_view.h"
#include "third_party/blink/renderer/core/input/event_handler.h"
#include "third_party/blink/renderer/core/layout/hit_test_location.h"
#include "third_party/blink/renderer/core/layout/hit_test_result.h"
#include "third_party/blink/public/mojom/input/scroll_direction.mojom-blink.h"

#include "third_party/blink/public/web/web_element.h"
#include "third_party/blink/public/common/scheduler/task_attribution_id.h"
#include "third_party/blink/public/web/web_document.h"
#include "third_party/blink/public/mojom/commit_result/commit_result.mojom.h"
#include "third_party/blink/renderer/core/frame/local_dom_window.h"
#include "third_party/blink/renderer/core/frame/policy_container.h"
#include "third_party/blink/renderer/core/html/html_frame_owner_element.h"
#include "third_party/blink/renderer/core/html_names.h"
#include "third_party/blink/renderer/platform/loader/fetch/policy_container_utils.h"
#include "third_party/blink/renderer/platform/weborigin/security_origin.h"
#include "third_party/blink/renderer/core/dom/document.h"
namespace uefi_browser {
namespace {
// Local child frames share the renderer task sequence and the root software widget.
class ChildFrame final : public blink::WebLocalFrameClient,
                         public blink::mojom::PolicyContainerHost {
 public:
  static blink::WebLocalFrame* Create(blink::WebLocalFrame* parent,SystemTable* table,
      base::RepeatingClosure changed,blink::mojom::TreeScopeType scope,
      blink::WebPolicyContainerBindParams bind,FinishChildFrameCreationFn finish) {
    auto* root=static_cast<blink::WebLocalFrameImpl*>(parent)->GetFrame();
    while (root->Tree().Parent()) root=blink::To<blink::LocalFrame>(root->Tree().Parent());
    unsigned count=0;
    for (auto* frame=static_cast<blink::Frame*>(root);frame;frame=frame->Tree().TraverseNext(root))
      if (++count>=16) return nullptr;
    auto* client=new ChildFrame(table,std::move(changed));
    client->policy_receiver_.Bind(std::move(bind.receiver));
    client->frame_=parent->CreateLocalChild(scope,client,nullptr,blink::LocalFrameToken());
    client->broker_.SetFrameToken(client->frame_->GetLocalFrameToken());
    finish(client->frame_,blink::DocumentToken(),blink::InitiatorStateToken(),client->broker_.Bind(),
        std::make_unique<base::UnguessableToken>(base::UnguessableToken::Create()));
    return client->frame_;
  }
  scoped_refptr<blink::WebWorkerFetchContext> CreateWorkerFetchContext(
      blink::WebDedicatedWorkerHostFactoryClient*) override {
    return uefi_browser::CreateWorkerFetchContext(table_, frame_);
  }
  std::unique_ptr<blink::WebMediaPlayer> CreateMediaPlayer(
      const blink::WebMediaPlayerSource&, blink::WebMediaPlayerClient* client,
      blink::MediaInspectorContext*, blink::WebMediaPlayerEncryptedMediaClient*,
      blink::WebContentDecryptionModule*, const blink::WebString&,
      const cc::LayerTreeSettings*, scoped_refptr<base::TaskRunner>) override {
    return uefi_browser::CreateMediaPlayer(frame_, client);
  }
  void BindToFrame(blink::WebNavigationControl* navigation) override { navigation_=navigation; }
  void FrameDetached(blink::DetachReason reason) override {
    weak_.InvalidateWeakPtrs(); request_.reset(); frame_=nullptr;
    if (navigation_) std::exchange(navigation_,nullptr)->Close(reason);
    delete this;
  }
  blink::WebLocalFrame* CreateChildFrame(blink::mojom::TreeScopeType scope,
      const blink::WebString&,const blink::WebString&,const blink::FramePolicy&,
      const blink::WebFrameOwnerProperties&,blink::FrameOwnerElementType,
      blink::WebPolicyContainerBindParams bind,ukm::SourceId,FinishChildFrameCreationFn finish) override {
    return Create(frame_,table_,changed_,scope,std::move(bind),finish);
  }
  void BeginNavigation(std::unique_ptr<blink::WebNavigationInfo> info) override {
    if ((info->url_request.HttpMethod().Utf8()!="GET" && info->url_request.HttpMethod().Utf8()!="POST") || info->navigation_policy!=blink::kWebNavigationPolicyCurrentTab) return;
    if (!navigation_->WillStartNavigation(*info)) return;
    base::SingleThreadTaskRunner::GetCurrentDefault()->PostTask(FROM_HERE,
        base::BindOnce(&ChildFrame::Navigate,weak_.GetWeakPtr(),std::move(info)));
  }
  void DidFinishLoad() override { changed_.Run(); std::puts("BLINK: child document load finished"); }
  void DidStopLoading() override { changed_.Run(); }
  void DidChangeScrollOffset(blink::mojom::ScrollType) override { changed_.Run(); }
  void DidAddMessageToConsole(const blink::WebConsoleMessage& message,
      const blink::WebString&,unsigned line,const blink::WebString&) override {
    std::printf("BLINK: child console level=%d line=%u %.2048s\n",static_cast<int>(message.level),line,message.text.Utf8().c_str());
  }
  blink::WebString UserAgentOverride() override {
    return blink::WebString::FromUtf8(BrowserUserAgent(GoogleCompatibility(GURL(frame_->GetDocument().Url().GetString().Utf8()))));
  }
  std::optional<blink::UserAgentMetadata> UserAgentMetadataOverride() override {
    return BrowserUserAgentMetadata(GoogleCompatibility(GURL(frame_->GetDocument().Url().GetString().Utf8())));
  }
  void SetReferrerPolicy(network::mojom::ReferrerPolicy,const blink::InitiatorStateToken&) override {}
  void AddContentSecurityPolicies(std::vector<network::mojom::ContentSecurityPolicyPtr>,const blink::InitiatorStateToken&) override {}
 private:
  ChildFrame(SystemTable* table,base::RepeatingClosure changed):table_(table),changed_(std::move(changed)) {}
  void Navigate(std::unique_ptr<blink::WebNavigationInfo> info) {
    if (!frame_ || !navigation_) return;
    GURL url(info->url_request.Url().GetString().Utf8());
    const bool local=url.spec()=="about:srcdoc" || url.spec()=="about:blank";
    if (!local && (!url.SchemeIsHTTPOrHTTPS() || url.has_username() || url.has_password())) return;
    request_.reset();
    auto* core=static_cast<blink::WebLocalFrameImpl*>(frame_)->GetFrame();
    auto* owner=blink::DynamicTo<blink::HTMLFrameOwnerElement>(core->Owner());
    if (!owner) return;
    auto* parent=blink::To<blink::LocalFrame>(core->Tree().Parent());
    auto parameters=blink::WebNavigationParams::CreateFromInfo(*info);
    parameters->frame_policy=owner->GetFramePolicy();
    blink::WebPolicyContainerPolicies policies;
    if (local) policies=blink::ToWebPolicyContainerPolicies(parent->DomWindow()->GetPolicyContainer()->GetPolicies());
    policies.sandbox_flags|=owner->GetFramePolicy().sandbox_flags |
        parent->DomWindow()->GetPolicyContainer()->GetPolicies().sandbox_flags;
    if ((policies.sandbox_flags & network::mojom::WebSandboxFlags::kOrigin)!=network::mojom::WebSandboxFlags::kNone)
      parameters->origin_to_commit=blink::SecurityOrigin::Create(info->url_request.Url())->DeriveNewOpaqueOrigin();
    policy_receiver_.reset();
    mojo::PendingAssociatedRemote<blink::mojom::PolicyContainerHost> remote;
    auto receiver=remote.InitWithNewEndpointAndPassReceiver(); remote.EnableUnassociatedUsage();
    policy_receiver_.Bind(std::move(receiver));
    auto policy=std::make_unique<blink::WebPolicyContainer>(std::move(policies),std::move(remote));
    if (local) {
      parameters->policy_container=std::move(policy);
      parameters->navigation_timings.navigation_start=base::TimeTicks::Now();
      std::string text=url.spec()=="about:srcdoc" ? owner->getAttribute(blink::html_names::kSrcdocAttr).Utf8() : std::string();
      blink::WebNavigationParams::FillStaticResponse(parameters.get(),blink::WebString::FromAscii("text/html"),
          blink::WebString::FromAscii("UTF-8"),base::span(text.data(),text.size()));
      navigation_->CommitNavigation(std::move(parameters),nullptr);
      return;
    }
    request_=LoadNavigation(table_,*navigation_,url.spec().c_str(),std::move(policy),info->frame_load_type,{},
        base::BindOnce(&ChildFrame::Finished,weak_.GetWeakPtr()),std::move(parameters),false);
  }
  void Finished(Status status) {
    std::printf("BLINK: child navigation status=%llx\n",static_cast<unsigned long long>(status)); changed_.Run();
  }
  SystemTable* table_;
  base::RepeatingClosure changed_;
  BrowserBroker broker_;
  blink::WebLocalFrame* frame_=nullptr;
  blink::WebNavigationControl* navigation_=nullptr;
  mojo::AssociatedReceiver<blink::mojom::PolicyContainerHost> policy_receiver_{this};
  std::unique_ptr<NavigationRequest> request_;
  base::WeakPtrFactory<ChildFrame> weak_{this};
};
}
class BrowserFrame::State final : public blink::WebLocalFrameClient,
                                  public blink::WebNonCompositedWidgetClient,
                                  public blink::mojom::FrameWidgetHost,
                                  public blink::mojom::WidgetHost,
                                  public blink::mojom::PolicyContainerHost {
 public:
  State(SystemTable* table, blink::scheduler::WebThreadScheduler& scheduler,
        SkCanvas& canvas, gfx::Size viewport, base::RepeatingClosure present,
        base::RepeatingCallback<void(std::string)> open_tab)
      : open_tab_(std::move(open_tab)), table_(table), agent_group_(scheduler.CreateWebAgentGroupScheduler()) {
    CHECK(agent_group_);
    CHECK(!viewport.IsEmpty());
    InstallResourceLoader(table_);
    auto colors = blink::ColorProviderColorMaps::CreateDefault();
    view_ = blink::WebView::Create(
        &view_client_, false, nullptr, std::nullopt,
        /*compositing_enabled=*/false, /*widgets_never_composited=*/true,
        nullptr, mojo::NullAssociatedReceiver(), *agent_group_, base::UnguessableToken::Create().ToString(),
        std::nullopt, base::UnguessableToken::Create(), &colors, -1, 0);
    CHECK(view_);
    view_->GetSettings()->SetJavaScriptEnabled(true);
    // The UEFI cookie bridge resolves document access on the renderer task sequence.
    view_->GetSettings()->SetCookieEnabled(true);
    view_->GetSettings()->SetLocalStorageEnabled(true);
    // Software input scrolling must not wait for main-thread animation frames.
    view_->GetSettings()->SetEnableScrollAnimator(false);
    view_->GetSettings()->SetImagesEnabled(true);
    view_->GetSettings()->SetLoadsImagesAutomatically(true);
    view_->GetSettings()->SetDownloadableBinaryFontsEnabled(true);
    view_->GetSettings()->SetDefaultFontSize(16);
    view_->GetSettings()->SetDefaultFixedFontSize(13);
    const auto family = blink::WebString::FromAscii("Noto Sans");
    view_->GetSettings()->SetStandardFontFamily(family);
    view_->GetSettings()->SetSansSerifFontFamily(family);
    view_->GetSettings()->SetSerifFontFamily(family);
    view_->GetSettings()->SetFixedFontFamily(family);
    view_->SetDefaultPageScaleLimits(1, 1);
    auto* frame = blink::WebLocalFrame::CreateMainFrame(
        view_, this, nullptr, broker_.Bind(), blink::LocalFrameToken(),
        blink::DocumentToken(), blink::InitiatorStateToken(), nullptr);
    CHECK(frame);
    broker_.SetFrameToken(frame->GetLocalFrameToken());
    CHECK(navigation_);
    mojo::PendingAssociatedRemote<blink::mojom::FrameWidgetHost> frame_host;
    auto frame_receiver = frame_host.InitWithNewEndpointAndPassReceiver();
    frame_host.EnableUnassociatedUsage();
    frame_host_receiver_.Bind(std::move(frame_receiver));
    mojo::PendingAssociatedRemote<blink::mojom::WidgetHost> widget_host;
    auto widget_receiver = widget_host.InitWithNewEndpointAndPassReceiver();
    widget_host.EnableUnassociatedUsage();
    widget_host_receiver_.Bind(std::move(widget_receiver));
    auto* widget = frame->InitializeFrameWidget(
        std::move(frame_host), mojo::NullAssociatedReceiver(),
        std::move(widget_host), mojo::NullAssociatedReceiver(),
        viz::FrameSinkId(1, next_widget_identifier_++));
    CHECK(widget);
    frame_ = frame;
    widget_ = widget;
    widget->InitializeNonCompositing(this);
    view_->DidAttachLocalMainFrame();
    widget->Resize(viewport);
    view_->SetIsActive(true);
    static_cast<blink::WebViewImpl*>(view_)->SetTabsToLinks(true);
    widget->SetFocus(true);
    loop_ = std::make_unique<FrameLoop>(*view_, *widget, canvas,
        gfx::Rect(viewport), agent_group_->DefaultTaskRunner(), base::BindRepeating(
        &State::PresentComposition, weak_factory_.GetWeakPtr(), base::Unretained(&canvas),
        viewport, std::move(present)));
    CHECK(loop_->Start());
    std::puts("BLINK: non-composited main frame created");
  }
  ~State() override {
    devtools_frontend_.reset();
    devtools_target_.reset();
    std::printf("RENDERER: input_messages=%llu maximum_queue_us=%lld maximum_dispatch_us=%lld\n",
        static_cast<unsigned long long>(input_sequence_), static_cast<long long>(maximum_input_wait_),
        static_cast<long long>(maximum_input_dispatch_));
    navigation_request_.reset();
    loop_.reset();
    if (view_) {
      std::string storage_namespace = view_->GetSessionStorageNamespaceId();
      std::exchange(view_, nullptr)->Close();
      blink::ReleaseFirmwareStorageNamespace(storage_namespace);
    }
    UninstallResourceLoader();
  }
  void PresentComposition(SkCanvas* canvas, gfx::Size viewport, base::RepeatingClosure present) {
    if (composer_.Candidates().empty()) {
      candidate_popup_.Clear();
      present.Run();
      return;
    }
    std::vector<gfx::Rect> bounds;
    auto* controller = InputController();
    if (!controller || controller->CompositionRange().IsNull()) {
      composer_.Clear();
      candidate_popup_.Clear();
      present.Run();
      return;
    }
    int x = 8, y = 8;
    if (controller && controller->GetCompositionCharacterBounds(bounds) && !bounds.empty()) {
      x = bounds.front().x(); y = bounds.front().bottom();
    }
    candidate_popup_.Draw(*canvas, composer_.Candidates(), x, y, viewport.width(), viewport.height());
    present.Run();
  }
  const ScrollSnapshot& Snapshot() const {
    if (composer_.Candidates().empty()) return loop_->Snapshot();
    // Candidate pixels are an overlay and cannot be replayed as document scroll pictures.
    composition_snapshot_.acknowledgement = loop_->Snapshot().acknowledgement;
    return composition_snapshot_;
  }
  uint64_t KeyboardScrollTarget() const { return view_ ? uefi_browser::KeyboardScrollTarget(*view_) : 0; }
  void ScrollbarState(uint64_t element, bool horizontal, unsigned hover, unsigned press, uint64_t sequence) {
    if (view_) ApplyScrollbarState(*view_,element,horizontal,hover,press);
    // Queued content clicks must hit the geometry that was already presented by the BSP.
    loop_->Stop(); loop_->AcknowledgeScroll(sequence); loop_->Start();
  }
  void ScrollTo(uint64_t element, int x, int y, uint64_t sequence) {
    if (view_) {
      const auto& scene=loop_->Snapshot().scene;
      ApplyScrollOffset(*view_,element,x,y,scene && !scene->queries.empty());
    }
    // A pre-scroll raster cannot acknowledge this offset and would delay the next scroll event.
    loop_->Stop();
    loop_->AcknowledgeScroll(sequence); loop_->Start();
  }
  void SetVisible(bool visible) {
    if (loop_) loop_->SetVisible(visible);
    if (widget_) widget_->SetFocus(visible);
    if (view_) {
      view_->SetIsActive(visible);
      view_->SetVisibilityState(visible ? blink::mojom::PageVisibilityState::kVisible :
          blink::mojom::PageVisibilityState::kHidden, false);
    }
  }
  blink::WebString UserAgentOverride() override {
    return blink::WebString::FromUtf8(BrowserUserAgent(GoogleCompatibility(GURL(CurrentUrl()))));
  }
  std::optional<blink::UserAgentMetadata> UserAgentMetadataOverride() override {
    return BrowserUserAgentMetadata(GoogleCompatibility(GURL(CurrentUrl())));
  }
  scoped_refptr<blink::WebWorkerFetchContext> CreateWorkerFetchContext(
      blink::WebDedicatedWorkerHostFactoryClient*) override {
    return uefi_browser::CreateWorkerFetchContext(table_, frame_);
  }
  std::unique_ptr<blink::WebMediaPlayer> CreateMediaPlayer(
      const blink::WebMediaPlayerSource&, blink::WebMediaPlayerClient* client,
      blink::MediaInspectorContext*, blink::WebMediaPlayerEncryptedMediaClient*,
      blink::WebContentDecryptionModule*, const blink::WebString&,
      const cc::LayerTreeSettings*, scoped_refptr<base::TaskRunner>) override {
    return uefi_browser::CreateMediaPlayer(frame_, client);
  }
  void BindToFrame(blink::WebNavigationControl* frame) override {
    CHECK(!navigation_);
    navigation_ = frame;
  }
  void FrameDetached(blink::DetachReason reason) override {
    navigation_request_.reset();
    devtools_frontend_.reset();
    devtools_target_.reset();
    widget_ = nullptr;
    frame_ = nullptr;
    loop_.reset();
    if (navigation_)
      std::exchange(navigation_, nullptr)->Close(reason);
  }
  void DidFinishLoad() override {
    std::puts("BLINK: main document load finished");
  }
  void DidStopLoading() override {
    std::puts("BLINK: main frame stopped loading");
  }
  void DidAddMessageToConsole(const blink::WebConsoleMessage& message,
      const blink::WebString&, unsigned line, const blink::WebString&) override {
    const auto text = message.text.Utf8();
    std::printf("BLINK: console level=%d line=%u %.2048s\n",
                static_cast<int>(message.level), line, text.c_str());
  }
  void ScheduleNonCompositedAnimation() override {
    // The owned frame loop supplies a bounded 16 ms presentation cadence.
    if (loop_)
      loop_->Start();
  }
  void SetCursor(const ui::Cursor& cursor) override { cursor_ = cursor; }
  void UpdateTooltipUnderCursor(const std::u16string& text,
      base::i18n::TextDirection direction) override {
    pointer_tooltip_ = text;
    pointer_tooltip_direction_ = direction;
  }
  void UpdateTooltipFromKeyboard(const std::u16string& text,
      base::i18n::TextDirection direction, const gfx::Rect& bounds) override {
    keyboard_tooltip_ = text;
    keyboard_tooltip_direction_ = direction;
    keyboard_tooltip_bounds_ = bounds;
  }
  void ClearKeyboardTriggeredTooltip() override { keyboard_tooltip_.clear(); }
  void TextInputStateChanged(ui::mojom::TextInputStatePtr state) override {
    text_input_state_ = std::move(state);
  }
  void SelectionBoundsChanged(const gfx::Rect& anchor,
      base::i18n::TextDirection anchor_direction, const gfx::Rect& focus,
      base::i18n::TextDirection focus_direction, const gfx::Rect& bounds,
      bool anchor_first) override {
    selection_anchor_ = anchor;
    selection_focus_ = focus;
    selection_bounds_ = bounds;
    selection_anchor_direction_ = anchor_direction;
    selection_focus_direction_ = focus_direction;
    selection_anchor_first_ = anchor_first;
  }
  void SetHasTouchEventConsumers(
      blink::mojom::TouchEventConsumersPtr consumers) override {
    touch_consumers_ = std::move(consumers);
  }
  void IntrinsicSizingInfoChanged(
      blink::mojom::IntrinsicSizingInfoPtr information) override {
    intrinsic_sizing_ = std::move(information);
  }
  void AnimateDoubleTapZoomInMainFrame(const gfx::Point&,
      const gfx::Rect&) override {
    std::puts("BLINK: double-tap zoom is unsupported");
  }
  void ZoomToFindInPageRectInMainFrame(const gfx::Rect&) override {
    std::puts("BLINK: find-in-page zoom is unsupported");
  }
  void AutoscrollStart(const gfx::PointF&) override {
    std::puts("BLINK: autoscroll is unsupported");
  }
  void AutoscrollFling(const gfx::Vector2dF&) override {
    std::puts("BLINK: autoscroll fling is unsupported");
  }
  void AutoscrollEnd() override {
    std::puts("BLINK: autoscroll end received without an active autoscroll");
  }
  void CreateFrameSink(
      mojo::PendingReceiver<viz::mojom::CompositorFrameSink>,
      mojo::PendingRemote<viz::mojom::CompositorFrameSinkClient>,
      mojo::PendingRemote<blink::mojom::RenderInputRouterClient>) override {
    // A compositor request violates the non-composited widget configuration.
    NOTREACHED() << "Firmware widgets cannot create a compositor frame sink";
  }
  void RegisterRenderFrameMetadataObserver(
      mojo::PendingReceiver<cc::mojom::RenderFrameMetadataObserverClient>,
      mojo::PendingRemote<cc::mojom::RenderFrameMetadataObserver>) override {
    NOTREACHED() << "Firmware widgets cannot register compositor metadata";
  }
  void SetReferrerPolicy(network::mojom::ReferrerPolicy policy,
      const blink::InitiatorStateToken& token) override {
    referrer_policy_ = policy;
    initiator_token_ = token;
  }
  void AddContentSecurityPolicies(
      std::vector<network::mojom::ContentSecurityPolicyPtr> policies,
      const blink::InitiatorStateToken& token) override {
    for (auto& policy : policies)
      content_security_policies_.push_back(std::move(policy));
    initiator_token_ = token;
  }
  blink::WebInputEventResult Dispatch(const blink::WebInputEvent& event) {
    if (!widget_) return blink::WebInputEventResult::kNotHandled;
    auto* widget = widget_;
    bool previous = widget->HandlingInputEvent();
    widget->SetHandlingInputEvent(true);
    auto result = widget->HandleInputEvent(blink::WebCoalescedInputEvent(event, ui::LatencyInfo()));
    if (widget_ == widget) widget->SetHandlingInputEvent(previous);
    return result;
  }
  bool CanAcceptInput() const { return input_messages_.size() < RENDERER_INPUT_CAPACITY; }
  void QueuePointer(const PointerSample& sample) {
    RendererInputMessage message{};
    message.kind = RENDERER_INPUT_POINTER;
    message.x = sample.x; message.y = sample.y; message.wheel = sample.wheel;
    message.buttons = sample.buttons; message.wheel_consumed = sample.wheel_consumed;
    QueueInput(message);
  }
  void QueueKey(const KeySample& sample) {
    RendererInputMessage message{};
    message.kind = RENDERER_INPUT_KEY;
    message.scan = sample.key.scan; message.character = sample.key.character;
    message.shift = sample.shift; message.toggle = sample.toggle;
    message.input_method = static_cast<uint8_t>(sample.input_method);
    message.composition_action = static_cast<uint8_t>(sample.composition_action);
    QueueInput(message);
  }
  enum class InputOperation { Event, Scroll, Scrollbar };
  void QueueScroll(uint64_t element, int x, int y, uint64_t sequence,
                   bool scrollbar=false, bool horizontal=false) {
    RendererInputMessage message{};
    message.x=x; message.y=y; message.buttons=horizontal;
    QueueInput(message,scrollbar ? InputOperation::Scrollbar : InputOperation::Scroll,element,sequence);
  }
  void QueueInput(RendererInputMessage message, InputOperation operation=InputOperation::Event,
                  uint64_t element=0, uint64_t scroll_sequence=0) {
    CHECK(CanAcceptInput());
    message.version = RENDERER_PROTOCOL_VERSION;
    message.generation = input_generation_;
    message.sequence = ++input_sequence_;
    input_messages_.push_back({message, base::TimeTicks::Now(), operation, element, scroll_sequence});
    // Geometry updates must not overtake earlier events and change their hit-test targets.
    // DOM event handlers require a scheduler task so Canvas observers and microtasks
    // complete before the next event is dispatched.
    ScheduleInput();
  }
  void ScheduleInput() {
    if (!frame_ || input_scheduled_ || input_messages_.empty()) return;
    input_scheduled_ = true;
    frame_->GetTaskRunner(blink::TaskType::kInternalInputBlocking)->PostTask(FROM_HERE,
        base::BindOnce(&State::DrainInput, weak_factory_.GetWeakPtr()));
  }
  void DrainInput() {
    input_scheduled_ = false;
    if (input_dispatching_) return;
    if (!frame_) { input_messages_.clear(); return; }
    if (input_messages_.empty()) return;
    auto queued = input_messages_.front();
    input_messages_.pop_front();
    const auto& message = queued.message;
    if (message.generation != input_generation_) { ScheduleInput(); return; }
    auto started = base::TimeTicks::Now();
    maximum_input_wait_ = std::max(maximum_input_wait_, (started - queued.received).InMicroseconds());
    auto alive = weak_factory_.GetWeakPtr();
    input_dispatching_ = true;
    if (queued.operation == InputOperation::Scroll)
      ScrollTo(queued.element,message.x,message.y,queued.scroll_sequence);
    else if (queued.operation == InputOperation::Scrollbar)
      ScrollbarState(queued.element,message.buttons!=0,message.x,message.y,queued.scroll_sequence);
    else if (message.kind == RENDERER_INPUT_POINTER)
      Pointer({message.x, message.y, message.wheel, message.buttons, message.wheel_consumed != 0});
    else if (message.kind == RENDERER_INPUT_KEY)
      Keyboard({{message.scan, message.character}, message.shift, message.toggle,
          static_cast<InputMethod>(message.input_method), static_cast<CompositionAction>(message.composition_action)});
    if (!alive) return;
    input_dispatching_ = false;
    maximum_input_dispatch_ = std::max(maximum_input_dispatch_, (base::TimeTicks::Now()-started).InMicroseconds());
    ScheduleInput();
  }
  bool pointer_capabilities_=false;
  void Pointer(const PointerSample& sample) {
    if (candidate_pointer_) {
      if (!sample.buttons) candidate_pointer_ = false;
      return;
    }
    int candidate = candidate_popup_.Hit(sample.x, sample.y);
    if ((sample.buttons & 1) && candidate >= 0) {
      candidate_pointer_ = true;
      auto* controller = InputController();
      if (!controller || controller->CompositionRange().IsNull()) {
        composer_.Clear();
        candidate_popup_.Clear();
        loop_->Start();
        return;
      }
      ApplyComposition(composer_.Select(candidate));
      return;
    }
    if (sample.buttons && !composer_.Preedit().empty()) {
      auto alive = weak_factory_.GetWeakPtr();
      FinishComposition(false);
      if (!alive) return;
    }
    if (!pointer_capabilities_) {
      pointer_capabilities_=true;
      // Firmware pointer samples represent a mouse cursor rather than touch contacts.
      view_->GetSettings()->SetAvailablePointerTypes(static_cast<int>(blink::mojom::PointerType::kPointerFineType));
      view_->GetSettings()->SetPrimaryPointerType(blink::mojom::PointerType::kPointerFineType);
      view_->GetSettings()->SetAvailableHoverTypes(static_cast<int>(blink::mojom::HoverType::kHoverHoverType));
      view_->GetSettings()->SetPrimaryHoverType(blink::mojom::HoverType::kHoverHoverType);
    }
    if (!widget_) return;
    using Event = blink::WebInputEvent;
    using Button = blink::WebPointerProperties::Button;
    // Firmware keystrokes do not provide a reliable held-modifier state for mouse events.
    int modifiers = 0;
    if (sample.buttons & 1) modifiers |= Event::kLeftButtonDown;
    if (sample.buttons & 2) modifiers |= Event::kRightButtonDown;
    const auto now = base::TimeTicks::Now();
    const gfx::PointF position(sample.x, sample.y);
    if (sample.x != pointer_x_ || sample.y != pointer_y_) {
      blink::WebMouseEvent event(Event::Type::kMouseMove, position, position,
          Button::kNoButton, 0, modifiers, now);
      event.movement_x = sample.x - pointer_x_;
      event.movement_y = sample.y - pointer_y_;
      Dispatch(event);
    }
    for (unsigned mask : {1u, 2u}) {
      if (!widget_) return;
      if (!((sample.buttons ^ pointer_buttons_) & mask)) continue;
      bool down = sample.buttons & mask;
      if (down) {
        click_count_ = mask == last_click_button_ &&
            now - last_click_ < base::Milliseconds(500) &&
            std::abs(sample.x - last_click_x_) <= 4 &&
            std::abs(sample.y - last_click_y_) <= 4 ? click_count_ + 1 : 1;
        last_click_ = now;
        last_click_button_ = mask;
        last_click_x_ = sample.x;
        last_click_y_ = sample.y;
        widget_->SetFocus(true);
      }
      blink::WebMouseEvent event(down ? Event::Type::kMouseDown : Event::Type::kMouseUp,
          position, position, mask == 1 ? Button::kLeft : Button::kRight,
          click_count_, modifiers, now);
      Dispatch(event);
    }
    pointer_x_ = sample.x;
    pointer_y_ = sample.y;
    pointer_buttons_ = sample.buttons;
    if (sample.wheel) {
      blink::WebMouseWheelEvent event(Event::Type::kMouseWheel, modifiers, now);
      event.SetPositionInWidget(position);
      event.SetPositionInScreen(position);
      event.delta_y = sample.wheel * 80;
      event.wheel_ticks_y = sample.wheel;
      event.event_action = blink::WebMouseWheelEvent::EventAction::kScrollVertical;
      if (sample.wheel_consumed) event.dispatch_type = Event::DispatchType::kEventNonBlocking;
      auto result = Dispatch(event);
      if (!sample.wheel_consumed && result == blink::WebInputEventResult::kNotHandled && frame_) {
        // The software widget has no cc input handler; Blink's main-thread
        // scroll chain still enforces overflow and overscroll containment.
        auto* local = static_cast<blink::WebLocalFrameImpl*>(frame_)->GetFrame();
        if (!local || !local->View()) return;
        auto location = blink::HitTestLocation(blink::PhysicalOffset(
            local->View()->ConvertFromRootFrame(gfx::Point(sample.x, sample.y))));
        auto hit = local->GetEventHandler().HitTestResultAtLocation(location);
        auto* node = hit.InnerNode();
        if (!node) return;
        auto* target = node->GetDocument().GetFrame();
        if (!target) return;
        auto direction = sample.wheel > 0
            ? blink::mojom::blink::ScrollDirection::kScrollUpIgnoringWritingMode
            : blink::mojom::blink::ScrollDirection::kScrollDownIgnoringWritingMode;
        for (int line = 0; line < std::abs(sample.wheel) * 2; ++line)
          target->GetEventHandler().BubblingScroll(direction,
              ui::ScrollGranularity::kScrollByLine, node);
      }
    }
  }
  TextComposer composer_;
  ImePopup candidate_popup_;
  bool candidate_pointer_ = false;
  mutable ScrollSnapshot composition_snapshot_;
  blink::WebInputMethodController* InputController() {
    auto* focused = view_ ? view_->FocusedFrame() : nullptr;
    return focused ? focused->GetInputMethodController() : nullptr;
  }
  void FinishComposition(bool cancel) {
    bool pending = !composer_.Preedit().empty();
    bool had_candidates = !composer_.Candidates().empty();
    auto committed = composer_.Finish(cancel);
    candidate_popup_.Clear();
    if (pending || had_candidates) loop_->Start();
    auto* controller = InputController();
    if (!pending || !controller || controller->CompositionRange().IsNull()) return;
    if (cancel) controller->SetComposition(blink::WebString(), {}, blink::WebRange(), 0, 0);
    else controller->CommitText(blink::WebString::FromUtf16(committed), {}, blink::WebRange(), 0);
  }
  bool ComposeKey(const KeySample& sample) {
    auto* controller = InputController();
    if (!controller || controller->TextInputType() == blink::kWebTextInputTypeNone) {
      composer_.Clear();
      return false;
    }
    // Script-driven focus and selection changes can end a composition between keys.
    if (controller->CompositionRange().IsNull()) composer_.Clear();
    bool pending = !composer_.Preedit().empty();
    bool shortcut = (sample.shift & 0x80000000u) && (sample.shift & 252);
    bool cancel = sample.key.scan == 23 || sample.key.character == 27;
    if (sample.composition_action != CompositionAction::None ||
        sample.input_method == InputMethod::Latin || shortcut || cancel) {
      FinishComposition(cancel);
      return sample.composition_action != CompositionAction::None || (cancel && pending);
    }
    CompositionUpdate update = composer_.Feed(sample);
    if (!update.handled) {
      FinishComposition(false);
      return pending && sample.key.character == 13;
    }
    return ApplyComposition(update);
  }
  bool ApplyComposition(const CompositionUpdate& update) {
    auto* controller = InputController();
    if (!controller) { composer_.Clear(); return true; }
    loop_->Start();
    auto alive = weak_factory_.GetWeakPtr();
    auto* focused = view_->FocusedFrame();
    auto element = focused->GetDocument().FocusedElement();
    if (!update.committed.empty()) {
      if (!controller->CommitText(blink::WebString::FromUtf16(update.committed), {}, blink::WebRange(), 0)) {
        if (alive) composer_.Clear();
        return true;
      }
      if (!alive) return true;
      if (view_->FocusedFrame() != focused || focused->GetDocument().FocusedElement() != element) {
        composer_.Clear();
        return true;
      }
      controller = InputController();
    }
    if (!controller || !controller->SetComposition(blink::WebString::FromUtf16(update.preedit), {},
        blink::WebRange(), static_cast<int>(composer_.Cursor()), static_cast<int>(composer_.Cursor()))) {
      if (alive) composer_.Clear();
    }
    return true;
  }
  void Keyboard(const KeySample& sample) {
    if (!sample.key.scan && !sample.key.character && sample.composition_action == CompositionAction::None) return;
    auto alive = weak_factory_.GetWeakPtr();
    if (ComposeKey(sample) || !alive) return;
    using Event = blink::WebInputEvent;
    int modifiers = 0;
    if (sample.shift & 0x80000000u) {
      if (sample.shift & 3) modifiers |= Event::kShiftKey;
      if (sample.shift & 12) modifiers |= Event::kControlKey;
      if (sample.shift & 48) modifiers |= Event::kAltKey;
      if (sample.shift & 192) modifiers |= Event::kMetaKey;
    }
    if (sample.toggle & 0x80) {
      if (sample.toggle & 4) modifiers |= Event::kCapsLockOn;
      if (sample.toggle & 2) modifiers |= Event::kNumLockOn;
    }
    if (!sample.key.scan && !sample.key.character) return;
    int key = 0;
    ui::DomKey dom_key = ui::DomKey::NONE;
    switch (sample.key.scan) {
      case 1: key=ui::VKEY_UP; dom_key=ui::DomKey::ARROW_UP; break;
      case 2: key=ui::VKEY_DOWN; dom_key=ui::DomKey::ARROW_DOWN; break;
      case 3: key=ui::VKEY_RIGHT; dom_key=ui::DomKey::ARROW_RIGHT; break;
      case 4: key=ui::VKEY_LEFT; dom_key=ui::DomKey::ARROW_LEFT; break;
      case 5: key=ui::VKEY_HOME; dom_key=ui::DomKey::HOME; break;
      case 6: key=ui::VKEY_END; dom_key=ui::DomKey::END; break;
      case 7: key=ui::VKEY_INSERT; dom_key=ui::DomKey::INSERT; break;
      case 8: key=ui::VKEY_DELETE; dom_key=ui::DomKey::DEL; break;
      case 9: key=ui::VKEY_PRIOR; dom_key=ui::DomKey::PAGE_UP; break;
      case 10: key=ui::VKEY_NEXT; dom_key=ui::DomKey::PAGE_DOWN; break;
      case 23: key=ui::VKEY_ESCAPE; dom_key=ui::DomKey::ESCAPE; break;
      default: break;
    }
    if (sample.key.scan >= 11 && sample.key.scan <= 22) {
      static const ui::DomKey function_keys[] = {
          ui::DomKey::F1, ui::DomKey::F2, ui::DomKey::F3, ui::DomKey::F4,
          ui::DomKey::F5, ui::DomKey::F6, ui::DomKey::F7, ui::DomKey::F8,
          ui::DomKey::F9, ui::DomKey::F10, ui::DomKey::F11, ui::DomKey::F12};
      key = ui::VKEY_F1 + sample.key.scan - 11;
      dom_key = function_keys[sample.key.scan - 11];
    }
    char16_t character = sample.key.character;
    if (!key) {
      dom_key = ui::DomKey::FromCharacter(character);
      if (character >= 'a' && character <= 'z') key = character - 'a' + 'A';
      else if (character >= 'A' && character <= 'Z') key = character;
      else if (character >= '0' && character <= '9') key = character;
      else key = character;
      if (character == 8) dom_key = ui::DomKey::BACKSPACE;
      if (character == 9) dom_key = ui::DomKey::TAB;
      if (character == 13) dom_key = ui::DomKey::ENTER;
      if (character == 27) dom_key = ui::DomKey::ESCAPE;
      if (character > 0 && character < 27 && (modifiers & Event::kControlKey) && character != 9 && character != 13 && character != 8) {
        key = 'A' + character - 1;
        dom_key = ui::DomKey::FromCharacter('a' + character - 1);
      }
    }
    blink::WebKeyboardEvent event(Event::Type::kRawKeyDown, modifiers, base::TimeTicks::Now());
    const auto code = ui::UsLayoutDomKeyToDomCode(dom_key);
    if (!sample.key.scan) key = ui::DomCodeToUsLayoutNonLocatedKeyboardCode(code);
    event.windows_key_code = key;
    event.native_key_code = sample.key.scan;
    event.dom_key = dom_key;
    event.dom_code = static_cast<int>(code);
    event.text[0] = character;
    event.unmodified_text[0] = character;
    Dispatch(event);
    if (character) {
      event.SetType(Event::Type::kChar);
      Dispatch(event);
    }
    // UEFI reports keystrokes rather than physical key releases.
    event.SetType(Event::Type::kKeyUp);
    Dispatch(event);
  }
  blink::WebLocalFrame* CreateChildFrame(blink::mojom::TreeScopeType scope,
      const blink::WebString&,const blink::WebString&,const blink::FramePolicy&,
      const blink::WebFrameOwnerProperties&,blink::FrameOwnerElementType,
      blink::WebPolicyContainerBindParams bind,ukm::SourceId,FinishChildFrameCreationFn finish) override {
    return ChildFrame::Create(frame_,table_,base::BindRepeating(&State::ScheduleNonCompositedAnimation,
        weak_factory_.GetWeakPtr()),scope,std::move(bind),finish);
  }
  void BeginNavigation(std::unique_ptr<blink::WebNavigationInfo> info) override {
    if (info->url_request.HttpMethod().Utf8() != "GET" && info->url_request.HttpMethod().Utf8() != "POST") {
      std::puts("BLINK: unsupported navigation method");
      return;
    }
    agent_group_->DefaultTaskRunner()->PostTask(FROM_HERE,
        base::BindOnce(&State::NavigateRequest, weak_factory_.GetWeakPtr(), std::move(info)));
  }
  void NavigateRequest(std::unique_ptr<blink::WebNavigationInfo> info) {
    if (!frame_ || !navigation_) return;
    if (info->navigation_policy != blink::kWebNavigationPolicyCurrentTab) {
      bool new_tab = info->navigation_policy == blink::kWebNavigationPolicyNewForegroundTab ||
          info->navigation_policy == blink::kWebNavigationPolicyNewBackgroundTab ||
          info->navigation_policy == blink::kWebNavigationPolicyNewWindow ||
          info->navigation_policy == blink::kWebNavigationPolicyNewPopup;
      std::printf("TABS: navigation policy=%d activation=%d\n",
          static_cast<int>(info->navigation_policy),info->has_transient_user_activation);
      if (new_tab && info->has_transient_user_activation && info->url_request.HttpMethod().Utf8() == "GET")
        OpenTabQueued(info->url_request.Url().GetString().Utf8());
      return;
    }
    SaveHistoryItem();
    GURL target(info->url_request.Url().GetString().Utf8());
    GURL current(frame_->GetDocument().Url().GetString().Utf8());
    if (IsBrowserPage(target)) {
      // Only a gesture from the trusted current document may change preferences.
      if (!IsBrowserPage(current) || !info->has_transient_user_activation ||
          !info->url_request.RequestorOrigin().IsSameOriginWith(frame_->GetDocument().GetSecurityOrigin())) {
        std::puts("SETTINGS: rejected untrusted navigation");
        return;
      }
      if (target.has_query()) {
        if (current.path() != "settings" || target.path() != "settings" ||
            !ChangePreference(target.query(), table_)) return;
        NavigateInternal("about:settings", blink::WebFrameLoadType::kReplaceCurrentItem, {});
        return;
      }
    }
    if (info->url_request.HttpMethod().Utf8() == "GET" && target.has_ref() && target.EqualsIgnoringRef(current) &&
        (info->frame_load_type == blink::WebFrameLoadType::kStandard ||
         info->frame_load_type == blink::WebFrameLoadType::kReplaceCurrentItem)) {
      auto result = navigation_->CommitSameDocumentNavigation(
          info->url_request.Url(), info->frame_load_type, blink::WebHistoryItem(),
          info->is_client_redirect, info->has_transient_user_activation,
          info->url_request.RequestorOrigin(), false, false, std::nullopt, true);
      if (result != blink::mojom::CommitResult::RestartCrossDocument) {
        std::printf("BLINK: same-document navigation result=%d\n", static_cast<int>(result));
        return;
      }
    }
    Status status = NavigateInternal(target.spec().c_str(), info->frame_load_type, {}, blink::WebNavigationParams::CreateFromInfo(*info));
    if (status) std::printf("BLINK: navigation failed status=%llx\n", static_cast<unsigned long long>(status));
  }
  blink::WebView* CreateNewWindow(const blink::WebURLRequest& request,
      const blink::WebWindowFeatures&, const blink::WebString&, const gfx::Rect&,
      blink::WebNavigationPolicy, network::mojom::WebSandboxFlags,
      const blink::SessionStorageNamespaceId&, bool& consumed_user_gesture,
      const std::optional<blink::WebPictureInPictureWindowOptions>&,
      const blink::WebURL&) override {
    // New-window requests are queued so tab creation cannot reenter Blink input dispatch.
    if (request.HttpMethod().Utf8() != "GET" || !request.HasUserGesture()) return nullptr;
    consumed_user_gesture = true;
    agent_group_->DefaultTaskRunner()->PostTask(FROM_HERE,
        base::BindOnce(&State::OpenTabQueued, weak_factory_.GetWeakPtr(),
                       request.Url().GetString().Utf8()));
    return nullptr;
  }
  void OpenTabQueued(std::string url) {
    GURL target(url);
    if (!target.SchemeIsHTTPOrHTTPS() || target.has_username() || target.has_password()) {
      std::puts("BLINK: unsupported navigation URL");
      return;
    }
    std::printf("BLINK: navigating %s\n", url.c_str());
    open_tab_.Run(std::move(url));
  }
  void FocusedElementChanged(const blink::WebElement& element) override {
    if (!element.IsNull()) std::printf("BLINK: focus tag=%s\n", element.TagName().Utf8().c_str());
  }
  void DidChangeScrollOffset(blink::mojom::ScrollType) override {
    if (frame_) std::printf("BLINK: scroll offset %.0f %.0f\n", frame_->GetScrollOffset().x(), frame_->GetScrollOffset().y());
  }

  struct HistoryEntry {
    std::string url;
    blink::WebHistoryItem item;
  };
  void SaveHistoryItem() {
    if (!frame_ || history_.empty()) return;
    frame_->UpdateCurrentHistoryItem();
    history_[history_index_].item = frame_->GetCurrentHistoryItem();
  }
  void RecordCommit(blink::WebHistoryCommitType type) {
    if (!frame_) return;
    frame_->UpdateCurrentHistoryItem();
    HistoryEntry entry{CurrentUrl(), frame_->GetCurrentHistoryItem()};
    if (entry.item.IsNull()) return;
    if (history_.empty()) {
      history_.push_back(entry);
      history_index_ = 0;
    } else if (pending_history_index_ >= 0) {
      history_index_ = static_cast<size_t>(pending_history_index_);
      history_[history_index_] = entry;
    } else if (type == blink::kWebStandardCommit) {
      history_.resize(history_index_ + 1);
      history_.push_back(entry);
      if (history_.size() > 256) history_.erase(history_.begin());
      history_index_ = history_.size() - 1;
    } else {
      history_[history_index_] = entry;
    }
    pending_history_index_ = -1;
    view_->SetHistoryListFromNavigation(static_cast<int32_t>(history_index_), static_cast<int32_t>(history_.size()));
    std::printf("HISTORY: index=%zu count=%zu back=%d forward=%d url=%s\n",
        history_index_, history_.size(), CanGoBack(), CanGoForward(), entry.url.c_str());
  }
  void DidCommitNavigation(blink::WebHistoryCommitType type, bool,
      const network::ParsedPermissionsPolicy&,
      const blink::DocumentPolicyFeatureState&) override {
    composer_.Clear();
    ++input_generation_;
    input_messages_.clear();
    pointer_buttons_ = 0;
    click_count_ = 0;
    // A committed document must not present a snapshot of its predecessor.
    if (loop_) { loop_->Stop(); loop_->Start(); }
    RecordCommit(type);
  }
  void DidFinishSameDocumentNavigation(blink::WebHistoryCommitType type, bool,
      blink::mojom::SameDocumentNavigationType, bool,
      const std::optional<blink::SameDocNavigationScreenshotDestinationToken>&,
      base::UnguessableToken, bool) override {
    RecordCommit(type);
  }
  void DidUpdateCurrentHistoryItem() override {
    if (frame_ && !history_.empty() && pending_history_index_ < 0) {
      frame_->UpdateCurrentHistoryItem();
      history_[history_index_].item = frame_->GetCurrentHistoryItem();
    }
  }
  bool CanGoBack() const { return !history_.empty() && history_index_ > 0; }
  bool CanGoForward() const { return !history_.empty() && history_index_ + 1 < history_.size(); }
  Status Traverse(int offset) {
    if ((offset < 0 && !CanGoBack()) || (offset > 0 && !CanGoForward())) return 0;
    SaveHistoryItem();
    size_t destination = offset < 0 ? history_index_ - 1 : history_index_ + 1;
    HistoryEntry entry = history_[destination];
    pending_history_index_ = static_cast<int>(destination);
    Status status = 0;
    if (entry.item.DocumentSequenceNumber() == frame_->GetCurrentHistoryItem().DocumentSequenceNumber()) {
      auto result = navigation_->CommitSameDocumentNavigation(
          blink::WebURL(blink::KURL(GURL(entry.url))), blink::WebFrameLoadType::kBackForward,
          entry.item, false, true, blink::WebSecurityOrigin(), true, false, std::nullopt, true);
      if (result == blink::mojom::CommitResult::RestartCrossDocument)
        status = NavigateInternal(entry.url.c_str(), blink::WebFrameLoadType::kBackForward, entry.item);
      else if (result != blink::mojom::CommitResult::Ok) status = ERROR(21);
    } else {
      status = NavigateInternal(entry.url.c_str(), blink::WebFrameLoadType::kBackForward, entry.item);
    }
    if (status) pending_history_index_ = -1;
    return status;
  }
  Status Reload() {
    pending_history_index_ = -1;
    SaveHistoryItem();
    return NavigateInternal(CurrentUrl().c_str(), blink::WebFrameLoadType::kReload, {});
  }
  std::string CurrentUrl() const { return frame_ ? frame_->GetDocument().Url().GetString().Utf8() : std::string(); }
  Status Navigate(const char* url) {
    pending_history_index_ = -1;
    SaveHistoryItem();
    return NavigateInternal(url, blink::WebFrameLoadType::kStandard, {});
  }
  Status Inspect(State& target) {
    if (&target == this || !frame_ || !target.frame_ || devtools_frontend_ || target.devtools_frontend_)
      return ERROR(2);
    if (!target.devtools_target_)
      target.devtools_target_ = std::make_unique<DevToolsTarget>(target.frame_);
    devtools_frontend_ = std::make_unique<DevToolsFrontend>(frame_, *target.devtools_target_);
    return Navigate(kDevToolsFrontendUrl);
  }
  Status NavigateInternal(const char* url, blink::WebFrameLoadType load_type,
                          blink::WebHistoryItem history_item,
                          std::unique_ptr<blink::WebNavigationParams> parameters = {}) {
    CHECK(navigation_);
    // An authorized tools frame retains its bindings until it closes.
    if (devtools_frontend_ && !IsDevToolsOrigin(GURL(url))) return ERROR(2);
    composer_.Clear();
    ++input_generation_;
    input_messages_.clear();
    const bool internal = IsBrowserPage(GURL(url)) || IsDevToolsOrigin(GURL(url));
    view_->GetSettings()->SetJavaScriptEnabled(internal || preferences.javascript);
    view_->GetSettings()->SetImagesEnabled(internal || preferences.images);
    view_->GetSettings()->SetLoadsImagesAutomatically(internal || preferences.images);
    view_->GetSettings()->SetDefaultFontSize(internal ? 16 : preferences.font_size);
    std::printf("SETTINGS: apply internal=%d font=%u javascript=%d images=%d\n", internal,
        internal ? 16 : preferences.font_size, internal || preferences.javascript, internal || preferences.images);
    navigation_pending_ = true;
    navigation_status_ = 0;
    policy_receiver_.reset();
    content_security_policies_.clear();
    referrer_policy_ = network::mojom::ReferrerPolicy::kDefault;
    mojo::PendingAssociatedRemote<blink::mojom::PolicyContainerHost> remote;
    auto receiver = remote.InitWithNewEndpointAndPassReceiver();
    remote.EnableUnassociatedUsage();
    policy_receiver_.Bind(std::move(receiver));
    auto policy = std::make_unique<blink::WebPolicyContainer>(
        blink::WebPolicyContainerPolicies{}, std::move(remote));
    navigation_request_ = LoadNavigation(table_, *navigation_, url, std::move(policy), load_type, history_item,
        base::BindOnce(&State::NavigationFinished, weak_factory_.GetWeakPtr()), std::move(parameters));
    return 0;
  }
  bool NavigationPending() const { return navigation_pending_; }
  Status NavigationStatus() const { return navigation_status_; }
  void NavigationFinished(Status result) {
    navigation_pending_ = false;
    navigation_status_ = result;
    if (loop_) loop_->Start();
    if (result) {
      pending_history_index_ = -1;
      std::printf("BLINK: navigation failed status=%llx\n", static_cast<unsigned long long>(result));
    }
  }
 private:
  struct QueuedInput {
    RendererInputMessage message;
    base::TimeTicks received;
    InputOperation operation;
    uint64_t element, scroll_sequence;
  };
  std::deque<QueuedInput> input_messages_;
  uint64_t input_generation_ = 0, input_sequence_ = 0;
  int64_t maximum_input_wait_ = 0, maximum_input_dispatch_ = 0;
  bool input_scheduled_ = false, input_dispatching_ = false;
  base::RepeatingCallback<void(std::string)> open_tab_;
  inline static uint32_t next_widget_identifier_ = 1;
  bool navigation_pending_ = false;
  Status navigation_status_ = 0;
  std::unique_ptr<NavigationRequest> navigation_request_;
  std::vector<HistoryEntry> history_;
  size_t history_index_ = 0;
  int pending_history_index_ = -1;
  mojo::AssociatedReceiver<blink::mojom::FrameWidgetHost> frame_host_receiver_{this};
  mojo::AssociatedReceiver<blink::mojom::WidgetHost> widget_host_receiver_{this};
  ui::Cursor cursor_;
  std::u16string pointer_tooltip_;
  std::u16string keyboard_tooltip_;
  base::i18n::TextDirection pointer_tooltip_direction_{};
  base::i18n::TextDirection keyboard_tooltip_direction_{};
  gfx::Rect keyboard_tooltip_bounds_;
  ui::mojom::TextInputStatePtr text_input_state_;
  blink::mojom::TouchEventConsumersPtr touch_consumers_;
  blink::mojom::IntrinsicSizingInfoPtr intrinsic_sizing_;
  gfx::Rect selection_anchor_;
  gfx::Rect selection_focus_;
  gfx::Rect selection_bounds_;
  base::i18n::TextDirection selection_anchor_direction_{};
  base::i18n::TextDirection selection_focus_direction_{};
  bool selection_anchor_first_ = true;
  mojo::AssociatedReceiver<blink::mojom::PolicyContainerHost> policy_receiver_{this};
  network::mojom::ReferrerPolicy referrer_policy_ = network::mojom::ReferrerPolicy::kDefault;
  blink::InitiatorStateToken initiator_token_;
  std::vector<network::mojom::ContentSecurityPolicyPtr> content_security_policies_;
  SystemTable* table_;
  blink::WebViewClient view_client_;
  BrowserBroker broker_;
  std::unique_ptr<blink::scheduler::WebAgentGroupScheduler> agent_group_;
  blink::WebView* view_ = nullptr;
  blink::WebNavigationControl* navigation_ = nullptr;
  std::unique_ptr<FrameLoop> loop_;
  blink::WebLocalFrame* frame_ = nullptr;
  blink::WebFrameWidget* widget_ = nullptr;
  int pointer_x_ = 0, pointer_y_ = 0;
  unsigned pointer_buttons_ = 0;
  base::TimeTicks last_click_;
  unsigned last_click_button_ = 0;
  int last_click_x_ = 0, last_click_y_ = 0, click_count_ = 0;
  std::unique_ptr<DevToolsTarget> devtools_target_;
  std::unique_ptr<DevToolsFrontend> devtools_frontend_;
  base::WeakPtrFactory<State> weak_factory_{this};
};
BrowserFrame::BrowserFrame(SystemTable* table,
    blink::scheduler::WebThreadScheduler& scheduler, SkCanvas& canvas,
    gfx::Size viewport, base::RepeatingClosure present,
    base::RepeatingCallback<void(std::string)> open_tab)
{
  if (RemoteRendererConfigured()) remote_=std::make_unique<RemoteFrame>(canvas,viewport,std::move(present),std::move(open_tab));
  else state_=std::make_unique<State>(table,scheduler,canvas,viewport,std::move(present),std::move(open_tab));
}
BrowserFrame::~BrowserFrame() = default;
Status BrowserFrame::Inspect(BrowserFrame& target) {
  if (remote_ && target.remote_) return remote_->Inspect(*target.remote_);
  if (state_ && target.state_) return state_->Inspect(*target.state_);
  return ERROR(2);
}
const ScrollSnapshot& BrowserFrame::Snapshot() const { return state_->Snapshot(); }
uint64_t BrowserFrame::KeyboardScrollTarget() const { return state_->KeyboardScrollTarget(); }
void BrowserFrame::ScrollbarState(uint64_t element, bool horizontal, unsigned hover, unsigned press, uint64_t sequence) { state_->QueueScroll(element,hover,press,sequence,true,horizontal); }
void BrowserFrame::ScrollTo(uint64_t element, int x, int y, uint64_t sequence) { state_->QueueScroll(element,x,y,sequence); }
Status BrowserFrame::Navigate(const char* url) { return remote_ ? remote_->Navigate(url) : state_->Navigate(url); }
bool BrowserFrame::NavigationPending() const { return remote_ ? remote_->NavigationPending() : state_->NavigationPending(); }
Status BrowserFrame::NavigationStatus() const { return remote_ ? remote_->NavigationStatus() : state_->NavigationStatus(); }
std::string BrowserFrame::CurrentUrl() const { return remote_ ? remote_->CurrentUrl() : state_->CurrentUrl(); }
bool BrowserFrame::CanGoBack() const { return remote_ ? remote_->CanGoBack() : state_->CanGoBack(); }
bool BrowserFrame::CanGoForward() const { return remote_ ? remote_->CanGoForward() : state_->CanGoForward(); }
Status BrowserFrame::GoBack() { return remote_ ? remote_->GoBack() : state_->Traverse(-1); }
Status BrowserFrame::GoForward() { return remote_ ? remote_->GoForward() : state_->Traverse(1); }
Status BrowserFrame::Reload() { return remote_ ? remote_->Reload() : state_->Reload(); }
void BrowserFrame::SetVisible(bool visible) { if (remote_) remote_->SetVisible(visible); else state_->SetVisible(visible); }
bool BrowserFrame::CanAcceptInput() const { return remote_ ? remote_->CanAcceptInput() : state_->CanAcceptInput(); }
void BrowserFrame::Pointer(const PointerSample& sample) { if (remote_) remote_->Pointer(sample); else state_->QueuePointer(sample); }
void BrowserFrame::PointerLeave() { if (remote_) remote_->PointerLeave(); }
void BrowserFrame::Keyboard(const KeySample& sample) { if (remote_) remote_->Keyboard(sample); else state_->QueueKey(sample); }
}

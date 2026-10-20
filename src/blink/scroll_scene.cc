#include "blink/scroll_scene.h"
#include "blink/scroll_wire.h"
#include "third_party/blink/public/common/metrics/document_update_reason.h"
#include "third_party/blink/renderer/core/layout/scroll_anchor.h"
#include "third_party/blink/renderer/core/layout/custom_scrollbar.h"
#include "third_party/blink/renderer/core/layout/layout_custom_scrollbar_part.h"
#include "third_party/blink/renderer/core/paint/custom_scrollbar_theme.h"
#include "third_party/blink/renderer/platform/graphics/paint/drawing_display_item.h"
#include "third_party/blink/renderer/core/paint/object_paint_properties.h"
#include "third_party/blink/renderer/core/css/style_request.h"
#include "third_party/blink/renderer/core/css/container_query_evaluator.h"
#include "third_party/blink/renderer/core/css/scroll_state_query_snapshot.h"
#include "third_party/blink/renderer/core/layout/layout_object_inlines.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <map>
#include <set>
#include <utility>
#include "third_party/blink/renderer/platform/bindings/script_forbidden_scope.h"
#include "third_party/blink/renderer/platform/heap/persistent.h"
#include "third_party/blink/renderer/core/dom/document.h"
#include "third_party/blink/renderer/core/dom/element.h"
#include "third_party/blink/renderer/core/frame/local_dom_window.h"
#include "third_party/blink/renderer/core/event_type_names.h"
#include "third_party/blink/renderer/core/editing/editing_utilities.h"
#include "third_party/blink/renderer/core/html/html_body_element.h"
#include "third_party/blink/renderer/core/html/forms/html_form_control_element.h"
#include "third_party/blink/renderer/core/scroll/scrollbar.h"
#include "third_party/blink/renderer/core/scroll/scrollbar_theme.h"
#include "third_party/blink/renderer/core/paint/paint_info.h"
#include "third_party/blink/renderer/platform/graphics/paint/paint_record_builder.h"
#include "third_party/blink/renderer/platform/graphics/paint/scrollbar_display_item.h"
#include "third_party/blink/renderer/platform/graphics/paint/hit_test_data.h"
#include "cc/paint/paint_record.h"
#include "skia/ext/font_utils.h"
#include "third_party/skia/include/core/SkPictureRecorder.h"
#include "third_party/skia/include/core/SkBBHFactory.h"
#include "third_party/skia/include/core/SkSerialProcs.h"
#include "third_party/skia/include/core/SkStream.h"
#include "third_party/skia/include/core/SkSurface.h"
#include "third_party/blink/public/web/web_view.h"
#include "third_party/blink/public/platform/web_runtime_features.h"
#include "third_party/blink/renderer/platform/runtime_enabled_features.h"
#include "third_party/blink/renderer/core/frame/web_local_frame_impl.h"
#include "third_party/blink/renderer/core/frame/local_frame.h"
#include "third_party/blink/renderer/core/frame/local_frame_view.h"
#include "third_party/blink/renderer/core/frame/event_handler_registry.h"
#include "third_party/blink/renderer/core/layout/layout_view.h"
#include "third_party/blink/renderer/core/paint/paint_layer_scrollable_area.h"
#include "third_party/blink/renderer/core/paint/paint_layer.h"
#include "third_party/blink/renderer/platform/graphics/paint/paint_controller.h"
#include "third_party/blink/renderer/platform/graphics/paint/paint_chunk_subset.h"
#include "third_party/blink/renderer/platform/graphics/compositing/paint_chunks_to_cc_layer.h"
#include "third_party/blink/renderer/platform/graphics/graphics_context.h"
namespace uefi_browser {
namespace {
// Paint properties are shared across local frames, while scroll-area registries are per frame.
std::vector<blink::LocalFrameView*> FrameViews(blink::LocalFrameView* root) {
  std::vector<blink::LocalFrameView*> views;
  if (!root) return views;
  auto* first=&root->GetFrame();
  for (auto* frame=static_cast<blink::Frame*>(first);frame;frame=frame->Tree().TraverseNext(first)) {
    auto* local=blink::DynamicTo<blink::LocalFrame>(frame);
    if (!local || !local->View() || !local->View()->GetLayoutView()) return {};
    views.push_back(local->View());
    if (views.size()>16) return {};
  }
  return views;
}
uint64_t StableScrollId(blink::ScrollableArea* area) {
  auto* layer=blink::DynamicTo<blink::PaintLayerScrollableArea>(area);
  auto* node=layer && layer->GetLayoutBox() ? layer->GetLayoutBox()->GetNode() : nullptr;
  return node ? (uint64_t{1}<<63)|static_cast<uint64_t>(node->GetDomNodeId()) : 0;
}
blink::ScrollableArea* FindArea(blink::LocalFrameView* root,uint64_t element) {
  if (!element) return nullptr;
  for (auto* view:FrameViews(root)) {
    if (!(element&(uint64_t{1}<<63))) {
      if (auto* area=view->ScrollableAreaWithElementId(blink::CompositorElementId(element))) return area;
    } else {
      if (view->LayoutViewport() && StableScrollId(view->LayoutViewport())==element) return view->LayoutViewport();
      for (const auto& entry:view->ScrollableAreas()) if (StableScrollId(entry.value.Get())==element) return entry.value.Get();
    }
  }
  return nullptr;
}
// A detached paint proxy avoids input notifications and mutations of the live scrollbar.
class ScrollbarPaintProxy final : public blink::Scrollbar {
 public:
  ScrollbarPaintProxy(blink::ScrollableArea* area,const blink::Scrollbar& source)
      : blink::Scrollbar(area,source.Orientation(),source.StyleSource(),&source.GetTheme()) {
    scrollable_area_=nullptr; SetFrameRect(source.FrameRect()); scrollable_area_=area;
    visible_size_=source.VisibleSize(); total_size_=source.TotalSize();
  }
  void State(blink::ScrollbarPart part,unsigned appearance) {
    hovered_part_=(appearance&1) ? part : blink::kNoPart;
    pressed_part_=(appearance&2) ? part : blink::kNoPart;
    enabled_=!(appearance&4);
    current_pos_=std::max(1,Maximum())/2.0f;
  }
};
void InvalidateCullRects(blink::PaintLayer* layer) {
  layer->SetNeedsCullRectUpdate();
  for (auto* child=layer->FirstChild();child;child=child->NextSibling()) InvalidateCullRects(child);
}
struct Capture {
  ScrollScene scene;
  blink::LocalFrameView* view;
  const ScrollScene* previous=nullptr;
  bool valid=true;
  size_t bytes=0;
  bool Reserve(size_t size) {
    constexpr size_t limit=32*1024*1024;
    if (size>limit || bytes>limit-size) { valid=false; return false; }
    bytes+=size; return true;
  }
  std::map<const blink::TransformPaintPropertyNode*,int> transforms;
  std::map<const blink::ClipPaintPropertyNode*,int> clips;
  std::map<const blink::EffectPaintPropertyNode*,int> effects;
  std::map<uint32_t,uint32_t> images,fonts;
  int Transform(const blink::TransformPaintPropertyNode* node) {
    if (!node || node==&blink::TransformPaintPropertyNode::Root()) return -1;
    if (auto found=transforms.find(node); found!=transforms.end()) return found->second;
    ScrollTransform value;
    value.parent=Transform(node->UnaliasedParent());
    if (!node->IsIdentityOr2dTranslation()) { valid=false; return -1; }
    auto translation=node->Get2dTranslation(); value.x=translation.x(); value.y=translation.y();
    if (auto* scroll=node->ScrollNode()) {
      auto* area=FindArea(view,scroll->GetCompositorElementId().GetInternalValue());
      if (area) {
        if (!scroll->GetMainThreadRepaintReasons().empty() || scroll->GetSnapContainerData()) valid=false;
        value.element=scroll->GetCompositorElementId().GetInternalValue();
        if (scroll->Parent()) value.scroll_parent=scroll->Parent()->GetCompositorElementId().GetInternalValue();
        value.position=area->GetScrollOffset().y(); value.y+=value.position;
        value.position_x=area->GetScrollOffset().x(); value.x+=value.position_x;
        value.minimum=area->MinimumScrollOffset().y(); value.minimum_x=area->MinimumScrollOffset().x();
        value.maximum_x=area->MaximumScrollOffset().x(); value.viewport_x=area->VisibleWidth();
        value.horizontal=scroll->UserScrollableHorizontal(); value.vertical=scroll->UserScrollableVertical();
        value.chain_x=scroll->OverscrollBehavior().x==cc::OverscrollBehavior::Type::kAuto;
        value.maximum=area->MaximumScrollOffset().y(); value.viewport=area->VisibleHeight();
        value.chain=scroll->OverscrollBehavior().y==cc::OverscrollBehavior::Type::kAuto;
      }
    }
    if (auto* constraint=node->GetStickyConstraint()) {
      value.sticky=true; value.constraint=*constraint;
      value.sticky_element=node->GetCompositorElementId().GetInternalValue();
    }
    int index=static_cast<int>(scene.transforms.size()); transforms[node]=index; scene.transforms.push_back(value); return index;
  }
  int Clip(const blink::ClipPaintPropertyNode* node) {
    if (!node || node==&blink::ClipPaintPropertyNode::Root()) return -1;
    if (auto found=clips.find(node); found!=clips.end()) return found->second;
    ScrollClip value;
    value.parent=Clip(node->UnaliasedParent());
    value.transform=Transform(&node->LocalTransformSpace().Unalias());
    if (node->ClipPath() || node->PixelMovingFilter()) valid=false;
    value.rectangle=SkRRect(node->PaintClipRect());
    int index=static_cast<int>(scene.clips.size()); clips[node]=index; scene.clips.push_back(value); return index;
  }
  int Effect(const blink::EffectPaintPropertyNode* node) {
    if (!node || node==&blink::EffectPaintPropertyNode::Root()) return -1;
    if (auto found=effects.find(node); found!=effects.end()) return found->second;
    ScrollEffect value;
    value.parent=Effect(node->UnaliasedParent());
    value.clip=node->OutputClip() ? Clip(&node->OutputClip()->Unalias()) : -1;
    value.opacity=node->Opacity(); value.blend=node->BlendMode();
    if (node->Filter() || node->BackdropFilter()) valid=false;
    int index=static_cast<int>(scene.effects.size()); effects[node]=index; scene.effects.push_back(value); return index;
  }
  static SkSerialReturnType Image(SkImage* image,void* context) {
    auto& self=*static_cast<Capture*>(context);
    auto [entry,added]=self.images.emplace(image->uniqueID(),self.scene.images.size());
    if (added) {
      ScrollImage resource; resource.width=image->width(); resource.height=image->height();
      size_t bytes=static_cast<size_t>(resource.width)*resource.height*4;
      if (!self.Reserve(bytes)) return SkData::MakeEmpty();
      if (self.previous) {
        auto found=std::find(self.previous->image_keys.begin(),self.previous->image_keys.end(),image->uniqueID());
        if (found!=self.previous->image_keys.end()) resource.pixels=self.previous->images[found-self.previous->image_keys.begin()].pixels;
      }
      if (!resource.pixels) {
        resource.pixels=SkData::MakeUninitialized(bytes);
        if (!image->readPixels(nullptr,SkImageInfo::MakeN32Premul(resource.width,resource.height),
            resource.pixels->writable_data(),resource.width*4,0,0)) self.valid=false;
      }
      self.scene.images.push_back(std::move(resource));
      self.scene.renderer_images.push_back(sk_ref_sp(image));
      self.scene.image_keys.push_back(image->uniqueID());
    }
    return SkData::MakeWithCopy(&entry->second,sizeof(uint32_t));
  }
  static SkSerialReturnType Font(SkTypeface* face,void* context) {
    auto& self=*static_cast<Capture*>(context);
    auto [entry,added]=self.fonts.emplace(face->uniqueID(),self.scene.fonts.size());
    if (added) {
      sk_sp<SkData> data;
      if (self.previous) {
        auto found=std::find(self.previous->font_keys.begin(),self.previous->font_keys.end(),face->uniqueID());
        if (found!=self.previous->font_keys.end()) data=self.previous->fonts[found-self.previous->font_keys.begin()];
      }
      if (!data) data=face->serialize(SkTypeface::SerializeBehavior::kDoIncludeData);
      if (!data || !self.Reserve(data->size())) { self.valid=false; return SkData::MakeEmpty(); }
      self.scene.font_keys.push_back(face->uniqueID());
      self.scene.fonts.push_back(std::move(data));
      self.scene.renderer_fonts.push_back(sk_ref_sp(face));
    }
    return SkData::MakeWithCopy(&entry->second,sizeof(uint32_t));
  }
};
}
std::shared_ptr<ScrollScene> CaptureScrollScene(blink::WebView& web_view,int* reason,const ScrollScene* previous) {
  *reason=10;
  auto* frame=static_cast<blink::WebLocalFrameImpl*>(web_view.MainFrame());
  auto* local=frame->GetFrame();
  auto* view=local->View();
  if (!view || !view->GetLayoutView()) return {};
  auto views=FrameViews(view);
  if (views.empty()) return {};
  bool blocking=false, listeners=false;
  for (auto* child:views) {
    auto& registry=child->GetFrame().GetEventHandlerRegistry();
    blocking|=registry.HasEventHandlers(blink::EventHandlerRegistry::kWheelEventBlocking);
    listeners|=registry.HasEventHandlers(blink::EventHandlerRegistry::kWheelEventPassive);
  }
  // Infinite culling records existing offscreen layout without forcing lazy content to load.
  blink::PaintController controller;
  blink::GraphicsContext context(controller);
  bool infinite=blink::RuntimeEnabledFeatures::InfiniteCullRectEnabled();
  blink::WebRuntimeFeatures::EnableFeatureFromString("InfiniteCullRect",true);
  for (auto* child:views) InvalidateCullRects(child->GetLayoutView()->Layer());
  view->PaintOutsideOfLifecycleWithThrottlingAllowed(context,blink::PaintFlag::kNoFlag,blink::CullRect::Infinite());
  blink::WebRuntimeFeatures::EnableFeatureFromString("InfiniteCullRect",infinite);
  for (auto* child:views) InvalidateCullRects(child->GetLayoutView()->Layer());
  const auto& artifact=controller.CommitNewDisplayItems();
  Capture capture; capture.view=view; capture.previous=previous;
  capture.scene.wheel_listeners=blocking || listeners;
  SkSerialProcs procedures; procedures.fImageProc=Capture::Image; procedures.fImageCtx=&capture;
  procedures.fTypefaceProc=Capture::Font; procedures.fTypefaceCtx=&capture;
  std::optional<blink::PaintChunkSubset> group;
  auto group_state=blink::PropertyTreeState::Root();
  ScrollPicture picture;
  auto flush=[&]() {
    if (!group) return;
    if (capture.scene.pictures.size()>=4096) { capture.valid=false; group.reset(); return; }
    auto record=blink::PaintChunksToCcLayer::Convert(*group,group_state);
    SkPictureRecorder recorder;
    record.Playback(recorder.beginRecording(picture.bounds));
    picture.data=recorder.finishRecordingAsPicture()->serialize(&procedures);
    if (!picture.data || !capture.Reserve(picture.data->size())) capture.valid=false;
    capture.scene.pictures.push_back(std::move(picture));
    group.reset();
  };
  auto rectangle=[](const gfx::Rect& rect) { return SkRect::MakeXYWH(rect.x(),rect.y(),rect.width(),rect.height()); };
  struct CustomPart { blink::CustomScrollbar* bar; blink::ScrollbarPart part; };
  std::map<blink::DisplayItemClientId,CustomPart> custom_parts;
  std::map<blink::CustomScrollbar*,unsigned> custom_bars;
  std::map<blink::CustomScrollbar*,blink::PaintLayerScrollableArea*> custom_areas;
  std::map<blink::CustomScrollbar*,const blink::PaintChunk*> custom_chunks;
  std::vector<blink::PaintLayerScrollableArea*> areas;
  for (auto* child:views) {
    for (const auto& entry:child->ScrollableAreas()) areas.push_back(entry.value.Get());
    if (std::find(areas.begin(),areas.end(),child->LayoutViewport())==areas.end()) areas.push_back(child->LayoutViewport());
  }
  constexpr blink::ScrollbarPart parts[]={blink::kScrollbarBGPart,blink::kBackButtonStartPart,blink::kBackButtonEndPart,
      blink::kForwardButtonStartPart,blink::kForwardButtonEndPart,blink::kTrackBGPart,blink::kBackTrackPart,blink::kForwardTrackPart,blink::kThumbPart};
  for (auto* area:areas) {
    if (!area) continue;
    for (auto* bar:{area->HorizontalScrollbar(),area->VerticalScrollbar()}) {
      if (!bar || !bar->IsCustomScrollbar()) continue;
      auto* custom=blink::To<blink::CustomScrollbar>(bar);
      custom_areas[custom]=area;
      for (auto part:parts) if (auto* object=custom->GetPart(part)) custom_parts.emplace(object->Id(),CustomPart{custom,part});
      for (const auto& chunk:artifact.GetPaintChunks())
        if (chunk.id.client_id==bar->Id() && chunk.hit_test_data) custom_chunks[custom]=&chunk;
    }
  }
  auto capture_bar=[&](blink::Scrollbar* bar,const blink::TransformPaintPropertyNode* translation,
      const gfx::Rect& frame,int transform,int clip,int effect,blink::ScrollableArea* area) -> bool {
    int scroll_transform=capture.Transform(translation);
    if (scroll_transform<0 && area && !bar->Enabled()) {
      ScrollTransform stationary; stationary.element=area->GetScrollElementId().GetInternalValue();
      scroll_transform=static_cast<int>(capture.scene.transforms.size()); capture.scene.transforms.push_back(stationary);
    }
    if (scroll_transform<0 || !capture.scene.transforms[scroll_transform].element) { *reason=17; return false; }
    auto& theme=bar->GetTheme();
    ScrollbarScene value; value.element=capture.scene.transforms[scroll_transform].element;
    value.transform=transform; value.clip=clip; value.frame=rectangle(frame);
    value.horizontal=bar->Orientation()==blink::kHorizontalScrollbar; value.jump=theme.JumpOnTrackClick();
    value.custom=bar->IsCustomScrollbar(); value.enabled=bar->Enabled();
    value.painted_hover=bar->HoveredPart(); value.painted_press=bar->PressedPart(); value.hover=value.painted_hover;
    value.initial_delay=std::max(1,static_cast<int>(theme.InitialAutoscrollTimerDelay().InMilliseconds()));
    value.repeat_delay=std::max(16,static_cast<int>(theme.AutoscrollTimerDelay().InMilliseconds()));
    auto track=theme.ConstrainTrackRectToTrackPieces(*bar,theme.TrackRect(*bar)), thumb=theme.ThumbRect(*bar);
    auto offset=frame.origin()-bar->FrameRect().origin();
    auto shifted=[&](gfx::Rect rect){rect.Offset(offset);return rectangle(rect);};
    value.track=shifted(track); value.thumb=shifted(thumb);
    auto* custom=bar->IsCustomScrollbar() ? blink::To<blink::CustomScrollbar>(bar) : nullptr;
    value.back_start=shifted(custom ? custom->ButtonRect(blink::kBackButtonStartPart) : theme.BackButtonRect(*bar));
    value.forward_end=shifted(custom ? custom->ButtonRect(blink::kForwardButtonEndPart) : theme.ForwardButtonRect(*bar));
    if (custom) {
      value.back_end=shifted(custom->ButtonRect(blink::kBackButtonEndPart));
      value.forward_start=shifted(custom->ButtonRect(blink::kForwardButtonStartPart));
    }
    ScrollTransform thumb_translation; thumb_translation.parent=transform;
    value.thumb_transform=static_cast<int>(capture.scene.transforms.size());
    capture.scene.transforms.push_back(thumb_translation);
    value.track_picture=static_cast<unsigned>(capture.scene.pictures.size());
    auto* proxy=blink::MakeGarbageCollected<ScrollbarPaintProxy>(area,*bar);
    auto* style_proxy=custom ? blink::MakeGarbageCollected<blink::CustomScrollbar>(area,bar->Orientation(),bar->StyleSource(),true) : nullptr;
    struct Detach {
      ScrollbarPaintProxy* proxy; blink::Scrollbar* style;
      ~Detach(){proxy->DisconnectFromScrollableArea(); if (style) style->DisconnectFromScrollableArea();}
    } detach{proxy,style_proxy};
    if (custom) {
      for (unsigned state=0;state<4;++state) {
        style_proxy->PrepareForSnapshot(*bar,state&1 ? blink::kThumbPart : blink::kNoPart,
            state&2 ? blink::kThumbPart : blink::kNoPart);
        value.thickness[state]=style_proxy->SnapshotThickness();
      }
      std::array<ScrollPicture,9> records;
      struct PaintedPart { const blink::ComputedStyle* style; gfx::Rect bounds; sk_sp<SkData> data; };
      std::array<std::vector<PaintedPart>,9> painted_parts;
      for (unsigned index=0;index<records.size();++index) {
        auto& part=records[index]; auto kind=parts[index];
        part.transform=kind==blink::kThumbPart ? value.thumb_transform : transform;
        part.clip=clip; part.effect=effect;
        part.scrollbar=static_cast<int>(capture.scene.scrollbars.size()); part.part=kind;
        part.track_piece=kind==blink::kBackTrackPart ? -1 : kind==blink::kForwardTrackPart ? 1 : 0;
      }
      constexpr blink::ScrollbarPart targets[]={blink::kNoPart,blink::kScrollbarBGPart,
          blink::kBackButtonStartPart,blink::kBackButtonEndPart,blink::kForwardButtonStartPart,
          blink::kForwardButtonEndPart,blink::kTrackBGPart,blink::kBackTrackPart,
          blink::kForwardTrackPart,blink::kThumbPart};
      for (auto hovered:targets) for (auto pressed:targets) {
        if (!bar->Enabled() && (hovered!=blink::kNoPart || pressed!=blink::kNoPart)) continue;
        style_proxy->PrepareForSnapshot(*bar,hovered,pressed);
        auto full_track=theme.TrackRect(*style_proxy);
        auto constrained=theme.ConstrainTrackRectToTrackPieces(*style_proxy,full_track);
        auto state_thumb=theme.ThumbRect(*style_proxy);
        bool buttons=theme.HasButtons(*style_proxy);
        ScrollbarLayout layout; layout.hover=hovered; layout.press=pressed;
        layout.track=shifted(constrained); layout.thumb=shifted(state_thumb);
        layout.back_start=shifted(buttons ? style_proxy->ButtonRect(blink::kBackButtonStartPart) : gfx::Rect());
        layout.back_end=shifted(buttons ? style_proxy->ButtonRect(blink::kBackButtonEndPart) : gfx::Rect());
        layout.forward_start=shifted(buttons ? style_proxy->ButtonRect(blink::kForwardButtonStartPart) : gfx::Rect());
        layout.forward_end=shifted(buttons ? style_proxy->ButtonRect(blink::kForwardButtonEndPart) : gfx::Rect());
        value.layouts.push_back(layout);
        for (unsigned index=0;index<records.size();++index) {
          auto& part=records[index]; auto kind=parts[index];
          gfx::Rect bounds;
          switch (kind) {
            case blink::kScrollbarBGPart: bounds=bar->FrameRect(); break;
            case blink::kTrackBGPart: bounds=full_track; break;
            case blink::kBackTrackPart: case blink::kForwardTrackPart: bounds=constrained; break;
            case blink::kThumbPart: bounds=state_thumb; break;
            default: bounds=buttons ? style_proxy->ButtonRect(kind) : gfx::Rect(); break;
          }
          auto* object=style_proxy->GetPart(kind);
          if (!object || ((kind==blink::kBackTrackPart || kind==blink::kForwardTrackPart) && !theme.HasThumb(*style_proxy))) bounds={};
          const auto* style=object ? &object->Style() : nullptr;
          auto& cache=painted_parts[index];
          auto cached=std::find_if(cache.begin(),cache.end(),[&](const auto& entry){
            return entry.bounds==bounds && (entry.style==style || (entry.style && style && *entry.style==*style));
          });
          if (cached!=cache.end()) {
            part.appearances.push_back(cached->data); part.appearance_bounds.push_back(shifted(bounds)); continue;
          }
          blink::PaintRecordBuilder builder;
          blink::PaintInfo info(builder.Context(),blink::CullRect::Infinite(),blink::PaintPhase::kForeground,false);
          if (object && !bounds.IsEmpty()) {
            object->SetOverriddenSize(blink::PhysicalSize(bounds.size()));
            object->GetMutableForPainting().FirstFragment().SetPaintOffset(blink::PhysicalOffset(bounds.origin()));
            blink::CustomScrollbarTheme::PaintIntoRect(*object,info,blink::PhysicalRect(bounds));
          }
          auto record=builder.EndRecording();
          auto picture_bounds=shifted(bounds);
          SkPictureRecorder recorder; auto* canvas=recorder.beginRecording(picture_bounds);
          canvas->clipRect(picture_bounds); canvas->translate(offset.x(),offset.y()); record.Playback(canvas);
          auto data=recorder.finishRecordingAsPicture()->serialize(&procedures);
          if (!data) return false;
          bool shared=false;
          for (const auto& existing:part.appearances) {
            if (existing->size()==data->size() && !std::memcmp(existing->data(),data->data(),data->size())) { data=existing; shared=true; break; }
          }
          if (!shared && !capture.Reserve(data->size())) return false;
          cache.push_back({style,bounds,data});
          part.appearances.push_back(std::move(data)); part.appearance_bounds.push_back(picture_bounds);
        }
      }
      for (auto& part:records) {
        part.bounds=part.appearance_bounds.front(); part.data=part.appearances.front();
        if (part.part==blink::kThumbPart) value.thumb_picture=static_cast<unsigned>(capture.scene.pictures.size());
        if (capture.scene.pictures.size()>=4096) return false;
        capture.scene.pictures.push_back(std::move(part));
      }
      value.last_picture=static_cast<unsigned>(capture.scene.pictures.size()-1);
      custom_bars[custom]=static_cast<unsigned>(capture.scene.scrollbars.size());
      capture.scene.scrollbars.push_back(std::move(value)); return true;
    }
    for (auto kind:parts) {
      if ((kind==blink::kBackTrackPart || kind==blink::kForwardTrackPart) && !theme.HasThumb(*bar)) continue;
      if (kind!=blink::kTrackBGPart && kind!=blink::kThumbPart &&
          kind!=blink::kBackButtonStartPart && kind!=blink::kForwardButtonEndPart) continue;
      gfx::Rect rectangle_bounds;
      switch (kind) {
        case blink::kScrollbarBGPart: rectangle_bounds=bar->FrameRect(); break;
        case blink::kTrackBGPart: rectangle_bounds=theme.TrackRect(*bar); break;
        case blink::kBackTrackPart: case blink::kForwardTrackPart: rectangle_bounds=track; break;
        case blink::kThumbPart: rectangle_bounds=thumb; break;
        default: rectangle_bounds=kind==blink::kBackButtonStartPart ? theme.BackButtonRect(*bar) : theme.ForwardButtonRect(*bar); break;
      }
      if (rectangle_bounds.IsEmpty()) continue;
      ScrollPicture part; part.transform=kind==blink::kThumbPart ? value.thumb_transform : transform;
      part.clip=clip; part.effect=effect; part.bounds=shifted(rectangle_bounds);
      part.scrollbar=static_cast<int>(capture.scene.scrollbars.size()); part.part=kind;
      part.track_piece=kind==blink::kBackTrackPart ? -1 : kind==blink::kForwardTrackPart ? 1 : 0;
      part.appearances.resize(8);
      for (unsigned appearance=0;appearance<8;++appearance) {
        // Aggregate backgrounds match hover/active on their contained controls in Blink's selector checker.
        proxy->State(kind==blink::kTrackBGPart ? blink::kThumbPart : kind,appearance);
        blink::PaintRecordBuilder builder;
        blink::PaintInfo info(builder.Context(),blink::CullRect::Infinite(),blink::PaintPhase::kForeground,false);
        if (kind==blink::kThumbPart) theme.PaintThumb(info,*proxy,rectangle_bounds);
        else theme.PaintTrackAndButtons(info,*proxy,bar->FrameRect());
        auto record=builder.EndRecording();
        SkPictureRecorder recorder; auto* canvas=recorder.beginRecording(part.bounds);
        canvas->clipRect(part.bounds); canvas->translate(offset.x(),offset.y()); record.Playback(canvas);
        auto data=recorder.finishRecordingAsPicture()->serialize(&procedures);
        if (!data) return false;
        for (unsigned previous=0;previous<appearance;++previous) {
          const auto& existing=part.appearances[previous];
          if (existing->size()==data->size() && !std::memcmp(existing->data(),data->data(),data->size())) { data=existing; break; }
        }
        bool shared=false; for (unsigned previous=0;previous<appearance;++previous) shared|=data==part.appearances[previous];
        if (!shared && !capture.Reserve(data->size())) return false;
        part.appearances[appearance]=std::move(data);
      }
      part.data=part.appearances[0];
      if (kind==blink::kThumbPart) value.thumb_picture=static_cast<unsigned>(capture.scene.pictures.size());
      if (capture.scene.pictures.size()>=4096) return false;
      capture.scene.pictures.push_back(std::move(part));
    }
    if (capture.scene.pictures.size()==value.track_picture) return false;
    value.last_picture=static_cast<unsigned>(capture.scene.pictures.size()-1);
    if (custom) custom_bars[custom]=static_cast<unsigned>(capture.scene.scrollbars.size());
    capture.scene.scrollbars.push_back(value); return true;
  };
  for (const auto& chunk:artifact.GetPaintChunks()) {
    if (chunk.hit_test_data) {
      for (const auto& rectangle:chunk.hit_test_data->wheel_event_rects) {
        if (capture.scene.wheel_regions.size()>=4096) { capture.scene.wheel_blocked=true; break; }
        capture.scene.wheel_regions.push_back({capture.Transform(&chunk.properties.Transform().Unalias()),
            capture.Clip(&chunk.properties.Clip().Unalias()),
            SkRect::MakeXYWH(rectangle.x(),rectangle.y(),rectangle.width(),rectangle.height())});
      }
    }
    if (chunk.begin_index==chunk.end_index) continue;
    int transform=capture.Transform(&chunk.properties.Transform().Unalias());
    int clip=capture.Clip(&chunk.properties.Clip().Unalias());
    int effect=capture.Effect(&chunk.properties.Effect().Unalias());
    if (!capture.valid || capture.scene.pictures.size()>=4096) return {};
    const auto& item=artifact.GetDisplayItemList()[chunk.begin_index];
    if (chunk.end_index==chunk.begin_index+1 && item.IsScrollbar()) {
      const auto& display=blink::To<blink::ScrollbarDisplayItem>(item);
      auto* translation=display.ScrollTranslation();
      auto* scroll=translation ? translation->ScrollNode() : nullptr;
      auto* area=scroll ? FindArea(view,scroll->GetCompositorElementId().GetInternalValue()) : nullptr;
      if (!area) for (auto* candidate:areas) if (candidate &&
          (display.ElementId()==candidate->GetScrollbarElementId(blink::kHorizontalScrollbar) ||
           display.ElementId()==candidate->GetScrollbarElementId(blink::kVerticalScrollbar))) { area=candidate; break; }
      auto* bar=area ? (display.ElementId()==area->GetScrollbarElementId(blink::kHorizontalScrollbar) ? area->HorizontalScrollbar() : area->VerticalScrollbar()) : nullptr;
      if (bar && !bar->IsCustomScrollbar()) {
        flush();
        if (!capture_bar(bar,translation,item.VisualRect(),transform,clip,effect,area)) return {};
        continue;
      }
    }
    bool custom_chunk=false;
    for (auto index=chunk.begin_index;index<chunk.end_index;++index)
      custom_chunk|=custom_parts.contains(artifact.GetDisplayItemList()[index].ClientId());
    if (custom_chunk) {
      flush();
      for (auto index=chunk.begin_index;index<chunk.end_index;++index) {
        const auto& drawing=artifact.GetDisplayItemList()[index];
        auto found=custom_parts.find(drawing.ClientId());
        if (found!=custom_parts.end()) {
          auto* bar=found->second.bar;
          auto anchor=custom_chunks.find(bar);
          if (anchor==custom_chunks.end() || anchor->second->properties!=chunk.properties) return {};
          if (custom_bars.contains(bar)) continue;
          auto* properties=custom_areas.at(bar)->GetLayoutBox()->FirstFragment().PaintProperties();
          if (!properties || !capture_bar(bar,properties->ScrollTranslation(),anchor->second->hit_test_data->scroll_hit_test_rect,transform,clip,effect,custom_areas.at(bar))) return {};
        } else {
          auto* record=blink::DynamicTo<blink::DrawingDisplayItem>(drawing);
          if (!record) return {};
          ScrollPicture part; part.transform=transform; part.clip=clip; part.effect=effect; part.bounds=rectangle(drawing.VisualRect());
          SkPictureRecorder recorder; record->GetPaintRecord().Playback(recorder.beginRecording(part.bounds));
          part.data=recorder.finishRecordingAsPicture()->serialize(&procedures);
          if (!part.data || !capture.Reserve(part.data->size()) || capture.scene.pictures.size()>=4096) return {};
          capture.scene.pictures.push_back(std::move(part));
        }
      }
      continue;
    }
    auto bounds=SkRect::MakeXYWH(chunk.bounds.x(),chunk.bounds.y(),chunk.bounds.width(),chunk.bounds.height());
    if (group && picture.transform==transform && picture.clip==clip && picture.effect==effect) {
      group->Merge(blink::PaintChunkSubset(artifact,chunk)); picture.bounds.join(bounds);
    } else {
      flush();
      if (!capture.valid) return {};
      picture={}; picture.transform=transform; picture.clip=clip; picture.effect=effect; picture.bounds=bounds;
      group.emplace(artifact,chunk); group_state=chunk.properties.Unalias();
    }
  }
  flush();
  if (!capture.valid) return {};
  if (capture.scene.pictures.empty()) return {};
  capture.scene.wheel_blocked|=blocking && capture.scene.wheel_regions.empty();
  // DOM identities survive layout-object replacement and scrollbar removal between snapshots.
  auto stable=[&](uint64_t native) { auto* area=FindArea(view,native); return area ? StableScrollId(area) : uint64_t{0}; };
  auto stable_element=[&](blink::CompositorElementId id) { uint64_t value=stable(id.GetInternalValue()); return value ? blink::CompositorElementId(value) : blink::CompositorElementId(); };
  for (auto& transform:capture.scene.transforms) {
    if (transform.element) transform.element=stable(transform.element);
    if (transform.scroll_parent) transform.scroll_parent=stable(transform.scroll_parent);
    if (transform.sticky) {
      transform.constraint.x_scroll_ancestor_element_id=stable_element(transform.constraint.x_scroll_ancestor_element_id);
      transform.constraint.y_scroll_ancestor_element_id=stable_element(transform.constraint.y_scroll_ancestor_element_id);
    }
  }
  for (auto& bar:capture.scene.scrollbars) bar.element=stable(bar.element);
  *reason=0;
  return std::make_shared<ScrollScene>(std::move(capture.scene));
}
void ApplyScrollOffset(blink::WebView& web_view,uint64_t element,int x,int y,bool synchronize_queries) {
  auto* view=static_cast<blink::WebLocalFrameImpl*>(web_view.MainFrame())->GetFrame()->View();
  auto* area=FindArea(view,element);
  if (!area) return;
  if (!synchronize_queries) {
    area->SetScrollOffset(blink::ScrollOffset(x,y),blink::mojom::blink::ScrollType::kCompositor,cc::ScrollSourceType::kRelativeScroll);
    return;
  }
  auto* layer=blink::DynamicTo<blink::PaintLayerScrollableArea>(area);
  auto* owner=layer && layer->GetLayoutBox() ? blink::DynamicTo<blink::Element>(layer->GetLayoutBox()->GetNode()) : nullptr;
  auto* evaluator=owner ? owner->GetContainerQueryEvaluator() : nullptr;
  blink::Persistent<blink::ScrollStateQuerySnapshot> query(evaluator && evaluator->DependsOnScrollable() ? evaluator->UefiScrollStateSnapshot() : nullptr);
  if (query) {
    // Resolve query-dependent ranges before clamping a coordinate produced by the BSP scene.
    for (unsigned pass=0;pass<4 && area;++pass) {
      auto minimum=area->MinimumScrollOffset(),maximum=area->MaximumScrollOffset();
      unsigned horizontal=(x>minimum.x() ? 1 : 0)|(x<maximum.x() ? 2 : 0);
      unsigned vertical=(y>minimum.y() ? 1 : 0)|(y<maximum.y() ? 2 : 0);
      query->OverrideForUefi(horizontal,vertical);
      view->UpdateLifecycleToLayoutClean(blink::DocumentUpdateReason::kInput);
      area=FindArea(view,element);
      if (area && minimum==area->MinimumScrollOffset() && maximum==area->MaximumScrollOffset()) break;
    }
  }
  if (area) area->SetScrollOffset(blink::ScrollOffset(x,y),
      blink::mojom::blink::ScrollType::kCompositor,cc::ScrollSourceType::kRelativeScroll);
  if (query) query->OverrideForUefi(-1,-1);
  // Sticky query descendants can also change the range after the first offset is applied.
  for (unsigned pass=0;pass<4;++pass) {
    view->UpdateLifecycleToLayoutClean(blink::DocumentUpdateReason::kInput);
    bool changed=false;
    for (auto* child:FrameViews(view)) {
      for (auto* object=static_cast<blink::LayoutObject*>(child->GetLayoutView());object;object=object->NextInPreOrder()) {
        auto* element=blink::DynamicTo<blink::Element>(object->GetNode());
        auto* evaluator=element ? element->GetContainerQueryEvaluator() : nullptr;
        if (evaluator && evaluator->UefiScrollStateSnapshot()) changed|=evaluator->UefiScrollStateSnapshot()->UpdateSnapshot();
      }
    }
    if (!changed) break;
    view->UpdateLifecycleToLayoutClean(blink::DocumentUpdateReason::kInput);
    if (auto* accepted=FindArea(view,element)) accepted->SetScrollOffset(blink::ScrollOffset(x,y),
        blink::mojom::blink::ScrollType::kCompositor,cc::ScrollSourceType::kRelativeScroll);
  }
}
void ApplyScrollbarState(blink::WebView& web_view,uint64_t element,bool horizontal,unsigned hover,unsigned press) {
  auto* frame=static_cast<blink::WebLocalFrameImpl*>(web_view.MainFrame())->GetFrame();
  auto* view=frame->View();
  if (!view) return;
  auto* area=FindArea(view,element);
  auto* bar=area ? (horizontal ? area->HorizontalScrollbar() : area->VerticalScrollbar()) : nullptr;
  if (!bar || !bar->IsCustomScrollbar()) return;
  blink::To<blink::CustomScrollbar>(bar)->ApplyCompositorState(
      static_cast<blink::ScrollbarPart>(hover),static_cast<blink::ScrollbarPart>(press));
  view->UpdateLifecycleToLayoutClean(blink::DocumentUpdateReason::kInput);
  // The BSP keeps absolute offsets during gutter switches; deferred anchoring must agree.
  for (auto* child:FrameViews(view)) {
    for (const auto& entry:child->ScrollableAreas()) entry.value->GetScrollAnchor()->CancelAdjustment();
    if (child->LayoutViewport()) child->LayoutViewport()->GetScrollAnchor()->CancelAdjustment();
  }
}

void CaptureScrollbarReflows(blink::WebView& web_view,ScrollScene& scene) {
  auto* view=static_cast<blink::WebLocalFrameImpl*>(web_view.MainFrame())->GetFrame()->View();
  auto views=FrameViews(view);
  if (views.empty()) return;
  struct QueryCacheGuard {
    ScrollScene& scene;
    bool dependent=false, complete=false;
    ~QueryCacheGuard() { if (dependent && !complete) scene={}; }
  } query_guard{scene};
  for (auto* child:views)
    for (auto* object=static_cast<blink::LayoutObject*>(child->GetLayoutView());object;object=object->NextInPreOrder())
      if (object->HasStyle()) query_guard.dependent|=object->Style().IsContainerForScrollStateContainerQueries() || object->Style().IsContainerForAnchoredContainerQueries();
  struct Candidate {
    blink::Persistent<blink::CustomScrollbar> bar;
    blink::Persistent<blink::Node> owner;
    std::vector<int> sizes;
    uint64_t element;
    bool horizontal;
    unsigned hover, press;
  };
  std::vector<Candidate> candidates;
  size_t combinations=1;
  for (const auto& bar:scene.scrollbars) {
    if (!bar.custom || !bar.enabled) continue;
    std::set<int> sizes(bar.thickness.begin(),bar.thickness.end());
    if (sizes.size()<2) continue;
    // A disappearing scrollbar can destroy its layout object during the speculative pass.
    if (*sizes.begin()<=0 || *sizes.rbegin()>256 || combinations>64/sizes.size()) return;
    auto* area=FindArea(view,bar.element);
    if (!area) return;
    auto* source=bar.horizontal ? area->HorizontalScrollbar() : area->VerticalScrollbar();
    if (!source || !source->IsCustomScrollbar()) return;
    candidates.push_back({blink::Persistent<blink::CustomScrollbar>(blink::To<blink::CustomScrollbar>(source)),
        blink::Persistent<blink::Node>(blink::To<blink::CustomScrollbar>(source)->GetLayoutBox()->GetNode()),
        {sizes.begin(),sizes.end()},bar.element,bar.horizontal,bar.painted_hover,bar.painted_press});
    combinations*=sizes.size();
  }
  struct Query {
    blink::Persistent<blink::Element> owner;
    blink::Persistent<blink::ScrollStateQuerySnapshot> snapshot;
    uint64_t element;
    bool stuck;
  };
  std::vector<Query> queries;
  for (auto* child:views) {
    for (auto* object=static_cast<blink::LayoutObject*>(child->GetLayoutView());object;object=object->NextInPreOrder()) {
      if (object->HasStyle() && object->Style().IsContainerForAnchoredContainerQueries()) return;
      auto* owner=blink::DynamicTo<blink::Element>(object->GetNode());
      auto* evaluator=owner ? owner->GetContainerQueryEvaluator() : nullptr;
      if (!evaluator) continue;
      if (evaluator->DependsOnSnapped() || evaluator->DependsOnScrolled() ||
          (evaluator->DependsOnStuck() && evaluator->DependsOnScrollable())) { scene={}; return; }
      if (!evaluator->DependsOnScrollable() && !evaluator->DependsOnStuck()) continue;
      bool stuck=evaluator->DependsOnStuck();
      auto* snapshot=evaluator->UefiScrollStateSnapshot();
      auto* area=owner->GetLayoutBox() ? owner->GetLayoutBox()->GetScrollableArea() : nullptr;
      if (owner==child->GetFrame().GetDocument()->documentElement()) area=child->LayoutViewport();
      unsigned states=stuck ? 9 : 16;
      if (!snapshot || (!stuck && !area) || combinations>64/states) { scene={}; return; }
      uint64_t element=stuck ? (uint64_t{1}<<63)|static_cast<uint64_t>(owner->GetDomNodeId()) : StableScrollId(area);
      if (std::any_of(queries.begin(),queries.end(),[&](const auto& query){return query.element==element;})) continue;
      queries.push_back({blink::Persistent<blink::Element>(owner),blink::Persistent<blink::ScrollStateQuerySnapshot>(snapshot),element,stuck});
      combinations*=states;
    }
  }
  if (candidates.empty() && queries.empty()) return;
  auto condition=[&](const Query& query,unsigned horizontal,unsigned vertical) {
    uint64_t element=query.element;
    if (query.stuck) {
      auto* object=query.owner->GetLayoutObject();
      auto* properties=object ? object->FirstFragment().PaintProperties() : nullptr;
      auto* translation=properties ? properties->StickyTranslation() : nullptr;
      element=translation ? translation->GetCompositorElementId().GetInternalValue() : 0;
    }
    return ScrollQuery{element,horizontal,vertical,query.stuck};
  };
  for (const auto& query:queries) scene.queries.push_back(condition(query,
      query.stuck ? static_cast<unsigned>(query.snapshot->StuckHorizontal()) : static_cast<unsigned>(query.snapshot->ScrollableHorizontal()),
      query.stuck ? static_cast<unsigned>(query.snapshot->StuckVertical()) : static_cast<unsigned>(query.snapshot->ScrollableVertical())));
  struct SavedOffset { blink::Persistent<blink::PaintLayerScrollableArea> area; blink::Persistent<blink::Node> owner; blink::ScrollOffset offset; };
  std::vector<SavedOffset> offsets;
  for (auto* child:views) {
    for (const auto& entry:child->ScrollableAreas()) offsets.push_back({blink::Persistent<blink::PaintLayerScrollableArea>(entry.value.Get()),blink::Persistent<blink::Node>(entry.value->GetLayoutBox()->GetNode()),entry.value->GetScrollOffset()});
    if (child->LayoutViewport()) offsets.push_back({blink::Persistent<blink::PaintLayerScrollableArea>(child->LayoutViewport()),blink::Persistent<blink::Node>(child->LayoutViewport()->GetLayoutBox()->GetNode()),child->LayoutViewport()->GetScrollOffset()});
  }
  blink::ScriptForbiddenScope forbid_script;
  for (auto* child:views) child->scrollbar_snapshot_mode=true;
  bool valid=true;
  size_t bytes=candidates.size()*sizeof(ScrollGutter);
  std::set<const SkData*> allocations;
  auto reserve=[&](const ScrollScene& variant) {
    bytes+=sizeof(ScrollScene)+variant.gutters.size()*sizeof(ScrollGutter)+variant.queries.size()*sizeof(ScrollQuery)+variant.transforms.size()*sizeof(ScrollTransform)+
        variant.clips.size()*sizeof(ScrollClip)+variant.effects.size()*sizeof(ScrollEffect)+
        variant.pictures.size()*sizeof(ScrollPicture)+variant.scrollbars.size()*sizeof(ScrollbarScene);
    for (const auto& bar:variant.scrollbars) bytes+=bar.layouts.size()*sizeof(ScrollbarLayout);
    for (const auto& picture:variant.pictures)
      bytes+=picture.appearances.size()*sizeof(sk_sp<SkData>)+picture.appearance_bounds.size()*sizeof(SkRect);
    auto add=[&](const sk_sp<SkData>& data) { if (data && allocations.insert(data.get()).second) bytes+=data->size(); };
    for (const auto& picture:variant.pictures) { add(picture.data); for (const auto& appearance:picture.appearances) add(appearance); }
    for (const auto& image:variant.images) add(image.pixels);
    for (const auto& font:variant.fonts) add(font);
    return bytes<=16*1024*1024;
  };
  if (!reserve(scene)) {
    for (auto* child:views) child->scrollbar_snapshot_mode=false;
    if (!queries.empty()) scene={};
    return;
  }
  for (size_t index=0;index<combinations && valid;++index) {
    size_t choice=index;
    std::vector<int> thicknesses;
    for (const auto& candidate:candidates) {
      int size=candidate.sizes[choice%candidate.sizes.size()]; choice/=candidate.sizes.size();
      thicknesses.push_back(size);
    }
    std::vector<ScrollQuery> query_states;
    for (const auto& query:queries) {
      unsigned count=query.stuck ? 3 : 4;
      unsigned horizontal=choice%count; choice/=count;
      unsigned vertical=choice%count; choice/=count;
      if (query.stuck && vertical) vertical+=2;
      if (query.stuck) query.snapshot->OverrideStuckForUefi(horizontal,vertical);
      else query.snapshot->OverrideForUefi(horizontal,vertical);
      query_states.push_back(condition(query,horizontal,vertical));
    }
    // Capture every tuple, including the current one, so selection never depends on paint state.
    for (size_t item=0;item<candidates.size();++item) candidates[item].bar->OverrideSnapshotThickness(thicknesses[item]);
    valid=view->UpdateScrollbarSnapshotLayout();
    for (const auto& candidate:candidates) valid&=candidate.bar->GetLayoutBox()!=nullptr;
    if (!valid) break;
    int reason=0;
    auto variant=CaptureScrollScene(web_view,&reason,&scene);
    if (!variant) { valid=false; break; }
    for (size_t item=0;item<queries.size();++item) {
      const auto& state=query_states[item];
      variant->queries.push_back(condition(queries[item],state.horizontal,state.vertical));
    }
    // Only driving scrollbars must remain stable; dependent scrollbars may appear or disappear.
    for (size_t item=0;item<candidates.size();++item) {
      const auto& candidate=candidates[item];
      auto found=std::find_if(variant->scrollbars.begin(),variant->scrollbars.end(),[&](const auto& bar) {
        return bar.element==candidate.element && bar.horizontal==candidate.horizontal;
      });
      auto original=std::find_if(scene.scrollbars.begin(),scene.scrollbars.end(),[&](const auto& bar) {
        return bar.element==candidate.element && bar.horizontal==candidate.horizontal;
      });
      if (found==variant->scrollbars.end() || found->thickness!=original->thickness) { valid=false; break; }
      variant->gutters.push_back({candidate.element,candidate.horizontal,thicknesses[item],original->thickness});
    }
    if (!valid || !reserve(*variant)) { valid=false; break; }
    scene.reflows.push_back(std::move(variant));
  }
  for (const auto& candidate:candidates) candidate.bar->OverrideSnapshotThickness(-1);
  // A container query can remove a scrollbar; discard that cache and restore the live tree.
  if (!valid) view->SetNeedsLayout();
  valid=view->UpdateScrollbarSnapshotLayout() && valid;
  for (const auto& candidate:candidates) {
    auto* area=FindArea(view,candidate.element);
    if (!area && candidate.owner && candidate.owner->GetLayoutBox()) area=candidate.owner->GetLayoutBox()->GetScrollableArea();
    auto* bar=area ? (candidate.horizontal ? area->HorizontalScrollbar() : area->VerticalScrollbar()) : nullptr;
    if (bar && bar->IsCustomScrollbar() && (bar->HoveredPart()!=candidate.hover || bar->PressedPart()!=candidate.press))
      blink::To<blink::CustomScrollbar>(bar)->ApplyCompositorState(
          static_cast<blink::ScrollbarPart>(candidate.hover),static_cast<blink::ScrollbarPart>(candidate.press));
  }
  valid=view->UpdateScrollbarSnapshotLayout() && valid;
  for (const auto& saved:offsets) {
    auto* area=saved.area->GetLayoutBox() ? saved.area.Get() :
        (saved.owner && saved.owner->GetLayoutBox() ? saved.owner->GetLayoutBox()->GetScrollableArea() : nullptr);
    if (area) area->SetScrollOffset(saved.offset,blink::mojom::blink::ScrollType::kClamping,cc::ScrollSourceType::kStationaryScroll);
  }
  for (const auto& query:queries) {
    if (query.stuck) query.snapshot->OverrideStuckForUefi(-1,-1);
    else query.snapshot->OverrideForUefi(-1,-1);
  }
  bool restored_layout=view->UpdateScrollbarSnapshotLayout();
  valid=restored_layout && valid;
  if (!valid) {
    // Recreated layout objects have new scroll IDs; even the restored base scene must be refreshed.
    int reason=0;
    auto restored=restored_layout ? CaptureScrollScene(web_view,&reason,&scene) : nullptr;
    scene=restored && queries.empty() ? std::move(*restored) : ScrollScene{};
  }
  if (valid) {
    for (const auto& candidate:candidates) {
      auto original=std::find_if(scene.scrollbars.begin(),scene.scrollbars.end(),[&](const auto& bar) {
        return bar.element==candidate.element && bar.horizontal==candidate.horizontal;
      });
      scene.gutters.push_back({candidate.element,candidate.horizontal,
          static_cast<int>(candidate.horizontal ? original->frame.height() : original->frame.width()),original->thickness});
    }
  }
  query_guard.complete=valid;
  for (auto* child:views) child->scrollbar_snapshot_mode=false;
  std::printf("COMPOSITOR: gutter snapshots=%zu bytes=%zu\n",scene.reflows.size(),bytes);
}

uint64_t KeyboardScrollTarget(blink::WebView& web_view) {
  if (web_view.FocusedFrame() && web_view.FocusedFrame()!=web_view.MainFrame()) return 0;
  auto* frame=static_cast<blink::WebLocalFrameImpl*>(web_view.MainFrame())->GetFrame();
  auto* document=frame ? frame->GetDocument() : nullptr;
  if (!document || !frame->View()) return 0;
  auto handlers=[](blink::EventTarget* target) {
    return target && (target->HasEventListeners(blink::event_type_names::kKeydown) ||
        target->HasEventListeners(blink::event_type_names::kKeypress) ||
        target->HasEventListeners(blink::event_type_names::kKeyup));
  };
  if (handlers(document->domWindow()) || handlers(document)) return 0;
  auto* focused=document->FocusedElement();
  if (focused && (blink::IsEditable(*focused) || focused->HasActivationBehavior() || blink::IsA<blink::HTMLFormControlElement>(focused))) return 0;
  blink::ScrollableArea* area=frame->View()->LayoutViewport();
  if (focused && focused!=document->body() && focused!=document->documentElement()) {
    auto* box=focused->GetLayoutBox();
    // Controls and links retain their keyboard defaults; only focused scroll containers are eligible.
    if (!box || !box->GetScrollableArea() || focused->IsTextControl()) return 0;
    area=box->GetScrollableArea();
  }
  for (blink::Node* node=focused ? focused : document->body();node;node=node->ParentOrShadowHostNode())
    if (handlers(node)) return 0;
  return area ? StableScrollId(area) : 0;
}
SkRect ScrollCompositor::Global(SkRect bounds,int transform) const {
  if (transform>=0) bounds.offset(offsets_[transform].x(),offsets_[transform].y());
  return bounds;
}
bool ScrollCompositor::WheelAllowed(int x,int y) const {
  if (scene_.wheel_blocked) return false;
  for (const auto& region:scene_.wheel_regions)
    if (Contains(region.clip,x,y) && Global(region.bounds,region.transform).contains(x,y)) return false;
  return true;
}
uint64_t ScrollCompositor::Move(uint64_t element,int value,bool absolute,bool chain,ScrollPosition* position,bool horizontal) {
  uint64_t boundary_element=0; ScrollPosition boundary_position;
  for (unsigned depth=0;element && depth<scene_.transforms.size();++depth) {
    auto found=std::find_if(scene_.transforms.begin(),scene_.transforms.end(),[&](const auto& node){return node.element==element;});
    if (found==scene_.transforms.end()) return 0;
    int& current=horizontal ? found->position_x : found->position;
    int minimum=horizontal ? found->minimum_x : found->minimum;
    int maximum=horizontal ? found->maximum_x : found->maximum;
    bool allowed=horizontal ? found->horizontal : found->vertical;
    bool propagate=horizontal ? found->chain_x : found->chain;
    if (allowed) { boundary_element=element; boundary_position={found->position_x,found->position}; }
    int next=allowed ? std::clamp(absolute ? value : current+value,minimum,maximum) : current;
    if (allowed && (next!=current || !chain || !propagate)) {
      int previous=current;
      current=next; UpdateTransforms(); SelectReflow();
      if (!reflow_valid_) {
        current=previous; UpdateTransforms();
        return 0;
      }
      auto accepted=std::find_if(scene_.transforms.begin(),scene_.transforms.end(),[&](const auto& node){return node.element==element;});
      *position=accepted==scene_.transforms.end() ? ScrollPosition{} : ScrollPosition{accepted->position_x,accepted->position};
      return element;
    }
    if (!chain || !propagate) return 0;
    element=found->scroll_parent;
  }
  // A consumed boundary wheel must not become a delayed relative event after newer absolute offsets.
  if (boundary_element) *position=boundary_position;
  return boundary_element;
}
uint64_t ScrollCompositor::Keyboard(uint64_t element,unsigned scan,bool space,bool shift,ScrollPosition* position) {
  auto found=std::find_if(scene_.transforms.begin(),scene_.transforms.end(),[&](const auto& node){return element && node.element==element;});
  if (found==scene_.transforms.end()) return 0;
  int page=std::max(1,found->viewport*7/8);
  if (space) return Move(element,shift ? -page : page,false,true,position);
  switch (scan) {
    case 1: return Move(element,-40,false,true,position);
    case 2: return Move(element,40,false,true,position);
    case 3: return Move(element,40,false,true,position,true);
    case 4: return Move(element,-40,false,true,position,true);
    case 5: return Move(element,found->minimum,true,false,position);
    case 6: return Move(element,found->maximum,true,false,position);
    case 9: return Move(element,-page,false,true,position);
    case 10: return Move(element,page,false,true,position);
    default: return 0;
  }
}
uint64_t ScrollCompositor::Repeat(ScrollPosition* position) {
  auto bar=std::find_if(scene_.scrollbars.begin(),scene_.scrollbars.end(),[&](const auto& item){return item.element==drag_element_ && item.horizontal==drag_horizontal_;});
  if (bar==scene_.scrollbars.end()) { CancelDrag(); return 0; }
  auto scroll=std::find_if(scene_.transforms.begin(),scene_.transforms.end(),[&](const auto& node){return node.element==drag_element_;});
  if (scroll==scene_.transforms.end()) { CancelDrag(); return 0; }
  auto inside=[&](SkRect rect){return Global(rect,bar->transform).contains(pointer_x_,pointer_y_);};
  if (!Contains(bar->clip,pointer_x_,pointer_y_)) return 0;
  bool backwards=drag_part_<0;
  int distance=40;
  if (std::abs(drag_part_)==2) {
    auto thumb=Global(bar->thumb,bar->thumb_transform);
    float coordinate=drag_horizontal_ ? pointer_x_ : pointer_y_;
    if (!inside(bar->track) || (backwards ? coordinate>=(drag_horizontal_ ? thumb.left() : thumb.top()) :
        coordinate<(drag_horizontal_ ? thumb.right() : thumb.bottom()))) return 0;
    distance=std::max(1,(drag_horizontal_ ? scroll->viewport_x : scroll->viewport)*7/8);
  } else if (backwards ? !(inside(bar->back_start) || inside(bar->back_end)) :
                        !(inside(bar->forward_start) || inside(bar->forward_end))) return 0;
  int before=drag_horizontal_ ? scroll->position_x : scroll->position;
  uint64_t element=Move(drag_element_,backwards ? -distance : distance,false,false,position,drag_horizontal_);
  return element && before!=(drag_horizontal_ ? position->x : position->y) ? element : 0;
}
bool ScrollCompositor::Tick(int64_t now,uint64_t* element,ScrollPosition* position) {
  if (!drag_element_ || !drag_part_ || !pressed_part_) return false;
  auto bar=std::find_if(scene_.scrollbars.begin(),scene_.scrollbars.end(),[&](const auto& item){return item.element==drag_element_ && item.horizontal==drag_horizontal_;});
  if (bar==scene_.scrollbars.end()) { CancelDrag(); return false; }
  if (!repeat_at_) { repeat_at_=now+bar->initial_delay; return false; }
  if (now<repeat_at_) return false;
  // Missed deadlines produce one step rather than an input burst after a slow raster.
  repeat_at_=now+bar->repeat_delay;
  *element=Repeat(position); return *element!=0;
}
unsigned ScrollCompositor::HitPart(const ScrollbarScene& bar,int x,int y) const {
  if (!bar.enabled) return blink::kNoPart;
  if (!Contains(bar.clip,x,y) || !Global(bar.frame,bar.transform).contains(x,y)) return blink::kNoPart;
  int hit=-1;
  for (int index=static_cast<int>(scene_.pictures.size())-1;index>=0;--index) {
    const auto& picture=scene_.pictures[index];
    if (Contains(picture.clip,x,y) && Global(Bounds(picture),picture.transform).contains(x,y)) { hit=index; break; }
  }
  if (hit<static_cast<int>(bar.track_picture) || hit>static_cast<int>(bar.last_picture)) return blink::kNoPart;
  if (Global(bar.thumb,bar.thumb_transform).contains(x,y)) return blink::kThumbPart;
  auto inside=[&](SkRect bounds){return Global(bounds,bar.transform).contains(x,y);};
  if (inside(bar.back_start)) return blink::kBackButtonStartPart;
  if (inside(bar.back_end)) return blink::kBackButtonEndPart;
  if (inside(bar.forward_start)) return blink::kForwardButtonStartPart;
  if (inside(bar.forward_end)) return blink::kForwardButtonEndPart;
  if (inside(bar.track)) {
    auto thumb=Global(bar.thumb,bar.thumb_transform);
    return (bar.horizontal ? x<thumb.left() : y<thumb.top()) ? blink::kBackTrackPart : blink::kForwardTrackPart;
  }
  return blink::kScrollbarBGPart;
}
unsigned ScrollCompositor::Appearance(const ScrollPicture& picture) const {
  if (picture.scrollbar<0) return 0;
  const auto& bar=scene_.scrollbars[picture.scrollbar];
  if (!bar.layouts.empty()) return bar.layout;
  unsigned hovered=HitPart(bar,pointer_x_,pointer_y_);
  unsigned pressed=drag_element_==bar.element && drag_horizontal_==bar.horizontal ? pressed_part_ : 0;
  auto matches=[&](unsigned part) {
    if (picture.part==blink::kScrollbarBGPart) return part!=blink::kNoPart;
    if (picture.part==blink::kTrackBGPart) return part==blink::kThumbPart || part==blink::kBackTrackPart || part==blink::kForwardTrackPart;
    return part==picture.part;
  };
  bool disabled=!bar.enabled;
  auto scroll=std::find_if(scene_.transforms.begin(),scene_.transforms.end(),[&](const auto& node){return node.element==bar.element;});
  if (scroll!=scene_.transforms.end()) {
    int minimum=bar.horizontal ? scroll->minimum_x : scroll->minimum;
    int maximum=bar.horizontal ? scroll->maximum_x : scroll->maximum;
    int position=bar.horizontal ? scroll->position_x : scroll->position;
    disabled|=minimum==maximum;
    if (!bar.custom) {
      if (picture.part==blink::kBackButtonStartPart) disabled|=position<=minimum;
      if (picture.part==blink::kForwardButtonEndPart) disabled|=position>=maximum;
    }
  }
  return (matches(hovered) ? 1 : 0) | (matches(pressed) ? 2 : 0) | (disabled ? 4 : 0);
}
bool ScrollCompositor::AppearanceChanged() const {
  if (selected_reflow_!=painted_reflow_) return true;
  for (const auto& bar:scene_.scrollbars) {
    unsigned pressed=drag_element_==bar.element && drag_horizontal_==bar.horizontal ? pressed_part_ : 0;
    if ((bar.layouts.empty() ? HitPart(bar,pointer_x_,pointer_y_) : bar.hover)!=bar.painted_hover || pressed!=bar.painted_press) return true;
  }
  return false;
}
bool ScrollCompositor::Hover(int x,int y) {
  std::vector<unsigned> before;
  for (const auto& picture:scene_.pictures) before.push_back(Appearance(picture));
  pointer_x_=x; pointer_y_=y;
  for (auto& bar:scene_.scrollbars) bar.hover=HitPart(bar,x,y);
  UpdateLayouts();
  if (SelectReflow()) return true;
  for (unsigned index=0;index<scene_.pictures.size();++index)
    if (before[index]!=Appearance(scene_.pictures[index])) return true;
  return false;
}
bool ScrollCompositor::Pointer(int x,int y,unsigned buttons,uint64_t* element,ScrollPosition* position) {
  pointer_x_=x; pointer_y_=y;
  if (drag_element_) {
    if (!(buttons&1)) { CancelDrag(); return true; }
    if (drag_part_) return true;
    int coordinate=drag_horizontal_ ? x : y;
    if (coordinate==drag_coordinate_) return true;
    drag_coordinate_=coordinate;
    auto found=std::find_if(scene_.scrollbars.begin(),scene_.scrollbars.end(),[&](const auto& bar){return bar.element==drag_element_ && bar.horizontal==drag_horizontal_;});
    if (found==scene_.scrollbars.end()) { CancelDrag(); return true; }
    auto scroll=std::find_if(scene_.transforms.begin(),scene_.transforms.end(),[&](const auto& node){return node.element==drag_element_;});
    auto track=Global(found->track,found->transform);
    float travel=drag_horizontal_ ? track.width()-found->thumb.width() : track.height()-found->thumb.height();
    if (scroll!=scene_.transforms.end() && travel>0) {
      float fraction=std::clamp(((drag_horizontal_ ? x-track.left() : y-track.top())-drag_offset_)/travel,0.0f,1.0f);
      int minimum=drag_horizontal_ ? scroll->minimum_x : scroll->minimum;
      int maximum=drag_horizontal_ ? scroll->maximum_x : scroll->maximum;
      *element=Move(drag_element_,minimum+static_cast<int>(std::lround(fraction*(maximum-minimum))),true,false,position,drag_horizontal_);
    }
    return true;
  }
  if (!(buttons&1)) return false;
  int hit=-1;
  for (int index=static_cast<int>(scene_.pictures.size())-1;index>=0;--index) {
    const auto& picture=scene_.pictures[index];
    if (Contains(picture.clip,x,y) && Global(Bounds(picture),picture.transform).contains(x,y)) { hit=index; break; }
  }
  for (auto iterator=scene_.scrollbars.rbegin();iterator!=scene_.scrollbars.rend();++iterator) {
    const auto& bar=*iterator;
    if (hit<static_cast<int>(bar.track_picture) || hit>static_cast<int>(bar.last_picture)) continue;
    if (!Contains(bar.clip,x,y) || !Global(bar.frame,bar.transform).contains(x,y)) continue;
    if (!bar.enabled) {
      drag_element_=bar.element; drag_horizontal_=bar.horizontal; drag_part_=5; pressed_part_=0;
      return true;
    }
    auto thumb=Global(bar.thumb,bar.thumb_transform);
    auto inside=[&](SkRect rect){return Global(rect,bar.transform).contains(x,y);};
    int part=0;
    if (!thumb.contains(x,y)) {
      if (inside(bar.back_start) || inside(bar.back_end)) part=-1;
      else if (inside(bar.forward_start) || inside(bar.forward_end)) part=1;
      else if (inside(bar.track)) part=(bar.horizontal ? x<thumb.left() : y<thumb.top()) ? -2 : 2;
      else return false;
    }
    drag_element_=bar.element; drag_horizontal_=bar.horizontal; drag_part_=part; repeat_at_=0;
    pressed_part_=HitPart(bar,x,y);
    UpdateLayouts();
    thumb=Global(bar.thumb,bar.thumb_transform);
    drag_offset_=bar.horizontal ? x-thumb.left() : y-thumb.top();
    drag_coordinate_=bar.horizontal ? x : y;
    if (part && std::abs(part)==2 && bar.jump) {
      drag_part_=0; drag_offset_=(bar.horizontal ? thumb.width() : thumb.height())/2;
      drag_coordinate_=bar.horizontal ? x-1 : y-1;
      return Pointer(x,y,buttons,element,position);
    }
    if (part) *element=Repeat(position);
    return true;
  }
  return false;
}

void ScrollCompositor::SwapScene(ScrollCompositor& other) {
  std::swap(scene_,other.scene_); pictures_.swap(other.pictures_); appearances_.swap(other.appearances_);
  images_.swap(other.images_); fonts_.swap(other.fonts_); offsets_.swap(other.offsets_); tiles_.swap(other.tiles_);
}
bool ScrollCompositor::Install(const ScrollScene& source) {
  reflows_.clear(); reflow_states_.clear(); retained_positions_.clear(); selected_reflow_=painted_reflow_=0;
  if (!InstallScene(source)) return false;
  for (const auto& variant:source.reflows) {
    auto compositor=std::make_unique<ScrollCompositor>();
    compositor->scene_.image_keys=scene_.image_keys; compositor->scene_.font_keys=scene_.font_keys;
    compositor->images_=images_; compositor->fonts_=fonts_;
    // BSP-owned immutable resources are shared across layouts with the same resource keys.
    if (!compositor->InstallScene(*variant)) { reflows_.clear(); return true; }
    compositor->selected_reflow_=static_cast<unsigned>(reflows_.size())+1;
    reflows_.push_back(std::move(compositor));
  }
  SelectReflow();
  return true;
}
std::vector<ScrollbarState> ScrollCompositor::TakeReflowStates() {
  SelectReflow();
  return std::exchange(reflow_states_,{});
}
bool ScrollCompositor::SelectReflow() {
  reflow_valid_=true;
  if (reflows_.empty()) return false;
  auto matches=[&](const ScrollScene& candidate) {
    for (const auto& gutter:candidate.gutters) {
      auto found=std::find_if(scene_.scrollbars.begin(),scene_.scrollbars.end(),[&](const auto& bar) {
        return bar.element==gutter.element && bar.horizontal==gutter.horizontal;
      });
      unsigned hover=found==scene_.scrollbars.end() ? 0 : found->hover;
      unsigned pressed=drag_element_==gutter.element && drag_horizontal_==gutter.horizontal ? pressed_part_ : 0;
      if (gutter.thickness!=gutter.states[(hover ? 1 : 0)|(pressed ? 2 : 0)]) return false;
    }
    if (candidate.queries.empty()) return true;
    bool has_stuck=std::any_of(candidate.queries.begin(),candidate.queries.end(),[](const auto& query){return query.stuck;});
    size_t count=has_stuck ? candidate.transforms.size() : 0;
    std::vector<gfx::Vector2dF> sticky_offsets(count),box_offsets(count),containing_offsets(count);
    for (size_t index=0;index<count;++index) {
      const auto& node=candidate.transforms[index];
      if (!node.sticky) continue;
      gfx::PointF position;
      for (const auto& scroll:candidate.transforms) if (scroll.element) {
        auto current=std::find_if(scene_.transforms.begin(),scene_.transforms.end(),[&](const auto& item){return item.element==scroll.element;});
        int x=current==scene_.transforms.end() ? scroll.position_x : std::clamp(current->position_x,scroll.minimum_x,scroll.maximum_x);
        int y=current==scene_.transforms.end() ? scroll.position : std::clamp(current->position,scroll.minimum,scroll.maximum);
        if (scroll.element==node.constraint.x_scroll_ancestor_element_id.GetInternalValue()) position.set_x(x);
        if (scroll.element==node.constraint.y_scroll_ancestor_element_id.GetInternalValue()) position.set_y(y);
      }
      auto ancestor_offset=[&](cc::ElementId element,const auto& offsets) {
        gfx::Vector2dF result;
        for (size_t ancestor=0;ancestor<index;++ancestor) {
          const auto& previous=candidate.transforms[ancestor];
          if (!element || previous.sticky_element!=element.GetInternalValue()) continue;
          if (previous.constraint.x_scroll_ancestor_element_id==node.constraint.x_scroll_ancestor_element_id) result.set_x(offsets[ancestor].x());
          if (previous.constraint.y_scroll_ancestor_element_id==node.constraint.y_scroll_ancestor_element_id) result.set_y(offsets[ancestor].y());
        }
        return result;
      };
      auto box=ancestor_offset(node.constraint.nearest_element_shifting_sticky_box,box_offsets);
      auto containing=ancestor_offset(node.constraint.nearest_element_shifting_containing_block,containing_offsets);
      sticky_offsets[index]=node.constraint.StickyPositionOffset(position,{},box,containing);
      box_offsets[index]=box+sticky_offsets[index];
      containing_offsets[index]=box+containing+sticky_offsets[index];
    }
    for (const auto& query:candidate.queries) {
      if (query.stuck) {
        auto found=std::find_if(candidate.transforms.begin(),candidate.transforms.end(),[&](const auto& item){return item.sticky && item.sticky_element==query.element;});
        auto delta=found==candidate.transforms.end() ? gfx::Vector2dF() : sticky_offsets[found-candidate.transforms.begin()];
        unsigned horizontal=delta.x()>0 ? 1 : delta.x()<0 ? 2 : 0;
        unsigned vertical=delta.y()>0 ? 3 : delta.y()<0 ? 4 : 0;
        if (horizontal!=query.horizontal || vertical!=query.vertical) return false;
        continue;
      }
      auto node=std::find_if(candidate.transforms.begin(),candidate.transforms.end(),[&](const auto& item){return item.element==query.element;});
      auto current=std::find_if(scene_.transforms.begin(),scene_.transforms.end(),[&](const auto& item){return item.element==query.element;});
      int x=current==scene_.transforms.end() ? 0 : current->position_x;
      int y=current==scene_.transforms.end() ? 0 : current->position;
      unsigned horizontal=node==candidate.transforms.end() ? 0 : (x>node->minimum_x ? 1 : 0)|(x<node->maximum_x ? 2 : 0);
      unsigned vertical=node==candidate.transforms.end() ? 0 : (y>node->minimum ? 1 : 0)|(y<node->maximum ? 2 : 0);
      if (query.horizontal!=horizontal || query.vertical!=vertical) return false;
    }
    return true;
  };
  if (matches(scene_)) return false;
  auto found=std::find_if(reflows_.begin(),reflows_.end(),[&](const auto& item){return matches(item->scene_);});
  if (found==reflows_.end()) { reflow_valid_=false; return false; }
  auto& other=**found;
  auto old_transforms=scene_.transforms;
  auto old_bars=scene_.scrollbars;
  for (const auto& node:old_transforms) if (node.element) retained_positions_[node.element]={node.position_x,node.position};
  SwapScene(other);
  // Alternate layouts share one resident tile budget instead of multiplying it by the state count.
  other.tiles_.clear();
  // Each slot holds the scene displaced by the last swap, so its identity moves with it.
  std::swap(selected_reflow_,other.selected_reflow_);
  for (auto& [element,position]:retained_positions_)
    if (std::none_of(scene_.transforms.begin(),scene_.transforms.end(),[&](const auto& node){return node.element==element;})) position={};
  for (auto& node:scene_.transforms) if (node.element) {
    auto previous=retained_positions_.find(node.element);
    if (previous!=retained_positions_.end()) {
      node.position=std::clamp(previous->second.y,node.minimum,node.maximum);
      node.position_x=std::clamp(previous->second.x,node.minimum_x,node.maximum_x);
    }
  }
  for (auto& bar:scene_.scrollbars) {
    auto previous=std::find_if(old_bars.begin(),old_bars.end(),[&](const auto& item){return item.element==bar.element && item.horizontal==bar.horizontal;});
    if (previous!=old_bars.end()) bar.hover=previous->hover;
    if (bar.custom && previous!=old_bars.end() && (bar.horizontal ? bar.frame.height()!=previous->frame.height() : bar.frame.width()!=previous->frame.width())) {
      unsigned pressed=drag_element_==bar.element && drag_horizontal_==bar.horizontal ? pressed_part_ : 0;
      reflow_states_.push_back({bar.element,bar.horizontal,bar.hover,pressed});
    }
  }
  if (drag_element_ && std::none_of(scene_.scrollbars.begin(),scene_.scrollbars.end(),[&](const auto& bar) {
        return bar.element==drag_element_ && bar.horizontal==drag_horizontal_;
      })) { drag_element_=0; pressed_part_=0; repeat_at_=0; }
  UpdateTransforms(); UpdateLayouts();
  if (drag_element_ && !drag_part_) for (const auto& bar:scene_.scrollbars) if (bar.element==drag_element_ && bar.horizontal==drag_horizontal_) {
    auto thumb=Global(bar.thumb,bar.thumb_transform);
    drag_offset_=bar.horizontal ? pointer_x_-thumb.left() : pointer_y_-thumb.top();
    drag_coordinate_=bar.horizontal ? pointer_x_ : pointer_y_;
  }
  std::printf("COMPOSITOR: async gutter scene=%u\n",selected_reflow_);
  return true;
}
void ScrollCompositor::ReleaseRasterTiles() {
  tiles_.clear();
  for (auto& scene:reflows_) scene->ReleaseRasterTiles();
}
void ScrollCompositor::Clear() {
  reflows_.clear(); reflow_states_.clear(); retained_positions_.clear(); selected_reflow_=painted_reflow_=0;
  drag_element_=0; pressed_part_=0;
  pictures_.clear(); appearances_.clear(); images_.clear(); fonts_.clear(); offsets_.clear(); tiles_.clear(); scene_={};
}
bool ScrollCompositor::InstallScene(const ScrollScene& source) {
  uint64_t captured_element=drag_element_;
  unsigned captured_part=pressed_part_;
  struct HoverState { uint64_t element; bool horizontal; unsigned part; };
  std::vector<HoverState> hovered_parts;
  for (const auto& bar:scene_.scrollbars) hovered_parts.push_back({bar.element,bar.horizontal,bar.hover});
  bool reuse_resources=(Ready() || !images_.empty() || !fonts_.empty()) && source.image_keys==scene_.image_keys && source.font_keys==scene_.font_keys;
  if (!reuse_resources) {
    Clear();
    // Copy bytes before releasing publication ownership; no AP reference survives installation.
    for (const auto& source_image:source.images) {
      SkPixmap pixels(SkImageInfo::MakeN32Premul(source_image.width,source_image.height),source_image.pixels->data(),source_image.width*4);
      auto image=SkImages::RasterFromPixmapCopy(pixels);
      if (!image) { Clear(); return false; }
      images_.push_back(std::move(image));
    }
    for (const auto& data:source.fonts) {
      SkMemoryStream stream(data->data(),data->size(),true);
      auto face=SkTypeface::MakeDeserialize(&stream,skia::DefaultFontMgr());
      if (!face) { Clear(); return false; }
      fonts_.push_back(std::move(face));
    }
  }
  scene_.image_keys=source.image_keys; scene_.font_keys=source.font_keys;
  scene_.wheel_listeners=source.wheel_listeners; scene_.wheel_blocked=source.wheel_blocked;
  scene_.wheel_regions=source.wheel_regions; scene_.scrollbars=source.scrollbars; scene_.gutters=source.gutters; scene_.queries=source.queries;
  for (auto& bar:scene_.scrollbars) {
    auto found=std::find_if(hovered_parts.begin(),hovered_parts.end(),[&](const auto& state){return state.element==bar.element && state.horizontal==bar.horizontal;});
    if (found!=hovered_parts.end()) bar.hover=found->part;
  }
  scene_.transforms=source.transforms; scene_.clips=source.clips; scene_.effects=source.effects;
  SkDeserialProcs procedures;
  procedures.fImageCtx=this;
  procedures.fImageProc=[](const void* data,size_t size,std::optional<SkAlphaType>,void* context) -> sk_sp<SkImage> {
    auto& images=static_cast<ScrollCompositor*>(context)->images_;
    uint32_t index=0; if (size!=sizeof(index)) return {}; std::memcpy(&index,data,size);
    return index<images.size() ? images[index] : nullptr;
  };
  procedures.fTypefaceCtx=this;
  procedures.fTypefaceStreamProc=[](SkStream& stream,void* context) -> sk_sp<SkTypeface> {
    auto& fonts=static_cast<ScrollCompositor*>(context)->fonts_;
    uint32_t index=0; if (stream.read(&index,sizeof(index))!=sizeof(index)) return {};
    return index<fonts.size() ? fonts[index] : nullptr;
  };
  std::vector<ScrollPicture> metadata;
  std::vector<sk_sp<SkPicture>> pictures;
  std::vector<std::vector<sk_sp<SkPicture>>> appearances;
  unsigned reused=0;
  for (unsigned index=0;index<source.pictures.size();++index) {
    const auto& incoming=source.pictures[index];
    if (!incoming.raster_appearances.empty()) {
      // Compressed pixels remain local and are expanded only for visible tiles.
      ScrollPicture value=incoming;
      value.data=SkData::MakeEmpty();
      metadata.push_back(std::move(value));
      pictures.push_back(SkPicture::MakePlaceholder(incoming.bounds));
      appearances.emplace_back();
      std::erase_if(tiles_,[&](const Tile& tile){return tile.picture==index;});
      continue;
    }
    bool reuse=reuse_resources && index<scene_.pictures.size() &&
        scene_.pictures[index].data->size()==incoming.data->size() &&
        !std::memcmp(scene_.pictures[index].data->data(),incoming.data->data(),incoming.data->size());
    reuse=reuse && scene_.pictures[index].appearances.size()==incoming.appearances.size();
    if (reuse) for (unsigned state=0;state<incoming.appearances.size();++state) {
      const auto& left=scene_.pictures[index].appearances[state]; const auto& right=incoming.appearances[state];
      if (static_cast<bool>(left)!=static_cast<bool>(right) || (left && (left->size()!=right->size() || std::memcmp(left->data(),right->data(),left->size())))) { reuse=false; break; }
    }
    auto picture=reuse ? pictures_[index] : SkPicture::MakeFromData(incoming.data->data(),incoming.data->size(),&procedures);
    if (!picture) { Clear(); return false; }
    if (!reuse) {
      // A spatial index avoids replaying every offscreen operation for each visible tile.
      SkRTreeFactory factory;
      SkPictureRecorder recorder;
      picture->playback(recorder.beginRecording(incoming.bounds,&factory));
      picture=recorder.finishRecordingAsPicture();
    }
    ScrollPicture value; value.transform=incoming.transform; value.clip=incoming.clip;
    value.effect=incoming.effect; value.bounds=incoming.bounds;
    value.scrollbar=incoming.scrollbar; value.track_piece=incoming.track_piece; value.part=incoming.part;
    value.appearance_bounds=incoming.appearance_bounds;
    value.appearances.resize(incoming.appearances.size());
    std::vector<sk_sp<SkPicture>> states(std::max(size_t(1),incoming.appearances.size()));
    for (unsigned state=0;state<states.size();++state) {
      auto data=incoming.appearances.empty() ? nullptr : incoming.appearances[state];
      if (!data) { states[state]=picture; continue; }
      bool duplicate=false;
      for (unsigned previous=0;previous<state;++previous) if (data==incoming.appearances[previous]) {
        value.appearances[state]=value.appearances[previous]; states[state]=states[previous]; duplicate=true; break;
      }
      if (duplicate) continue;
      value.appearances[state]=reuse ? scene_.pictures[index].appearances[state] : SkData::MakeWithCopy(data->data(),data->size());
      states[state]=reuse ? appearances_[index][state] : state==0 ? picture : SkPicture::MakeFromData(data->data(),data->size(),&procedures);
      if (!states[state]) { Clear(); return false; }
    }
    appearances.push_back(std::move(states));
    value.data=reuse ? scene_.pictures[index].data : SkData::MakeWithCopy(incoming.data->data(),incoming.data->size());
    metadata.push_back(std::move(value)); pictures.push_back(std::move(picture));
    if (reuse) ++reused;
    else std::erase_if(tiles_,[&](const Tile& tile){return tile.picture==index;});
  }
  std::erase_if(tiles_,[&](const Tile& tile){return tile.picture>=pictures.size();});
  scene_.pictures=std::move(metadata); pictures_=std::move(pictures); appearances_=std::move(appearances);
  if (reused) std::printf("COMPOSITOR: reused pictures=%u resident=%zu\n",reused,tiles_.size());
  UpdateTransforms();
  drag_element_=captured_element; pressed_part_=captured_part;
  if (std::none_of(scene_.scrollbars.begin(),scene_.scrollbars.end(),[&](const auto& bar){return bar.element==drag_element_ && bar.horizontal==drag_horizontal_;})) CancelDrag();
  UpdateLayouts();
  for (auto& bar:scene_.scrollbars) bar.hover=HitPart(bar,pointer_x_,pointer_y_);
  UpdateLayouts();
  std::printf("COMPOSITOR: input wheel_listeners=%u blocking_regions=%zu global_block=%u bars=%zu\n",
      scene_.wheel_listeners,scene_.wheel_regions.size(),scene_.wheel_blocked,scene_.scrollbars.size());
  return true;
}
SkRect ScrollCompositor::Bounds(const ScrollPicture& picture) const {
  if (picture.appearance_bounds.empty() || picture.scrollbar<0) return picture.bounds;
  return picture.appearance_bounds[scene_.scrollbars[picture.scrollbar].layout];
}
void ScrollCompositor::CancelDrag() {
  drag_element_=0; pressed_part_=0;
  UpdateLayouts();
  SelectReflow();
}
void ScrollCompositor::UpdateLayouts() {
  bool changed=false;
  for (auto& bar:scene_.scrollbars) {
    if (bar.layouts.empty()) continue;
    unsigned pressed=drag_element_==bar.element && drag_horizontal_==bar.horizontal ? pressed_part_ : 0;
    auto found=std::find_if(bar.layouts.begin(),bar.layouts.end(),[&](const auto& layout){return layout.hover==bar.hover && layout.press==pressed;});
    unsigned index=found==bar.layouts.end() ? 0 : static_cast<unsigned>(found-bar.layouts.begin());
    const auto& layout=bar.layouts[index];
    auto old_thumb=Global(bar.thumb,bar.thumb_transform);
    bar.layout=index; bar.track=layout.track; bar.thumb=layout.thumb;
    bar.back_start=layout.back_start; bar.back_end=layout.back_end;
    bar.forward_start=layout.forward_start; bar.forward_end=layout.forward_end;
    UpdateTransforms();
    if (drag_element_==bar.element && drag_horizontal_==bar.horizontal && !drag_part_) {
      auto new_thumb=Global(bar.thumb,bar.thumb_transform);
      drag_offset_+=bar.horizontal ? old_thumb.left()-new_thumb.left() : old_thumb.top()-new_thumb.top();
    }
    changed=true;
  }
  if (changed) UpdateTransforms();
}
void ScrollCompositor::UpdateTransforms() {
  offsets_.resize(scene_.transforms.size());
  std::vector<gfx::Vector2dF> sticky_box_offsets(offsets_.size()),containing_block_offsets(offsets_.size());
  for (const auto& bar:scene_.scrollbars) {
    auto found=std::find_if(scene_.transforms.begin(),scene_.transforms.end(),[&](const auto& node){return node.element==bar.element;});
    if (found!=scene_.transforms.end()) {
      int minimum=bar.horizontal ? found->minimum_x : found->minimum;
      int maximum=bar.horizontal ? found->maximum_x : found->maximum;
      int position=bar.horizontal ? found->position_x : found->position;
      float travel=bar.horizontal ? bar.track.width()-bar.thumb.width() : bar.track.height()-bar.thumb.height();
      float distance=maximum>minimum ? static_cast<float>(position-minimum)*travel/(maximum-minimum) : 0;
      float offset=distance>0 && distance<1 ? 1 : std::floor(distance);
      if (bar.horizontal) scene_.transforms[bar.thumb_transform].x=bar.track.left()+offset-bar.thumb.left();
      else scene_.transforms[bar.thumb_transform].y=bar.track.top()+offset-bar.thumb.top();
    }
  }
  for (unsigned index=0; index<scene_.transforms.size(); ++index) {
    auto& node=scene_.transforms[index];
    SkPoint offset=SkPoint::Make(node.x,node.y);
    if (node.element) { offset.fX=node.x-node.position_x; offset.fY=node.y-node.position; }
    if (node.sticky) {
      gfx::PointF position;
      for (const auto& scroll:scene_.transforms) {
        if (scroll.element==node.constraint.x_scroll_ancestor_element_id.GetInternalValue()) position.set_x(scroll.position_x);
        if (scroll.element==node.constraint.y_scroll_ancestor_element_id.GetInternalValue()) position.set_y(scroll.position);
      }
      auto ancestor_offset=[&](cc::ElementId element,const auto& offsets) {
        gfx::Vector2dF result;
        for (unsigned ancestor=0;ancestor<index;++ancestor) {
          const auto& candidate=scene_.transforms[ancestor];
          if (!element || candidate.sticky_element!=element.GetInternalValue()) continue;
          if (candidate.constraint.x_scroll_ancestor_element_id==node.constraint.x_scroll_ancestor_element_id)
            result.set_x(offsets[ancestor].x());
          if (candidate.constraint.y_scroll_ancestor_element_id==node.constraint.y_scroll_ancestor_element_id)
            result.set_y(offsets[ancestor].y());
        }
        return result;
      };
      auto box=ancestor_offset(node.constraint.nearest_element_shifting_sticky_box,sticky_box_offsets);
      auto containing=ancestor_offset(node.constraint.nearest_element_shifting_containing_block,containing_block_offsets);
      auto delta=node.constraint.StickyPositionOffset(position,{},box,containing);
      sticky_box_offsets[index]=box+delta;
      containing_block_offsets[index]=box+containing+delta;
      offset.fX=std::round(delta.x()+node.constraint.pixel_snap_offset.x());
      offset.fY=std::round(delta.y()+node.constraint.pixel_snap_offset.y());
    }
    if (node.parent>=0) offset+=offsets_[node.parent];
    offsets_[index]=offset;
  }
}
void ScrollCompositor::Clip(SkCanvas& canvas,int clip) {
  for (;clip>=0;clip=scene_.clips[clip].parent) {
    auto& node=scene_.clips[clip]; auto rectangle=node.rectangle;
    if (node.transform>=0) rectangle.offset(offsets_[node.transform].x(),offsets_[node.transform].y());
    canvas.clipRRect(rectangle,true);
  }
}
bool ScrollCompositor::Contains(int clip,float x,float y) const {
  for (;clip>=0;clip=scene_.clips[clip].parent) {
    const auto& node=scene_.clips[clip]; auto rectangle=node.rectangle;
    if (node.transform>=0) rectangle.offset(offsets_[node.transform].x(),offsets_[node.transform].y());
    if (!rectangle.contains(SkRect::MakeXYWH(x,y,0.01f,0.01f))) return false;
  }
  return true;
}
uint64_t ScrollCompositor::Scroll(int x,int y,int delta,ScrollPosition* position) {
  for (int index=static_cast<int>(scene_.pictures.size())-1;index>=0;--index) {
    const auto& picture=scene_.pictures[index];
    if (!Contains(picture.clip,x,y)) continue;
    auto bounds=Bounds(picture);
    if (picture.transform>=0) bounds.offset(offsets_[picture.transform].x(),offsets_[picture.transform].y());
    if (!bounds.contains(x,y)) continue;
    int transform=picture.transform;
    while (transform>=0 && !scene_.transforms[transform].element) transform=scene_.transforms[transform].parent;
    if (transform>=0) return Move(scene_.transforms[transform].element,delta,false,true,position);
    // The topmost painted target determines the scroll chain, including fixed overlays.
    return 0;
  }
  return 0;
}
void ScrollCompositor::Draw(SkCanvas& canvas,int width,int height) {
  canvas.clear(SK_ColorWHITE);
  std::vector<int> effects;
  unsigned misses=0;
  for (unsigned index=0;index<pictures_.size();++index) {
    const auto& picture=scene_.pictures[index];
    unsigned appearance=Appearance(picture);
    std::vector<int> path;
    for (int effect=picture.effect;effect>=0;effect=scene_.effects[effect].parent) path.push_back(effect);
    std::reverse(path.begin(),path.end());
    unsigned common=0; while (common<path.size() && common<effects.size() && path[common]==effects[common]) ++common;
    while (effects.size()>common) { canvas.restore(); canvas.restore(); effects.pop_back(); }
    for (unsigned depth=common;depth<path.size();++depth) {
      const auto& effect=scene_.effects[path[depth]];
      canvas.save(); Clip(canvas,effect.clip);
      SkPaint paint; paint.setAlphaf(effect.opacity); paint.setBlendMode(effect.blend);
      if (effect.opacity==1 && effect.blend==SkBlendMode::kSrcOver) canvas.save();
      else canvas.saveLayer(nullptr,&paint);
      effects.push_back(path[depth]);
    }
    canvas.save(); Clip(canvas,picture.clip);
    SkPoint offset=picture.transform>=0 ? offsets_[picture.transform] : SkPoint::Make(0,0);
    canvas.translate(offset.x(),offset.y());
    if (picture.track_piece && picture.scrollbar>=0) {
      const auto& bar=scene_.scrollbars[picture.scrollbar];
      auto thumb=Global(bar.thumb,bar.thumb_transform);
      auto target=bar.track;
      float middle=bar.horizontal ? thumb.centerX()-offset.x() : thumb.centerY()-offset.y();
      if (bar.horizontal) { if (picture.track_piece<0) target.fRight=middle; else target.fLeft=middle; }
      else { if (picture.track_piece<0) target.fBottom=middle; else target.fTop=middle; }
      canvas.clipRect(target);
    }
    SkRect visible=canvas.getLocalClipBounds();
    if (visible.intersect(Bounds(picture))) {
      constexpr int tile_width=512,tile_height=256;
      int left=std::floor(visible.left()/tile_width),right=std::ceil(visible.right()/tile_width);
      int top=std::floor(visible.top()/tile_height),bottom=std::ceil(visible.bottom()/tile_height);
      for (int y=top;y<bottom;++y) for (int x=left;x<right;++x) {
        auto found=std::find_if(tiles_.begin(),tiles_.end(),[&](const Tile& tile){return tile.picture==index && tile.appearance==appearance && tile.x==x && tile.y==y;});
        SkIRect extent=SkIRect::MakeXYWH(x*tile_width,y*tile_height,tile_width,tile_height);
        SkIRect bounds; Bounds(picture).roundOut(&bounds);
        if (!extent.intersect(bounds)) continue;
        sk_sp<SkImage> image;
        if (found!=tiles_.end()) { image=found->image; tiles_.erase(found); }
        else if (!picture.raster_appearances.empty()) {
          while (!ScrollWireMemoryAvailable(2*1024*1024) && !tiles_.empty()) tiles_.pop_front();
          if (!ScrollWireMemoryAvailable(2*1024*1024)) continue;
          image=DecodeScrollTile(*picture.raster_appearances[appearance],x,y);
          if (!image) continue;
          ++misses;
        } else {
          auto surface=SkSurfaces::Raster(SkImageInfo::MakeN32Premul(extent.width(),extent.height()));
          if (!surface) continue;
          auto* target=surface->getCanvas(); target->clear(SK_ColorTRANSPARENT);
          target->translate(-extent.left(),-extent.top()); target->drawPicture(appearances_[index][appearance]);
          image=surface->makeImageSnapshot(); ++misses;
        }
        tiles_.push_back({index,appearance,x,y,image});
        size_t bytes=0;
        for (const auto& tile:tiles_) bytes+=static_cast<size_t>(tile.image->width())*tile.image->height()*4;
        while (bytes>32*1024*1024 || tiles_.size()>1024) {
          bytes-=static_cast<size_t>(tiles_.front().image->width())*tiles_.front().image->height()*4;
          tiles_.pop_front();
        }
        canvas.drawImage(image,extent.left(),extent.top());
      }
    }
    canvas.restore();
  }
  while (!effects.empty()) { canvas.restore(); canvas.restore(); effects.pop_back(); }
  if (misses) {
    size_t bytes=0;
    for (const auto& tile:tiles_) bytes+=static_cast<size_t>(tile.image->width())*tile.image->height()*4;
    std::printf("COMPOSITOR: raster tiles=%u resident=%zu bytes=%zu viewport=%dx%d\n",misses,tiles_.size(),bytes,width,height);
  }
}
}

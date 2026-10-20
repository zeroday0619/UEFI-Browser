#ifndef UEFI_SCROLL_SNAPSHOT_H
#define UEFI_SCROLL_SNAPSHOT_H
#include <cstdint>
#include <array>
#include "third_party/skia/include/core/SkBlendMode.h"
#include <memory>
#include <vector>
#include "cc/trees/sticky_position_constraint.h"
#include "third_party/skia/include/core/SkData.h"
#include "third_party/skia/include/core/SkRRect.h"
namespace uefi_browser {
struct ScrollPosition { int x=0, y=0; };
struct ScrollTransform {
  int parent=-1;
  float x=0, y=0;
  uint64_t element=0, scroll_parent=0, sticky_element=0;
  int position=0, minimum=0, maximum=0, viewport=0;
  int position_x=0, minimum_x=0, maximum_x=0, viewport_x=0;
  bool horizontal=false, vertical=false, chain_x=true;
  bool chain=true, sticky=false;
  cc::StickyPositionConstraint constraint;
};
struct ScrollClip { int parent=-1, transform=-1; SkRRect rectangle; };
struct ScrollEffect { int parent=-1, clip=-1; float opacity=1; SkBlendMode blend=SkBlendMode::kSrcOver; };
struct ScrollPicture {
  int transform=-1, clip=-1, effect=-1;
  SkRect bounds;
  int scrollbar=-1, track_piece=0;
  unsigned part=0;
  std::vector<sk_sp<SkData>> appearances;
  std::vector<SkRect> appearance_bounds;
  sk_sp<SkData> data;
};
struct ScrollRegion { int transform=-1, clip=-1; SkRect bounds; };
struct ScrollbarLayout {
  unsigned hover=0, press=0;
  SkRect track, thumb, back_start, back_end, forward_start, forward_end;
};
struct ScrollbarScene {
  uint64_t element=0;
  int transform=-1, clip=-1, thumb_transform=-1;
  unsigned track_picture=0, thumb_picture=0, last_picture=0;
  bool horizontal=false, jump=false, custom=false, enabled=true;
  std::array<int,4> thickness{};
  unsigned painted_hover=0, painted_press=0, hover=0, layout=0;
  std::vector<ScrollbarLayout> layouts;
  int initial_delay=250, repeat_delay=50;
  SkRect back_start=SkRect::MakeEmpty(), back_end=SkRect::MakeEmpty(), forward_start=SkRect::MakeEmpty(), forward_end=SkRect::MakeEmpty();
  SkRect frame, track, thumb;
};
struct ScrollQuery { uint64_t element; unsigned horizontal, vertical; bool stuck=false; };
struct ScrollGutter { uint64_t element; bool horizontal; int thickness; std::array<int,4> states; };
struct ScrollImage { int width=0, height=0; sk_sp<SkData> pixels; };
// Shared publications contain values and immutable bytes; Skia objects are rebuilt by the consumer.
struct ScrollScene {
  std::vector<std::shared_ptr<ScrollScene>> reflows;
  std::vector<ScrollGutter> gutters;
  std::vector<ScrollQuery> queries;
  bool wheel_listeners=false, wheel_blocked=false;
  std::vector<ScrollRegion> wheel_regions;
  std::vector<ScrollbarScene> scrollbars;
  std::vector<ScrollTransform> transforms;
  std::vector<ScrollClip> clips;
  std::vector<ScrollEffect> effects;
  std::vector<ScrollPicture> pictures;
  std::vector<uint32_t> image_keys, font_keys;
  std::vector<ScrollImage> images;
  std::vector<sk_sp<SkData>> fonts;
};
struct ScrollSnapshot {
  uint64_t acknowledgement=0;
  std::shared_ptr<ScrollScene> scene;
};
}
#endif

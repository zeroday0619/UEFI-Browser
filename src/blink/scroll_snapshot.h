#ifndef UEFI_SCROLL_SNAPSHOT_H
#define UEFI_SCROLL_SNAPSHOT_H
#include <cstdint>
#include <cmath>
#include "third_party/skia/include/core/SkMatrix.h"
#include <array>
#include "third_party/skia/include/core/SkBlendMode.h"
#include <memory>
#include <vector>
#include "cc/trees/sticky_position_constraint.h"
#include "third_party/skia/include/core/SkData.h"
#include "third_party/skia/include/core/SkImage.h"
#include "third_party/skia/include/core/SkTypeface.h"
#include "third_party/skia/include/core/SkRRect.h"
namespace uefi_browser {
struct ScrollPosition { int x=0, y=0; };
struct ScrollTransform {
  int parent=-1;
  float x=0, y=0;
  float scale_x=1, skew_x=0, skew_y=0, scale_y=1;
  uint64_t element=0, scroll_parent=0, sticky_element=0;
  int position=0, minimum=0, maximum=0, viewport=0;
  int position_x=0, minimum_x=0, maximum_x=0, viewport_x=0;
  bool horizontal=false, vertical=false, chain_x=true;
  bool chain=true, sticky=false;
  cc::StickyPositionConstraint constraint;
};
inline SkMatrix ScrollMatrix(const ScrollTransform& value, float x, float y) {
  return SkMatrix::MakeAll(value.scale_x,value.skew_x,x,value.skew_y,value.scale_y,y,0,0,1);
}
inline bool BoundedScrollMatrix(const SkMatrix& matrix) {
  // Bounded components can still overflow when ancestors are multiplied together.
  SkScalar components[9]; matrix.get9(components);
  for (float value:components)
    if (!std::isfinite(value) || std::abs(value)>1000000) return false;
  return !matrix.hasPerspective();
}
struct ScrollClip { int parent=-1, transform=-1; SkRRect rectangle; };
enum class ScrollFilterKind : unsigned {
  Grayscale, Sepia, Saturate, HueRotate, Invert, Brightness, Contrast, Opacity,
  Blur, DropShadow, ColorMatrix
};
struct ScrollFilter {
  ScrollFilterKind kind=ScrollFilterKind::Grayscale;
  float amount=0;
  int offset_x=0, offset_y=0;
  unsigned tile_mode=3;
  std::array<float,4> color{};
  std::array<float,20> matrix{};
};
inline bool ValidScrollFilter(const ScrollFilter& value) {
  if (static_cast<unsigned>(value.kind)>static_cast<unsigned>(ScrollFilterKind::ColorMatrix) ||
      !std::isfinite(value.amount) || value.offset_x < -4096 || value.offset_x > 4096 || value.offset_y < -4096 || value.offset_y > 4096 ||
      value.tile_mode>3) return false;
  for (float channel:value.color) if (!std::isfinite(channel) || std::abs(channel)>16) return false;
  if (value.color[3]<0 || value.color[3]>1) return false;
  for (float coefficient:value.matrix) if (!std::isfinite(coefficient) || std::abs(coefficient)>10000) return false;
  switch (value.kind) {
    case ScrollFilterKind::HueRotate: return std::abs(value.amount)<=360000;
    // Filters that create alpha from transparent input require an explicit filter region.
    case ScrollFilterKind::ColorMatrix: return value.amount==0 && value.matrix[19]==0;
    case ScrollFilterKind::Blur:
    case ScrollFilterKind::DropShadow: return value.amount>=0 && value.amount<=128;
    case ScrollFilterKind::Saturate:
    case ScrollFilterKind::Brightness:
    case ScrollFilterKind::Contrast: return value.amount>=0 && value.amount<=256;
    default: return value.amount>=0 && value.amount<=1;
  }
}
struct ScrollEffect {
  int parent=-1, clip=-1, transform=-1;
  float opacity=1;
  SkBlendMode blend=SkBlendMode::kSrcOver;
  std::vector<ScrollFilter> filters;
};
struct ScrollRaster {
  SkIRect bounds;
  std::vector<sk_sp<SkData>> tiles;
};
struct GpuCanvasFrame;
struct ScrollPicture {
  uint64_t gpu_token=0;
  std::shared_ptr<GpuCanvasFrame> gpu_frame;
  int transform=-1, clip=-1, effect=-1;
  SkRect bounds;
  int scrollbar=-1, track_piece=0;
  unsigned part=0;
  std::vector<sk_sp<SkData>> appearances;
  std::vector<SkRect> appearance_bounds;
  sk_sp<SkData> data;
  // The consumer owns these validated compressed tiles; no producer object is referenced.
  std::vector<std::shared_ptr<ScrollRaster>> raster_appearances;
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
// This object graph is process-local; CPL3 publications use the scroll wire protocol.
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
  // Capture retains renderer-local resources so encoding does not create duplicate font caches.
  std::vector<sk_sp<SkImage>> renderer_images;
  std::vector<sk_sp<SkTypeface>> renderer_fonts;
};
struct ScrollSnapshot {
  uint64_t acknowledgement=0;
  std::shared_ptr<ScrollScene> scene;
  // Only the producer retains this buffer; publication copies its bytes into the mailbox.
  std::shared_ptr<const std::vector<uint8_t>> encoded;
};
}
#endif

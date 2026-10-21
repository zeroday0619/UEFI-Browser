#ifndef UEFI_SCROLL_SCENE_H
#define UEFI_SCROLL_SCENE_H
#include "blink/scroll_snapshot.h"
#include "third_party/skia/include/core/SkCanvas.h"
#include "third_party/skia/include/core/SkPicture.h"
#include "third_party/skia/include/core/SkImage.h"
#include "third_party/skia/include/core/SkImageFilter.h"
#include "third_party/skia/include/core/SkTypeface.h"
#include <deque>
#include <map>
#include <optional>
#include "cc/paint/paint_record.h"
#include "ui/gfx/geometry/rect.h"
namespace blink { class WebView; }
namespace uefi_browser {
struct ViewportSnapshot {
  gfx::Rect bounds;
  std::optional<cc::PaintRecord> picture;
  int64_t paint_microseconds=0;
};
const char* ScrollCaptureFailureName(int reason);
// Only immutable paint records and owned resources survive between encoding tasks.
class ScrollSceneCapture {
 public:
  ~ScrollSceneCapture();
  bool EncodeSlice();
  bool Finish();
  void CaptureReflows(blink::WebView&);
  std::shared_ptr<ScrollScene> TakeScene();
 private:
  struct State;
  explicit ScrollSceneCapture(std::unique_ptr<State>);
  std::unique_ptr<State> state_;
  friend std::unique_ptr<ScrollSceneCapture> BeginScrollScene(
      blink::WebView&, int*, std::shared_ptr<const ScrollScene>, ViewportSnapshot*);
};
std::unique_ptr<ScrollSceneCapture> BeginScrollScene(
    blink::WebView&, int* reason, std::shared_ptr<const ScrollScene> previous,
    ViewportSnapshot* viewport=nullptr);
uint64_t KeyboardScrollTarget(blink::WebView&);
std::shared_ptr<ScrollScene> CaptureScrollScene(blink::WebView&, int* reason, const ScrollScene* previous);
void CaptureScrollbarReflows(blink::WebView&, ScrollScene&);
void ApplyScrollOffset(blink::WebView&, uint64_t element, int x, int y, bool synchronize_queries);
void ApplyScrollbarState(blink::WebView&, uint64_t element, bool horizontal, unsigned hover, unsigned press);
struct ScrollbarState { uint64_t element; bool horizontal; unsigned hover, press; };
class ScrollCompositor {
 public:
  bool Install(const ScrollScene&, int width, int height);
  std::vector<ScrollbarState> TakeReflowStates();
  void Clear();
  void ReleaseRasterTiles();
  uint64_t Scroll(int x, int y, int delta, ScrollPosition* position);
  bool WheelAllowed(int x, int y) const;
  bool WheelListeners() const { return scene_.wheel_listeners; }
  uint64_t Keyboard(uint64_t element, unsigned scan, bool space, bool shift, ScrollPosition* position);
  bool Hover(int x, int y);
  bool AppearanceChanged() const;
  bool Pointer(int x, int y, unsigned buttons, uint64_t* element, ScrollPosition* position);
  bool Tick(int64_t now, uint64_t* element, ScrollPosition* position);
  void CancelDrag();
  void Draw(SkCanvas& canvas, int width, int height);
  bool Ready() const { return !pictures_.empty(); }
 private:
  bool SelectReflow();
  bool reflow_valid_=true;
  void SwapScene(ScrollCompositor&);
  bool InstallScene(const ScrollScene&);
  bool PrepareFilters();
  int width_=0, height_=0;
  std::vector<sk_sp<SkImageFilter>> filters_;
  std::vector<std::unique_ptr<ScrollCompositor>> reflows_;
  std::vector<ScrollbarState> reflow_states_;
  std::map<uint64_t,ScrollPosition> retained_positions_;
  unsigned selected_reflow_=0, painted_reflow_=0;
  uint64_t Move(uint64_t element, int value, bool absolute, bool chain, ScrollPosition* position, bool horizontal=false);
  bool LocalPoint(int transform, float x, float y, SkPoint* point) const;
  bool Hit(SkRect bounds, int transform, float x, float y) const;
  SkRect Thumb(const ScrollbarScene&) const;
  void UpdateTransforms();
  void UpdateLayouts();
  SkRect Bounds(const ScrollPicture&) const;
  void Clip(SkCanvas&, int clip, int stop=-1);
  bool Contains(int clip, float x, float y) const;
  uint64_t drag_element_=0;
  float drag_offset_=0;
  float drag_coordinate_=0;
  bool drag_horizontal_=false;
  int drag_part_=0, pointer_x_=-1, pointer_y_=-1;
  unsigned pressed_part_=0;
  unsigned HitPart(const ScrollbarScene&, int x, int y) const;
  unsigned Appearance(const ScrollPicture&) const;
  int64_t repeat_at_=0;
  uint64_t Repeat(ScrollPosition* position);
  ScrollScene scene_;
  std::vector<sk_sp<SkPicture>> pictures_;
  std::vector<std::vector<sk_sp<SkPicture>>> appearances_;
  std::vector<sk_sp<SkImage>> images_;
  std::vector<sk_sp<SkTypeface>> fonts_;
  std::vector<SkMatrix> matrices_, inverses_;
  std::vector<bool> invertible_;
  struct Tile { unsigned picture, appearance; int x,y; sk_sp<SkImage> image; };
  std::deque<Tile> tiles_;
};
}
#endif

#ifndef UEFI_SCROLL_SCENE_H
#define UEFI_SCROLL_SCENE_H
#include "blink/scroll_snapshot.h"
#include "third_party/skia/include/core/SkCanvas.h"
#include "third_party/skia/include/core/SkPicture.h"
#include "third_party/skia/include/core/SkImage.h"
#include "third_party/skia/include/core/SkTypeface.h"
#include <deque>
namespace blink { class WebView; }
namespace uefi_browser {
uint64_t KeyboardScrollTarget(blink::WebView&);
std::shared_ptr<ScrollScene> CaptureScrollScene(blink::WebView&, int* reason, const ScrollScene* previous);
class ScrollCompositor {
 public:
  bool Install(const ScrollScene&);
  void Clear();
  uint64_t Scroll(int x, int y, int delta, ScrollPosition* position);
  bool WheelAllowed(int x, int y) const;
  bool WheelListeners() const { return scene_.wheel_listeners; }
  uint64_t Keyboard(uint64_t element, unsigned scan, bool space, bool shift, ScrollPosition* position);
  bool Hover(int x, int y);
  bool AppearanceChanged() const;
  bool Pointer(int x, int y, unsigned buttons, uint64_t* element, ScrollPosition* position);
  bool Tick(int64_t now, uint64_t* element, ScrollPosition* position);
  void CancelDrag() { drag_element_=0; pressed_part_=0; }
  void Draw(SkCanvas& canvas, int width, int height);
  bool Ready() const { return !pictures_.empty(); }
 private:
  uint64_t Move(uint64_t element, int value, bool absolute, bool chain, ScrollPosition* position, bool horizontal=false);
  SkRect Global(SkRect bounds, int transform) const;
  void UpdateTransforms();
  void Clip(SkCanvas&, int clip);
  bool Contains(int clip, float x, float y) const;
  uint64_t drag_element_=0;
  float drag_offset_=0;
  bool drag_horizontal_=false;
  int drag_part_=0, pointer_x_=-1, pointer_y_=-1;
  unsigned pressed_part_=0;
  unsigned HitPart(const ScrollbarScene&, int x, int y) const;
  unsigned Appearance(const ScrollPicture&) const;
  int64_t repeat_at_=0;
  uint64_t Repeat(ScrollPosition* position);
  ScrollScene scene_;
  std::vector<sk_sp<SkPicture>> pictures_;
  std::vector<std::array<sk_sp<SkPicture>,8>> appearances_;
  std::vector<sk_sp<SkImage>> images_;
  std::vector<sk_sp<SkTypeface>> fonts_;
  std::vector<SkPoint> offsets_;
  struct Tile { unsigned picture, appearance; int x,y; sk_sp<SkImage> image; };
  std::deque<Tile> tiles_;
};
}
#endif

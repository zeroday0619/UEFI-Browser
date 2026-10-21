#ifndef UEFI_IME_POPUP_H
#define UEFI_IME_POPUP_H
#include "blink/text_composer.h"
#include "third_party/skia/include/core/SkRect.h"
class SkCanvas;
namespace uefi_browser {
class ImePopup {
 public:
  void Draw(SkCanvas& canvas, const std::vector<InputCandidate>& candidates,
            int x, int y, int width, int height);
  int Hit(int x, int y) const;
  bool Visible() const { return !ids_.empty(); }
  void Clear() { bounds_ = SkIRect::MakeEmpty(); ids_.clear(); }
 private:
  SkIRect bounds_ = SkIRect::MakeEmpty();
  std::vector<int> ids_;
};
}
#endif

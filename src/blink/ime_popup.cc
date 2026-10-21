#include "blink/ime_popup.h"
#include "skia/ext/font_utils.h"
#include "third_party/skia/include/core/SkCanvas.h"
#include "third_party/skia/include/core/SkFont.h"
#include "third_party/skia/include/core/SkFontMgr.h"
#include "third_party/skia/include/core/SkPaint.h"
#include <algorithm>
namespace uefi_browser {
void ImePopup::Draw(SkCanvas& canvas, const std::vector<InputCandidate>& candidates,
                    int x, int y, int width, int height) {
  Clear();
  if (candidates.empty() || width < 40 || height < 40) return;
  int rows = std::min<int>({static_cast<int>(candidates.size()), 9, (height - 8) / 28});
  int popup_width = std::min(300, width - 8), popup_height = rows * 28 + 8;
  x = std::clamp(x, 0, width - popup_width);
  y = std::clamp(y, 0, height - popup_height);
  bounds_ = SkIRect::MakeXYWH(x, y, popup_width, popup_height);
  SkPaint paint;
  paint.setColor(SkColorSetRGB(245, 248, 252));
  canvas.drawRect(SkRect::Make(bounds_), paint);
  paint.setColor(SkColorSetRGB(70, 90, 120));
  paint.setStyle(SkPaint::kStroke_Style);
  canvas.drawRect(SkRect::Make(bounds_), paint);
  paint.setStyle(SkPaint::kFill_Style);
  auto manager = skia::DefaultFontMgr();
  canvas.save();
  canvas.clipRect(SkRect::Make(bounds_));
  for (int row = 0; row < rows; ++row) {
    ids_.push_back(candidates[row].id);
    if (candidates[row].selected) {
      paint.setColor(SkColorSetRGB(210, 230, 250));
      canvas.drawRect(SkRect::MakeXYWH(x + 1, y + 4 + row * 28, popup_width - 2, 28), paint);
    }
    paint.setColor(SkColorSetRGB(30, 40, 55));
    std::u16string text = std::u16string(1, u'1' + row) + u"  " + candidates[row].text;
    float left = x + 8;
    for (size_t index = 0; index < text.size(); ++index) {
      if (left >= x + popup_width - 8) break;
      SkUnichar character = text[index];
      if (character >= 0xd800 && character <= 0xdbff && index + 1 < text.size() &&
          text[index + 1] >= 0xdc00 && text[index + 1] <= 0xdfff)
        character = 0x10000 + ((character - 0xd800) << 10) + text[++index] - 0xdc00;
      auto face = manager->matchFamilyStyleCharacter(nullptr, SkFontStyle(), nullptr, 0, character);
      SkFont font(std::move(face), 18);
      canvas.drawSimpleText(&character, sizeof(character), SkTextEncoding::kUTF32,
                            left, y + 24 + row * 28, font, paint);
      left += font.measureText(&character, sizeof(character), SkTextEncoding::kUTF32);
    }
  }
  canvas.restore();
}
int ImePopup::Hit(int x, int y) const {
  if (!bounds_.contains(x, y) || y < bounds_.top() + 4) return -1;
  size_t row = (y - bounds_.top() - 4) / 28;
  return row < ids_.size() ? ids_[row] : -1;
}
}

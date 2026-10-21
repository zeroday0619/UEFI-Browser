#include "blink/text_composer.h"
#include "base/strings/utf_string_conversions.h"
#include <cstdio>
namespace uefi_browser {
#if defined(UEFI_ENABLE_MOZC)
CompositionUpdate TextComposer::Update(JapaneseUpdate update) {
  if (!update.error.empty()) {
    std::printf("IME: Mozc error: %.256s\n", update.error.c_str());
    Clear();
    return {};
  }
  preedit_ = base::UTF8ToUTF16(update.preedit);
  candidates_.clear();
  for (const auto& candidate : update.candidates)
    candidates_.push_back({candidate.id, base::UTF8ToUTF16(candidate.text), candidate.selected});
  // Mozc reports Unicode scalar offsets while Blink consumes UTF-16 offsets.
  cursor_ = 0;
  for (size_t character = 0; cursor_ < preedit_.size() && character < update.cursor; ++character) {
    char16_t first = preedit_[cursor_++];
    if (first >= 0xd800 && first <= 0xdbff && cursor_ < preedit_.size() &&
        preedit_[cursor_] >= 0xdc00 && preedit_[cursor_] <= 0xdfff) ++cursor_;
  }
  return {update.handled, base::UTF8ToUTF16(update.committed), preedit_};
}
#endif
CompositionUpdate TextComposer::Feed(const KeySample& sample) {
  if (mode_ != sample.input_method) { Clear(); mode_ = sample.input_method; }
#if defined(UEFI_ENABLE_MOZC)
  if (mode_ == InputMethod::Japanese) {
    JapaneseKey key;
    switch (sample.key.scan) {
      case 1: key = JapaneseKey::Up; break;
      case 2: key = JapaneseKey::Down; break;
      case 3: key = JapaneseKey::Right; break;
      case 4: key = JapaneseKey::Left; break;
      case 5: key = JapaneseKey::Home; break;
      case 6: key = JapaneseKey::End; break;
      case 8: key = JapaneseKey::Delete; break;
      case 0:
        switch (sample.key.character) {
          case 8: key = JapaneseKey::Backspace; break;
          case 9: key = JapaneseKey::Tab; break;
          case 13: key = JapaneseKey::Enter; break;
          case 32: key = JapaneseKey::Space; break;
          default:
            if (sample.key.character < 32 || sample.key.character > 126) return {};
            key = JapaneseKey::Character;
        }
        break;
      default: return {};
    }
    return Update(japanese_.Feed(key, sample.key.character));
  }
#endif
  CompositionUpdate update;
  if (mode_ == InputMethod::Korean && !sample.key.scan) {
    if (sample.key.character == 8) update = korean_.Backspace();
    else if (sample.key.character < 128) update = korean_.Feed(static_cast<char>(sample.key.character));
  }
  if (update.handled) { preedit_ = update.preedit; cursor_ = preedit_.size(); }
  return update;
}
CompositionUpdate TextComposer::Select(int id) {
#if defined(UEFI_ENABLE_MOZC)
  if (mode_ == InputMethod::Japanese) return Update(japanese_.Select(id));
#endif
  return {};
}
std::u16string TextComposer::Finish(bool cancel) {
  std::u16string committed = cancel ? std::u16string() : preedit_;
#if defined(UEFI_ENABLE_MOZC)
  if (!cancel && mode_ == InputMethod::Japanese && (!preedit_.empty() || !candidates_.empty())) {
    auto update = Update(japanese_.Finish());
    if (!update.committed.empty()) committed = update.committed;
  }
#endif
  Clear();
  return committed;
}
void TextComposer::Clear() {
  korean_.Clear();
#if defined(UEFI_ENABLE_MOZC)
  if (mode_ == InputMethod::Japanese && (!preedit_.empty() || !candidates_.empty())) japanese_.Clear();
#endif
  preedit_.clear();
  candidates_.clear();
  cursor_ = 0;
}
}

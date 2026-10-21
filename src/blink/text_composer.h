#ifndef UEFI_TEXT_COMPOSER_H
#define UEFI_TEXT_COMPOSER_H
#include "blink/input.h"
#include "blink/korean_input.h"
#include <vector>
#if defined(UEFI_ENABLE_MOZC)
#include "blink/japanese_input.h"
#endif
namespace uefi_browser {
struct InputCandidate { int id; std::u16string text; bool selected = false; };
class TextComposer {
 public:
  CompositionUpdate Feed(const KeySample& sample);
  CompositionUpdate Select(int id);
  std::u16string Finish(bool cancel);
  void Clear();
  const std::u16string& Preedit() const { return preedit_; }
  const std::vector<InputCandidate>& Candidates() const { return candidates_; }
  size_t Cursor() const { return cursor_; }
 private:
#if defined(UEFI_ENABLE_MOZC)
  CompositionUpdate Update(JapaneseUpdate update);
  JapaneseInput japanese_;
#endif
  KoreanInput korean_;
  InputMethod mode_ = InputMethod::Latin;
  std::u16string preedit_;
  std::vector<InputCandidate> candidates_;
  size_t cursor_ = 0;
};
}
#endif

#include "blink/korean_input.h"
#include "hangul.h"
namespace uefi_browser {
namespace {
std::u16string Utf16(const ucschar* text) {
  std::u16string result;
  if (!text) return result;
  for (; *text; ++text) {
    if (*text <= 0xffff && (*text < 0xd800 || *text > 0xdfff)) {
      result += static_cast<char16_t>(*text);
    } else if (*text >= 0x10000 && *text <= 0x10ffff) {
      result += static_cast<char16_t>(0xd800 + ((*text - 0x10000) >> 10));
      result += static_cast<char16_t>(0xdc00 + ((*text - 0x10000) & 0x3ff));
    } else result += u'\ufffd';
  }
  return result;
}
}
KoreanInput::~KoreanInput() {
  if (context_) hangul_ic_delete(context_);
}
void KoreanInput::Clear() {
  if (context_) hangul_ic_reset(context_);
  preedit_.clear();
}
CompositionUpdate KoreanInput::Read(bool handled) {
  preedit_ = Utf16(hangul_ic_get_preedit_string(context_));
  return {handled, Utf16(hangul_ic_get_commit_string(context_)), preedit_};
}
CompositionUpdate KoreanInput::Feed(char key) {
  // Navigation and punctuation are committed by the editor before normal dispatch.
  if (!((key >= 'a' && key <= 'z') || (key >= 'A' && key <= 'Z'))) return {};
  if (!context_) context_ = hangul_ic_new("2");
  if (!context_) return {};
  return Read(hangul_ic_process(context_, static_cast<unsigned char>(key)));
}
CompositionUpdate KoreanInput::Backspace() {
  if (!context_ || preedit_.empty()) return {};
  return Read(hangul_ic_backspace(context_));
}
}

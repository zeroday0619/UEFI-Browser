#ifndef UEFI_KOREAN_INPUT_H
#define UEFI_KOREAN_INPUT_H
#include <string>
struct _HangulInputContext;
namespace uefi_browser {
struct CompositionUpdate { bool handled=false; std::u16string committed, preedit; };
class KoreanInput {
 public:
  KoreanInput() = default;
  ~KoreanInput();
  KoreanInput(const KoreanInput&) = delete;
  KoreanInput& operator=(const KoreanInput&) = delete;
  const std::u16string& Preedit() const { return preedit_; }
  void Clear();
  CompositionUpdate Feed(char key);
  CompositionUpdate Backspace();
 private:
  CompositionUpdate Read(bool handled);
  _HangulInputContext* context_ = nullptr;
  std::u16string preedit_;
};
}
#endif

#ifndef UEFI_JAPANESE_INPUT_H
#define UEFI_JAPANESE_INPUT_H
#include <cstdint>
#include <memory>
#include <string>
#include <vector>
namespace uefi_browser {
enum class JapaneseKey { Character, Space, Enter, Backspace, Escape, Up, Down, Left, Right, Delete, Home, End, Tab };
struct JapaneseCandidate { int id; std::string text; bool selected = false; };
struct JapaneseUpdate {
  bool handled = false;
  std::string committed, preedit, error;
  size_t cursor = 0;
  std::vector<JapaneseCandidate> candidates;
};
class JapaneseInput {
 public:
  JapaneseInput();
  ~JapaneseInput();
  JapaneseInput(const JapaneseInput&) = delete;
  JapaneseInput& operator=(const JapaneseInput&) = delete;
  JapaneseUpdate Feed(JapaneseKey key, uint32_t character = 0);
  JapaneseUpdate Finish();
  JapaneseUpdate Select(int candidate);
  void Clear();
 private:
  class State;
  std::unique_ptr<State> state_;
};
}
#endif

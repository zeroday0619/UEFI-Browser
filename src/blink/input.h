#ifndef UEFI_BLINK_INPUT_H_
#define UEFI_BLINK_INPUT_H_
#include <memory>
#include "uefi.h"
namespace uefi_browser {
struct PointerSample { int x, y, wheel; unsigned buttons; bool wheel_consumed=false; };
enum class InputMethod : uint8_t { Latin, Korean, Japanese };
enum class CompositionAction : uint8_t { None, Finish };
struct KeySample { Key key; uint32_t shift; uint8_t toggle;
  InputMethod input_method=InputMethod::Latin;
  CompositionAction composition_action=CompositionAction::None;
};
class FirmwareInput {
 public:
  FirmwareInput(SystemTable*, int width, int height);
  ~FirmwareInput();
  bool ReadPointer(PointerSample&);
  bool PeekPointer(PointerSample&);
  void SetPointerObserver(void (*observer)(void*, int, int), void* context);
  bool ReadKey(KeySample&);
 private:
  class State;
  std::unique_ptr<State> state_;
};
}
#endif

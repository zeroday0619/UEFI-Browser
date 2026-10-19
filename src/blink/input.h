#ifndef UEFI_BLINK_INPUT_H_
#define UEFI_BLINK_INPUT_H_
#include <memory>
#include "uefi.h"
namespace uefi_browser {
struct PointerSample { int x, y, wheel; unsigned buttons; bool wheel_consumed=false; };
struct KeySample { Key key; uint32_t shift; uint8_t toggle; };
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

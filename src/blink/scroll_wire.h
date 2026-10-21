#ifndef UEFI_SCROLL_WIRE_H
#define UEFI_SCROLL_WIRE_H
#include "blink/scroll_snapshot.h"
#include "third_party/skia/include/core/SkImage.h"
namespace uefi_browser {
// The mailbox carries only bounded value records and compressed pixels.
constexpr size_t kScrollWireCapacity=16*1024*1024;
// The encoder owns the immutable scene until completion or cancellation.
class ScrollWireEncoder {
 public:
  explicit ScrollWireEncoder(std::shared_ptr<const ScrollScene>);
  ~ScrollWireEncoder();
  bool EncodeSlice(size_t maximum_steps=64);
  std::vector<uint8_t> TakeBytes();
 private:
  struct State;
  std::unique_ptr<State> state_;
};
std::vector<uint8_t> EncodeScrollScene(const ScrollScene&);
std::shared_ptr<ScrollScene> DecodeScrollScene(const uint8_t*,size_t);
sk_sp<SkImage> DecodeScrollTile(const ScrollRaster&,int x,int y);
bool ScrollWireMemoryAvailable(size_t);
bool TestScrollWire();
}
#endif

#ifndef UEFI_SCROLL_WIRE_H
#define UEFI_SCROLL_WIRE_H
#include "blink/scroll_snapshot.h"
#include "third_party/skia/include/core/SkImage.h"
namespace uefi_browser {
// The mailbox carries only bounded value records and compressed pixels.
constexpr size_t kScrollWireCapacity=16*1024*1024;
std::vector<uint8_t> EncodeScrollScene(const ScrollScene&);
std::shared_ptr<ScrollScene> DecodeScrollScene(const uint8_t*,size_t);
sk_sp<SkImage> DecodeScrollTile(const ScrollRaster&,int x,int y);
bool ScrollWireMemoryAvailable(size_t);
bool TestScrollWire();
}
#endif

#ifndef UEFI_BROWSER_MEDIA_PLAYER_H
#define UEFI_BROWSER_MEDIA_PLAYER_H
#include <memory>
namespace blink {
class WebLocalFrame;
class WebMediaPlayer;
class WebMediaPlayerClient;
}  // namespace blink
namespace uefi_browser {
std::unique_ptr<blink::WebMediaPlayer> CreateMediaPlayer(
    blink::WebLocalFrame* frame,
    blink::WebMediaPlayerClient* client);
}
#endif

#ifndef UEFI_REMOTE_FRAME_H
#define UEFI_REMOTE_FRAME_H
#include "blink/browser_frame.h"
namespace uefi_browser {
bool StartRenderer(SystemTable*, int width, int height);
void StopRenderer();
void TerminateRenderer();
bool PollRemoteFrames();
bool RemoteRendererConfigured();
int RunRenderer(SystemTable*, blink::scheduler::WebThreadScheduler&);
bool PollRenderer(void*);
class RemoteFrame {
 public:
  RemoteFrame(SkCanvas&, gfx::Size, base::RepeatingClosure,
              base::RepeatingCallback<void(std::string)>);
  ~RemoteFrame();
  Status Navigate(const char*);
  bool NavigationPending() const;
  Status NavigationStatus() const;
  std::string CurrentUrl() const;
  bool CanGoBack() const;
  bool CanGoForward() const;
  Status GoBack();
  Status GoForward();
  Status Reload();
  bool CanAcceptInput() const;
  void Pointer(const PointerSample&);
  void PointerLeave();
  void Keyboard(const KeySample&);
  void SetVisible(bool);
  void Poll();
  void Recover();
 private:
  class State;
  std::unique_ptr<State> state_;
};
}
#endif

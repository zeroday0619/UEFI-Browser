#ifndef UEFI_DEVTOOLS_H
#define UEFI_DEVTOOLS_H
#include <memory>
namespace blink { class WebLocalFrame; }
namespace uefi_browser {
class DevToolsTarget {
 public:
  explicit DevToolsTarget(blink::WebLocalFrame*);
  ~DevToolsTarget();
 private:
  friend class DevToolsFrontend;
  class State;
  std::unique_ptr<State> state_;
};
class DevToolsFrontend {
 public:
  DevToolsFrontend(blink::WebLocalFrame*, DevToolsTarget&);
  ~DevToolsFrontend();
 private:
  class State;
  std::unique_ptr<State> state_;
};
}
#endif

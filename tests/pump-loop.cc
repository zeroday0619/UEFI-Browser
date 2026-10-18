#include "blink/pump_loop.h"
#include <cassert>
#include <cstdio>
#include <functional>
using uefi_browser::PumpLoop;
struct Next { bool immediate = false; bool is_immediate() const { return immediate; } };
struct Delegate {
  std::function<Next()> work;
  std::function<void()> idle = [] {};
  std::function<void()> before = [] {};
  Next DoWork() { return work(); }
  void DoIdleWork() { idle(); }
  void BeforeWait() { before(); }
};
int main() {
  PumpLoop loop;
  int work = 0, idle = 0, waits = 0;
  Delegate delegate;
  delegate.work = [&] { if (++work == 4) loop.Quit(); return Next{work < 3}; };
  delegate.idle = [&] { ++idle; };
  loop.Run(delegate, [] { return false; }, [&](Next) { ++waits; });
  assert(work == 4 && idle == 1 && waits == 1);

  // Posting from idle must not sleep until the next native polling interval.
  work = idle = waits = 0;
  delegate.work = [&] { if (++work == 2) loop.Quit(); return Next{}; };
  delegate.idle = [&] { ++idle; loop.ScheduleWork(); };
  loop.Run(delegate, [] { return false; }, [&](Next) { ++waits; });
  assert(work == 2 && idle == 1 && waits == 0);

  // A post made from BeforeWait must also avoid the upcoming sleep.
  work = waits = 0;
  delegate.idle = [] {};
  delegate.before = [&] { loop.ScheduleWork(); };
  loop.Run(delegate, [] { return false; }, [&](Next) { ++waits; });
  assert(work == 2 && waits == 0);

  // Quitting a nested loop must preserve the outer loop's execution state.
  work = 0;
  int nested = 0;
  delegate.before = [] {};
  delegate.work = [&] {
    ++work;
    if (work == 1) {
      Delegate inner;
      inner.work = [&] { ++nested; loop.Quit(); return Next{}; };
      loop.Run(inner, [] { return false; }, [](Next) { assert(false); });
    } else { loop.Quit(); }
    return Next{true};
  };
  loop.Run(delegate, [] { return false; }, [](Next) { assert(false); });
  assert(work == 2 && nested == 1);

  // Native completion can quit without executing another application task.
  delegate.work = [] { assert(false); return Next{}; };
  loop.Run(delegate, [&] { loop.Quit(); return true; }, [](Next) { assert(false); });

  // Idle cancellation must prevent both BeforeWait and firmware waiting.
  delegate.work = [] { return Next{}; };
  delegate.idle = [&] { loop.Quit(); };
  delegate.before = [] { assert(false); };
  loop.Run(delegate, [] { return false; }, [](Next) { assert(false); });
  std::puts("PASS: pump immediate, idle wake, pre-wait wake, nested quit, native quit, idle quit");
}

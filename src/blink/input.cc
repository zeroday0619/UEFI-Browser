#include "blink/input.h"
#include <algorithm>
#include <cstdio>
namespace uefi_browser {
namespace {
struct RelativeState { int32_t x, y, z; uint8_t left, right; };
struct RelativeMode { uint64_t x, y, z; uint8_t left, right; };
struct RelativePointer {
  Status(UEFI_API *reset)(RelativePointer*, uint8_t);
  Status(UEFI_API *read)(RelativePointer*, RelativeState*);
  void* wait;
  RelativeMode* mode;
};
struct AbsoluteState { uint64_t x, y, z; uint32_t buttons; };
struct AbsoluteMode { uint64_t minimum_x, minimum_y, minimum_z, maximum_x, maximum_y, maximum_z; uint32_t attributes; };
struct AbsolutePointer {
  Status(UEFI_API *reset)(AbsolutePointer*, uint8_t);
  Status(UEFI_API *read)(AbsolutePointer*, AbsoluteState*);
  void* wait;
  AbsoluteMode* mode;
};
struct ExtendedKey { Key key; uint32_t shift; uint8_t toggle; };
struct ExtendedInput {
  void* reset;
  Status(UEFI_API *read)(ExtendedInput*, ExtendedKey*);
  void* wait;
  Status(UEFI_API *set_state)(ExtendedInput*, uint8_t*);
};
static_assert(sizeof(RelativeState) == 16 && sizeof(AbsoluteState) == 32);
static_assert(sizeof(ExtendedKey) == 12 && offsetof(ExtendedKey, shift) == 4);
int Coordinate(uint64_t value, uint64_t minimum, uint64_t maximum, int extent) {
  if (maximum <= minimum || value <= minimum) return 0;
  if (value >= maximum) return extent - 1;
  return static_cast<int>(static_cast<double>(value - minimum) /
      static_cast<double>(maximum - minimum) * (extent - 1));
}
}
class FirmwareInput::State {
 public:
  State(SystemTable* system, int width, int height)
      : table(system), width(width), height(height), x(width / 2), y(height / 2) {
    auto handles = reinterpret_cast<Status(UEFI_API *)(int, Guid*, void*, size_t*, Handle**)>(table->boot->function[36]);
    auto connect = reinterpret_cast<Status(UEFI_API *)(Handle, Handle*, void*, uint8_t)>(table->boot->function[30]);
    auto release = reinterpret_cast<Status(UEFI_API *)(void*)>(table->boot->function[6]);
    auto protocol = reinterpret_cast<Status(UEFI_API *)(Handle, Guid*, void**)>(table->boot->function[16]);
    Handle* list = nullptr;
    size_t count = 0;
    // Disk boot can leave USB input controllers disconnected.
    if (!handles(0, nullptr, nullptr, &count, &list)) {
      for (size_t index = 0; index < count; ++index) connect(list[index], nullptr, nullptr, 1);
      release(list);
    }
    Guid relative_guid = {0x31878c87,0x0b75,0x11d5,{0x9a,0x4f,0,0x90,0x27,0x3f,0xc1,0x4d}};
    Guid absolute_guid = {0x8d59d32b,0xc655,0x4ae9,{0x9b,0x15,0xf2,0x59,4,0x99,0x2a,0x43}};
    for (int kind = 0; kind < 2; ++kind) {
      Guid* identifier = kind ? &absolute_guid : &relative_guid;
      if (handles(2, identifier, nullptr, &count, &list)) continue;
      for (size_t index = 0; index < count; ++index) {
        void* interface = nullptr;
        if (list[index] == table->input_handle || protocol(list[index], identifier, &interface)) continue;
        if (kind) absolute = static_cast<AbsolutePointer*>(interface);
        else {
          auto* candidate = static_cast<RelativePointer*>(interface);
          // A wheel-capable USB mouse takes precedence over the PS/2 mouse.
          if (!relative || candidate->mode->z) relative = candidate;
        }
      }
      release(list);
    }
    Guid extended_guid = {0xdd9e7534,0x7762,0x4698,{0x8c,0x14,0xf5,0x85,0x17,0xa6,0x25,0xaa}};
    protocol(table->input_handle, &extended_guid, reinterpret_cast<void**>(&extended));
    if (relative) relative->reset(relative, 0);
    if (absolute) absolute->reset(absolute, 0);
    auto create = reinterpret_cast<Status(UEFI_API *)(uint32_t, size_t, void(UEFI_API *)(void*, void*), void*, void**)>(table->boot->function[7]);
    auto schedule = reinterpret_cast<Status(UEFI_API *)(void*, int, uint64_t)>(table->boot->function[8]);
    if (!create(0x80000200, 8, CaptureTimer, this, &timer)) schedule(timer, 1, 50000);
    std::printf("INPUT: relative=%d absolute=%d extended-keyboard=%d wheel=%llu\n",
        relative != nullptr, absolute != nullptr, extended != nullptr,
        static_cast<unsigned long long>(relative ? relative->mode->z : 0));
  }
  ~State() {
    if (timer) {
      reinterpret_cast<Status(UEFI_API *)(void*, int, uint64_t)>(table->boot->function[8])(timer, 0, 0);
      reinterpret_cast<Status(UEFI_API *)(void*)>(table->boot->function[11])(timer);
    }
  }
  static void UEFI_API CaptureTimer(void*, void* context) {
    static_cast<State*>(context)->Capture(false);
  }
  void Capture(bool pump) {
  auto& state = *this;
  auto check = reinterpret_cast<Status(UEFI_API *)(void*)>(state.table->boot->function[12]);
  bool changed = false;
  int wheel = 0;
  AbsoluteState absolute{};
  if (state.absolute) {
    if (pump && state.absolute->wait) check(state.absolute->wait);
    if (!state.absolute->read(state.absolute, &absolute)) {
      state.x = Coordinate(absolute.x, state.absolute->mode->minimum_x, state.absolute->mode->maximum_x, state.width);
      state.y = Coordinate(absolute.y, state.absolute->mode->minimum_y, state.absolute->mode->maximum_y, state.height);
      state.buttons = absolute.buttons & 3;
      changed = true;
    }
  }
  RelativeState relative{};
  if (state.relative) {
    if (pump && state.relative->wait) check(state.relative->wait);
    if (!state.relative->read(state.relative, &relative)) {
      if (!state.absolute) {
        state.x = std::clamp(state.x + std::clamp(relative.x, -state.width, state.width), 0, state.width - 1);
        state.y = std::clamp(state.y + std::clamp(relative.y, -state.height, state.height), 0, state.height - 1);
        state.buttons = !!relative.left | (!!relative.right << 1);
      }
      wheel = std::clamp(relative.z, -16, 16);
      changed = true;
    }
  }
    if (changed) {
      captured = true;
      if (observer) observer(observer_context, x, y);
      // Merge only motion runs; the first sample after a button edge remains intact.
      if (!wheel && count >= 2) {
        auto& last = samples[(head + count - 1) % 256];
        const auto& previous = samples[(head + count - 2) % 256];
        if (!last.wheel && !previous.wheel && last.buttons == buttons &&
            previous.buttons == buttons) {
          last.x = x;
          last.y = y;
          return;
        }
      }
      // A bounded queue preserves button transitions while Blink paints.
      if (count == 256) { head = (head + 1) % 256; --count; }
      samples[(head + count) % 256] = {x, y, wheel, buttons};
      ++count;
    }
  }
  void* timer = nullptr;
  PointerSample samples[256]{};
  unsigned head = 0, count = 0;
  unsigned delivered_buttons = 0;
  bool captured = false;
  void (*observer)(void*, int, int) = nullptr;
  void* observer_context = nullptr;
  SystemTable* table;
  int width, height, x, y;
  unsigned buttons = 0;
  RelativePointer* relative = nullptr;
  AbsolutePointer* absolute = nullptr;
  ExtendedInput* extended = nullptr;
};
FirmwareInput::FirmwareInput(SystemTable* table, int width, int height)
    : state_(std::make_unique<State>(table, width, height)) {}
FirmwareInput::~FirmwareInput() = default;
bool FirmwareInput::ReadPointer(PointerSample& result) {
  auto& state = *state_;
  auto raise = reinterpret_cast<size_t(UEFI_API *)(size_t)>(state.table->boot->function[0]);
  auto restore = reinterpret_cast<void(UEFI_API *)(size_t)>(state.table->boot->function[1]);
  size_t previous = raise(8);
  state.Capture(true);
  bool available = state.count != 0;
  if (available) {
    result = state.samples[state.head];
    state.head = (state.head + 1) % 256;
    --state.count;
    // Motion may be coalesced only between button transitions and wheel events.
    if (!result.wheel && result.buttons == state.delivered_buttons) {
      while (state.count) {
        const auto& next = state.samples[state.head];
        if (next.wheel || next.buttons != result.buttons) break;
        result = next;
        state.head = (state.head + 1) % 256;
        --state.count;
      }
    }
    state.delivered_buttons = result.buttons;
  }
  restore(previous);
  return available;
}

void FirmwareInput::SetPointerObserver(void (*observer)(void*, int, int), void* context) {
  auto raise = reinterpret_cast<size_t(UEFI_API *)(size_t)>(state_->table->boot->function[0]);
  auto restore = reinterpret_cast<void(UEFI_API *)(size_t)>(state_->table->boot->function[1]);
  size_t previous = raise(8);
  state_->observer_context = context;
  state_->observer = observer;
  restore(previous);
}

bool FirmwareInput::PeekPointer(PointerSample& result) {
  auto& state = *state_;
  auto raise = reinterpret_cast<size_t(UEFI_API *)(size_t)>(state.table->boot->function[0]);
  auto restore = reinterpret_cast<void(UEFI_API *)(size_t)>(state.table->boot->function[1]);
  size_t previous = raise(8);
  state.Capture(true);
  result = {state.x, state.y, 0, state.buttons};
  bool available = state.captured;
  restore(previous);
  return available;
}

bool FirmwareInput::ReadKey(KeySample& result) {
  if (state_->extended) {
    ExtendedKey key{};
    if (state_->extended->read(state_->extended, &key)) return false;
    result = {key.key, key.shift, key.toggle};
  } else {
    Key key{};
    if (state_->table->input->read(state_->table->input, &key)) return false;
    result = {key, 0, 0};
  }
  return true;
}
}

#pragma once

#include "lanlink/common/ids.hpp"

#include <cstdint>
#include <variant>

namespace lanlink {

struct MouseDelta {
    double dx{};
    double dy{};
    constexpr MouseDelta() = default;
    constexpr MouseDelta(double x, double y) : dx(x), dy(y) {}
    constexpr bool zero() const noexcept { return dx == 0.0 && dy == 0.0; }
};

struct MouseButton {
    union {
        std::uint32_t button;
        std::uint32_t code;
    };
    bool pressed{};
    constexpr MouseButton(std::uint32_t value = 0, bool down = false)
        : button(value), pressed(down) {}
};

struct MouseWheel {
    union {
        double delta;
        double amount;
    };
    constexpr MouseWheel(double value = 0.0) : delta(value) {}
    constexpr bool zero() const noexcept { return delta == 0.0; }
};

struct Key {
    union {
        std::uint32_t code;
        std::uint32_t key;
    };
    bool pressed{};
  // `code` is a USB HID usage, never an OS-specific virtual-key or keycode.
  constexpr Key(std::uint32_t value = 0, bool down = false) : code(value), pressed(down) {}
};

struct PointerWarp {
    double x{};
    double y{};
    constexpr PointerWarp() = default;
    constexpr PointerWarp(double x_value, double y_value) : x(x_value), y(y_value) {}
};

// An absolute move follows the runtime's virtual cursor. Unlike PointerWarp,
// which is a control-plane synchronization operation used during handoff,
// this is a regular remote motion event and must be injected as a screen
// coordinate on the target desktop.
struct PointerMove {
    double x{};
    double y{};
    constexpr PointerMove() = default;
    constexpr PointerMove(double x_value, double y_value) : x(x_value), y(y_value) {}
};

using Payload = std::variant<MouseDelta, MouseButton, MouseWheel, Key, PointerWarp, PointerMove>;

enum class InputOrigin {
    kPhysical,
    kRemoteInjected,
    kSynthetic,
    Physical = kPhysical,
    RemoteInjected = kRemoteInjected,
    Synthetic = kSynthetic,
};

struct InputEvent {
    SourceId source;
    SessionId session;
    std::uint64_t generation{};
    std::uint64_t seq{};
    Payload payload;
    InputOrigin origin{InputOrigin::kRemoteInjected};
};

} // namespace lanlink

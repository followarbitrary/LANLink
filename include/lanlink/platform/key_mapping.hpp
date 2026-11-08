#pragma once

#include <cstdint>
#include <optional>

namespace lanlink::platform {

struct WindowsKeyStroke {
  std::uint16_t scan_code{};
  bool extended{false};
};

enum class MacModifierFlag : std::uint8_t {
  kAlphaShift,
  kShift,
  kControl,
  kAlternate,
  kCommand,
};

enum class MacMouseMotionKind : std::uint8_t {
  kMoved,
  kLeftDragged,
  kRightDragged,
  kOtherDragged,
};

// The wire-level Key::code is a USB HID usage, independent of keyboard
// layout and operating-system virtual-key numbering.
// Cross-platform modifier semantics are preserved by the HID layer:
// macOS Command <-> Windows GUI/Win, macOS Option <-> Windows Alt, while
// Control, Shift, Caps Lock, Enter, Backspace, arrows, and function keys keep
// their corresponding HID usages.
std::optional<std::uint32_t> windows_vk_to_hid(std::uint32_t vk);
std::optional<std::uint32_t> hid_to_windows_vk(std::uint32_t usage);
std::optional<std::uint32_t> mac_keycode_to_hid(std::uint32_t keycode);
std::optional<std::uint32_t> hid_to_mac_keycode(std::uint32_t usage);
std::optional<MacModifierFlag> mac_modifier_flag_for_hid(std::uint32_t usage);
// The button mask uses bit 0 for button 1, bit 1 for button 2, and bit 2 for
// button 3. macOS needs a dragged event while any corresponding button is held.
MacMouseMotionKind mac_mouse_motion_kind_for_buttons(std::uint32_t buttons);

// Helpers used when releasing injected keys and placing the pointer on native backends.
std::optional<std::uint32_t> windows_release_vk_for_hid(std::uint32_t usage);
std::optional<std::uint32_t> mac_release_keycode_for_hid(std::uint32_t usage);
// Return the Set-1 scan code information required by SendInput for keys whose
// left/right or extended identity cannot be preserved by wVk alone.
std::optional<WindowsKeyStroke> windows_key_stroke_for_hid(std::uint32_t usage);
std::int32_t windows_absolute_mouse_coordinate(double coordinate, std::int32_t origin,
                                               std::int32_t extent);

}  // namespace lanlink::platform

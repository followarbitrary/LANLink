#pragma once

#include <cstdint>
#include <optional>

namespace lanlink::platform {

// The wire-level Key::code is a USB HID usage, independent of keyboard
// layout and operating-system virtual-key numbering.
std::optional<std::uint32_t> windows_vk_to_hid(std::uint32_t vk);
std::optional<std::uint32_t> hid_to_windows_vk(std::uint32_t usage);
std::optional<std::uint32_t> mac_keycode_to_hid(std::uint32_t keycode);
std::optional<std::uint32_t> hid_to_mac_keycode(std::uint32_t usage);

// Helpers used when releasing injected keys and placing the pointer on native backends.
std::optional<std::uint32_t> windows_release_vk_for_hid(std::uint32_t usage);
std::optional<std::uint32_t> mac_release_keycode_for_hid(std::uint32_t usage);
std::int32_t windows_absolute_mouse_coordinate(double coordinate, std::int32_t origin,
                                               std::int32_t extent);

}  // namespace lanlink::platform

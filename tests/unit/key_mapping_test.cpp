#include "lanlink/platform/key_mapping.hpp"

#include <gtest/gtest.h>

#include <cstdint>
#include <utility>

namespace lanlink::platform {
namespace {

TEST(KeyMapping, WindowsVirtualKeysMapToCanonicalHidUsages) {
  EXPECT_EQ(windows_vk_to_hid(0x41).value(), 0x04u);  // A
  EXPECT_EQ(windows_vk_to_hid(0x5A).value(), 0x1Du);  // Z
  EXPECT_EQ(windows_vk_to_hid(0x30).value(), 0x27u);  // 0
  EXPECT_EQ(windows_vk_to_hid(0x39).value(), 0x26u);  // 9
  EXPECT_EQ(windows_vk_to_hid(0x0D).value(), 0x28u);  // Enter
  EXPECT_EQ(windows_vk_to_hid(0x1B).value(), 0x29u);  // Escape
  EXPECT_EQ(windows_vk_to_hid(0x08).value(), 0x2Au);  // Backspace
  EXPECT_EQ(windows_vk_to_hid(0x09).value(), 0x2Bu);  // Tab
  EXPECT_EQ(windows_vk_to_hid(0x20).value(), 0x2Cu);  // Space
  EXPECT_EQ(windows_vk_to_hid(0xA0).value(), 0xE1u);  // Left Shift
  EXPECT_EQ(windows_vk_to_hid(0xA1).value(), 0xE5u);  // Right Shift
  EXPECT_EQ(windows_vk_to_hid(0xA2).value(), 0xE0u);  // Left Control
  EXPECT_EQ(windows_vk_to_hid(0xA3).value(), 0xE4u);  // Right Control
  EXPECT_EQ(windows_vk_to_hid(0xA4).value(), 0xE2u);  // Left Alt
  EXPECT_EQ(windows_vk_to_hid(0xA5).value(), 0xE6u);  // Right Alt
  EXPECT_EQ(windows_vk_to_hid(0x5B).value(), 0xE3u);  // Left GUI
  EXPECT_EQ(windows_vk_to_hid(0x5C).value(), 0xE7u);  // Right GUI
  EXPECT_EQ(windows_vk_to_hid(0x70).value(), 0x3Au);  // F1
  EXPECT_EQ(windows_vk_to_hid(0x7B).value(), 0x45u);  // F12
}

TEST(KeyMapping, WindowsNavigationFunctionAndModifierKeysMapToCanonicalHidUsages) {
  const std::pair<std::uint32_t, std::uint32_t> mappings[] = {
      {0x14, 0x39},  // Caps Lock
      {0x0D, 0x28},  // Enter
      {0x08, 0x2A},  // Backspace
      {0x25, 0x50},  // Left
      {0x26, 0x52},  // Up
      {0x27, 0x4F},  // Right
      {0x28, 0x51},  // Down
      {0xA0, 0xE1},  // Left Shift
      {0xA1, 0xE5},  // Right Shift
      {0xA2, 0xE0},  // Left Control
      {0xA3, 0xE4},  // Right Control
      {0xA4, 0xE2},  // Left Option/Alt
      {0xA5, 0xE6},  // Right Option/Alt
      {0x5B, 0xE3},  // Left Command/GUI
      {0x5C, 0xE7},  // Right Command/GUI
  };
  for (const auto [vk, hid] : mappings) EXPECT_EQ(windows_vk_to_hid(vk).value(), hid);

  for (std::uint32_t index = 0; index < 12; ++index)
    EXPECT_EQ(windows_vk_to_hid(0x70u + index).value(), 0x3Au + index);
}

TEST(KeyMapping, MacKeyCodesMapToCanonicalHidUsages) {
  EXPECT_EQ(mac_keycode_to_hid(0).value(), 0x04u);    // A
  EXPECT_EQ(mac_keycode_to_hid(6).value(), 0x1Du);    // Z
  EXPECT_EQ(mac_keycode_to_hid(29).value(), 0x27u);   // 0
  EXPECT_EQ(mac_keycode_to_hid(18).value(), 0x1Eu);   // 1
  EXPECT_EQ(mac_keycode_to_hid(36).value(), 0x28u);   // Enter
  EXPECT_EQ(mac_keycode_to_hid(53).value(), 0x29u);   // Escape
  EXPECT_EQ(mac_keycode_to_hid(51).value(), 0x2Au);   // Backspace
  EXPECT_EQ(mac_keycode_to_hid(48).value(), 0x2Bu);   // Tab
  EXPECT_EQ(mac_keycode_to_hid(49).value(), 0x2Cu);   // Space
  EXPECT_EQ(mac_keycode_to_hid(56).value(), 0xE1u);   // Left Shift
  EXPECT_EQ(mac_keycode_to_hid(60).value(), 0xE5u);   // Right Shift
  EXPECT_EQ(mac_keycode_to_hid(59).value(), 0xE0u);   // Left Control
  EXPECT_EQ(mac_keycode_to_hid(62).value(), 0xE4u);   // Right Control
  EXPECT_EQ(mac_keycode_to_hid(58).value(), 0xE2u);   // Left Option
  EXPECT_EQ(mac_keycode_to_hid(61).value(), 0xE6u);   // Right Option
  EXPECT_EQ(mac_keycode_to_hid(55).value(), 0xE3u);   // Left Command
  EXPECT_EQ(mac_keycode_to_hid(54).value(), 0xE7u);   // Right Command
  EXPECT_EQ(mac_keycode_to_hid(96).value(), 0x3Eu);   // F5
  EXPECT_EQ(mac_keycode_to_hid(111).value(), 0x45u);  // F12
}

TEST(KeyMapping, MacNavigationFunctionAndModifierKeysMapToCanonicalHidUsages) {
  const std::pair<std::uint32_t, std::uint32_t> mappings[] = {
      {57, 0x39},   // Caps Lock
      {36, 0x28},   // Enter
      {51, 0x2A},   // Backspace
      {123, 0x50},  // Left
      {126, 0x52},  // Up
      {124, 0x4F},  // Right
      {125, 0x51},  // Down
      {56, 0xE1},   // Left Shift
      {60, 0xE5},   // Right Shift
      {59, 0xE0},   // Left Control
      {62, 0xE4},   // Right Control
      {58, 0xE2},   // Left Option
      {61, 0xE6},   // Right Option
      {55, 0xE3},   // Left Command
      {54, 0xE7},   // Right Command
  };
  for (const auto [keycode, hid] : mappings)
    EXPECT_EQ(mac_keycode_to_hid(keycode).value(), hid);

  const std::uint32_t keycodes[] = {122, 120, 99, 118, 96, 97,
                                    98,  100, 101, 109, 103, 111};
  for (std::uint32_t index = 0; index < 12; ++index)
    EXPECT_EQ(mac_keycode_to_hid(keycodes[index]).value(), 0x3Au + index);
}

TEST(KeyMapping, InverseMappingsRoundTripCommonUsages) {
  constexpr std::uint32_t usages[] = {0x04u, 0x1Du, 0x1Eu, 0x27u, 0x28u, 0x29u,
                                      0x2Au, 0x2Bu, 0x2Cu, 0x3Au, 0x45u, 0xE0u, 0xE1u,
                                      0xE2u, 0xE3u, 0xE4u, 0xE5u, 0xE6u, 0xE7u};
  for (const auto usage : usages) {
    ASSERT_TRUE(hid_to_windows_vk(usage).has_value()) << usage;
    ASSERT_TRUE(hid_to_mac_keycode(usage).has_value()) << usage;
    EXPECT_EQ(windows_vk_to_hid(*hid_to_windows_vk(usage)), usage);
    EXPECT_EQ(mac_keycode_to_hid(*hid_to_mac_keycode(usage)), usage);
  }
}

TEST(KeyMapping, UnknownValuesAreRejected) {
  EXPECT_FALSE(windows_vk_to_hid(0xFFFFu).has_value());
  EXPECT_FALSE(mac_keycode_to_hid(0xFFFFu).has_value());
  EXPECT_FALSE(hid_to_windows_vk(0x00u).has_value());
  EXPECT_FALSE(hid_to_mac_keycode(0x00u).has_value());
  EXPECT_FALSE(hid_to_windows_vk(0xFFFFu).has_value());
  EXPECT_FALSE(hid_to_mac_keycode(0xFFFFu).has_value());
}

TEST(KeyMapping, ReleaseHelpersConvertCanonicalHidUsages) {
  EXPECT_EQ(windows_release_vk_for_hid(0xE1u).value(), 0xA0u);
  EXPECT_EQ(windows_release_vk_for_hid(0x04u).value(), 0x41u);
  EXPECT_EQ(mac_release_keycode_for_hid(0xE1u).value(), 56u);
  EXPECT_EQ(mac_release_keycode_for_hid(0x04u).value(), 0u);
  EXPECT_FALSE(windows_release_vk_for_hid(0xFFFFu).has_value());
  EXPECT_FALSE(mac_release_keycode_for_hid(0xFFFFu).has_value());
}

TEST(KeyMapping, WindowsAbsoluteCoordinatesUseVirtualDesktopOrigin) {
  EXPECT_EQ(windows_absolute_mouse_coordinate(-1920.0, -1920, 3840), 0);
  EXPECT_EQ(windows_absolute_mouse_coordinate(0.0, -1920, 3840), 32776);
  EXPECT_EQ(windows_absolute_mouse_coordinate(1919.0, -1920, 3840), 65535);
}

TEST(KeyMapping, ExtendedFunctionAndKeypadKeysRoundTrip) {
  EXPECT_EQ(windows_vk_to_hid(0x7C).value(), 0x68u);  // F13
  EXPECT_EQ(windows_vk_to_hid(0x87).value(), 0x73u);  // F24
  EXPECT_EQ(mac_keycode_to_hid(105).value(), 0x68u);  // F13
  EXPECT_EQ(mac_keycode_to_hid(64).value(), 0x6Cu);   // F17

  EXPECT_EQ(windows_vk_to_hid(0x60).value(), 0x62u);  // keypad 0
  EXPECT_EQ(windows_vk_to_hid(0x61).value(), 0x59u);  // keypad 1
  EXPECT_EQ(windows_vk_to_hid(0x6F).value(), 0x54u);  // keypad /
  EXPECT_EQ(windows_vk_to_hid(0x6E).value(), 0x63u);  // keypad .
  EXPECT_EQ(mac_keycode_to_hid(82).value(), 0x62u);    // keypad 0
  EXPECT_EQ(mac_keycode_to_hid(83).value(), 0x59u);    // keypad 1
  EXPECT_EQ(mac_keycode_to_hid(75).value(), 0x54u);    // keypad /
  EXPECT_EQ(mac_keycode_to_hid(65).value(), 0x63u);    // keypad .
  EXPECT_EQ(mac_keycode_to_hid(76).value(), 0x58u);    // keypad Enter

  EXPECT_EQ(hid_to_windows_vk(0x68u).value(), 0x7Cu);
  EXPECT_EQ(hid_to_mac_keycode(0x68u).value(), 105u);
  EXPECT_EQ(hid_to_windows_vk(0x62u).value(), 0x60u);
  EXPECT_EQ(hid_to_mac_keycode(0x62u).value(), 82u);
  EXPECT_EQ(hid_to_mac_keycode(0x58u).value(), 76u);
}

TEST(KeyMapping, WindowsSpecialKeysUseDistinctScanCodesAndExtendedFlags) {
  const auto left_control = windows_key_stroke_for_hid(0xE0u);
  const auto right_control = windows_key_stroke_for_hid(0xE4u);
  const auto left_command = windows_key_stroke_for_hid(0xE3u);
  const auto right_command = windows_key_stroke_for_hid(0xE7u);
  ASSERT_TRUE(left_control.has_value());
  ASSERT_TRUE(right_control.has_value());
  ASSERT_TRUE(left_command.has_value());
  ASSERT_TRUE(right_command.has_value());
  EXPECT_EQ(left_control->scan_code, 0x1Du);
  EXPECT_FALSE(left_control->extended);
  EXPECT_EQ(right_control->scan_code, 0x1Du);
  EXPECT_TRUE(right_control->extended);
  EXPECT_EQ(left_command->scan_code, 0x5Bu);
  EXPECT_TRUE(left_command->extended);
  EXPECT_EQ(right_command->scan_code, 0x5Cu);
  EXPECT_TRUE(right_command->extended);

  const auto left = windows_key_stroke_for_hid(0x50u);
  const auto up = windows_key_stroke_for_hid(0x52u);
  const auto f12 = windows_key_stroke_for_hid(0x45u);
  const auto keypad_enter = windows_key_stroke_for_hid(0x58u);
  ASSERT_TRUE(left.has_value());
  ASSERT_TRUE(up.has_value());
  ASSERT_TRUE(f12.has_value());
  ASSERT_TRUE(keypad_enter.has_value());
  EXPECT_EQ(left->scan_code, 0x4Bu);
  EXPECT_TRUE(left->extended);
  EXPECT_EQ(up->scan_code, 0x48u);
  EXPECT_TRUE(up->extended);
  EXPECT_EQ(f12->scan_code, 0x58u);
  EXPECT_FALSE(f12->extended);
  EXPECT_EQ(keypad_enter->scan_code, 0x1Cu);
  EXPECT_TRUE(keypad_enter->extended);
  EXPECT_EQ(hid_to_windows_vk(0x58u).value(), 0x0Du);
}

}  // namespace
}  // namespace lanlink::platform

#include "lanlink/platform/key_mapping.hpp"

#include <gtest/gtest.h>

#include <cstdint>

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

}  // namespace
}  // namespace lanlink::platform

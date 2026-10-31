#include "lanlink/protocol/wire.hpp"

#include <array>
#include <cmath>
#include <cstring>
#include <gtest/gtest.h>
#include <span>
#include <string>
#include <type_traits>
#include <vector>

namespace {
using namespace lanlink;
using namespace lanlink::protocol;

Uuid128 id(std::uint64_t high, std::uint64_t low) { return {high, low}; }

template <typename T> void expect_round_trip(const T& message) {
    Message original = message;
    if constexpr (std::is_same_v<T, InputMessage>) {
        auto& input = std::get<InputMessage>(original);
        input.event.origin = InputOrigin::kRemoteInjected;
    }
    auto encoded = encode(original);
    ASSERT_TRUE(encoded.ok()) << encoded.error();
    auto decoded = decode(encoded.value());
    ASSERT_TRUE(decoded.ok()) << decoded.error();
    EXPECT_EQ(decoded.value(), original);
}

TEST(ProtocolWire, RoundTripsEveryMessageAndInputPayload) {
    expect_round_trip(Hello{id(1, 2), kProtocolVersion});
    expect_round_trip(Heartbeat{0x0102030405060708ULL});
    expect_round_trip(AcquireRequest{id(3, 4), id(5, 6), id(7, 8), Point{1.25, -2.5}});
    expect_round_trip(AcquireResponse{id(7, 8), true, id(9, 10), 42, ""});
    expect_round_trip(AcquireResponse{id(7, 8), false, {}, 0, "denied"});
    expect_round_trip(AcquireResponse{id(7, 8), false, id(9, 10), 42, "busy"});
    expect_round_trip(ActivateRequest{id(9, 10), 42});
    expect_round_trip(ActivateAck{id(9, 10), 42, true, ""});
    expect_round_trip(ActivateAck{{}, 0, false, "stale"});
    expect_round_trip(ActivateAck{id(9, 10), 42, false, "stale"});
    expect_round_trip(RenewRequest{id(9, 10), 42});
    expect_round_trip(RenewAck{id(9, 10), 42, true, ""});
    expect_round_trip(RenewAck{{}, 0, false, "expired"});
    expect_round_trip(RenewAck{id(9, 10), 42, false, "expired"});

    InputEvent base{id(11, 12), id(13, 14), 3, 4, MouseDelta{1.5, -2.25}, InputOrigin::kPhysical};
    expect_round_trip(InputMessage{base});
    base.payload = MouseButton{2, true};
    expect_round_trip(InputMessage{base});
    base.payload = MouseWheel{3.5};
    expect_round_trip(InputMessage{base});
    base.payload = Key{65, false};
    expect_round_trip(InputMessage{base});
    base.payload = PointerWarp{100.0, 200.0};
    expect_round_trip(InputMessage{base});

    expect_round_trip(Release{id(9, 10), 42});
    expect_round_trip(Revoke{id(9, 10), 42, RevokeReason::kProtocolError});
    expect_round_trip(ErrorMessage{17, "protocol error"});
}

TEST(ProtocolWire, EncodesGoldenHeaderInNetworkOrder) {
    auto encoded = encode(Heartbeat{0x0102030405060708ULL});
    ASSERT_TRUE(encoded.ok());
    const std::vector<std::uint8_t> expected = {
        'L', 'L', 'N', 'K', 1, static_cast<std::uint8_t>(MessageType::kHeartbeat),
        0,   0,   0,   0,   0, 8,
        1,   2,   3,   4,   5, 6,
        7,   8};
    EXPECT_EQ(encoded.value(), expected);
}

TEST(ProtocolWire, RejectsMalformedHeadersAndTrailingBytes) {
    auto encoded = encode(Heartbeat{1});
    ASSERT_TRUE(encoded.ok());
    auto bytes = encoded.value();
    bytes[0] = 'X';
    EXPECT_FALSE(decode(bytes).ok());
    bytes = encoded.value();
    bytes[4] = 2;
    EXPECT_FALSE(decode(bytes).ok());
    bytes = encoded.value();
    bytes[6] = 1;
    EXPECT_FALSE(decode(bytes).ok());
    bytes = encoded.value();
    bytes[5] = 0xff;
    EXPECT_FALSE(decode(bytes).ok());
    bytes = encoded.value();
    bytes[11] = 9;
    EXPECT_FALSE(decode(bytes).ok());
    bytes = encoded.value();
    bytes[11] = 7;
    bytes.push_back(0);
    EXPECT_FALSE(decode(bytes).ok());
    bytes = encoded.value();
    bytes.resize(bytes.size() - 1);
    EXPECT_FALSE(decode(bytes).ok());
}

TEST(ProtocolWire, RejectsInvalidValuesAndSizeLimits) {
    EXPECT_FALSE(encode(Hello{{}, kProtocolVersion}).ok());
    EXPECT_FALSE(encode(Hello{id(1, 1), 2}).ok());
    EXPECT_FALSE(encode(AcquireRequest{{}, id(1, 1), id(2, 2), Point{0, 0}}).ok());
    EXPECT_FALSE(encode(Revoke{{}, 1, RevokeReason::kProtocolError}).ok());
    EXPECT_FALSE(encode(AcquireResponse{id(1, 1), true, {}, 0, ""}).ok());
    EXPECT_FALSE(encode(InputMessage{InputEvent{
                            {}, id(1, 1), 1, 1, MouseDelta{0, 0}, InputOrigin::kPhysical}})
                     .ok());
    EXPECT_FALSE(encode(InputMessage{InputEvent{id(1, 1), id(1, 1), 1, 1, MouseDelta{NAN, 0},
                                                InputOrigin::kPhysical}})
                     .ok());
    EXPECT_FALSE(encode(Revoke{id(1, 1), 1, static_cast<RevokeReason>(99)}).ok());
    EXPECT_FALSE(encode(ErrorMessage{1, std::string(4097, 'x')}).ok());

    auto encoded = encode(Heartbeat{1});
    ASSERT_TRUE(encoded.ok());
    auto bytes = encoded.value();
    bytes[8] = 0x01;
    bytes[9] = 0x00;
    bytes[10] = 0x00;
    bytes[11] = 0x00;
    EXPECT_FALSE(decode(bytes).ok());
}

TEST(ProtocolWire, RejectsInvalidInputTagsAndBooleans) {
    InputEvent event{id(1, 2), id(3, 4), 1, 1, MouseButton{7, true}, InputOrigin::kPhysical};
    auto encoded = encode(InputMessage{event});
    ASSERT_TRUE(encoded.ok());
    auto bytes = encoded.value();
    constexpr std::size_t kInputTagOffset = kHeaderSize + 16 + 16 + 8 + 8;
    constexpr std::size_t kInputButtonPressedOffset = kInputTagOffset + 1 + 4;
    bytes[kInputButtonPressedOffset] = 2;
    EXPECT_FALSE(decode(bytes).ok());
    bytes = encoded.value();
    bytes[kInputTagOffset] = 99;
    EXPECT_FALSE(decode(bytes).ok());
}

TEST(ProtocolWire, FrameDecoderHandlesSplitAndCoalescedFrames) {
    auto a = encode(Heartbeat{1});
    auto b = encode(ErrorMessage{2, "bad"});
    ASSERT_TRUE(a.ok());
    ASSERT_TRUE(b.ok());
    std::vector<std::uint8_t> both = a.value();
    both.insert(both.end(), b.value().begin(), b.value().end());

    FrameDecoder decoder;
    auto first = decoder.feed(std::span<const std::uint8_t>(both.data(), 5));
    ASSERT_TRUE(first.ok());
    EXPECT_TRUE(first.value().empty());
    EXPECT_EQ(decoder.buffered_bytes(), 5u);
    auto second = decoder.feed(std::span<const std::uint8_t>(both.data() + 5, both.size() - 5));
    ASSERT_TRUE(second.ok());
    ASSERT_EQ(second.value().size(), 2u);
    EXPECT_EQ(std::get<Heartbeat>(second.value()[0]).counter, 1u);
    EXPECT_EQ(std::get<ErrorMessage>(second.value()[1]).message, "bad");
    EXPECT_EQ(decoder.buffered_bytes(), 0u);
}

TEST(ProtocolWire, FrameDecoderClearsAfterOversizedOrMalformedFrame) {
    FrameDecoder decoder;
    std::vector<std::uint8_t> header = {
        'L', 'L', 'N',  'K',  1,    static_cast<std::uint8_t>(MessageType::kHeartbeat),
        0,   0,   0xff, 0xff, 0xff, 0xff};
    auto result = decoder.feed(header);
    EXPECT_FALSE(result.ok());
    EXPECT_EQ(decoder.buffered_bytes(), 0u);
    EXPECT_TRUE(decoder.finish().ok());
}

TEST(ProtocolWire, FinishRejectsPartialFrame) {
    auto encoded = encode(Heartbeat{1});
    ASSERT_TRUE(encoded.ok());
    FrameDecoder decoder;
    ASSERT_TRUE(decoder.feed(std::span<const std::uint8_t>(encoded.value().data(), 4)).ok());
    EXPECT_FALSE(decoder.finish().ok());
    EXPECT_EQ(decoder.buffered_bytes(), 0u);
}

} // namespace

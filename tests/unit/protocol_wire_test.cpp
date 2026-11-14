#include "lanlink/protocol/wire.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <gtest/gtest.h>
#include <limits>
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
    expect_round_trip(DisplayInfo{id(15, 16), Rect{-1920.0, 0.0, 0.0, 1080.0}, 2.0, true});
    expect_round_trip(PointerPosition{id(17, 18), 9, Point{-123.5, 456.25}});
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
    base.payload = PointerMove{1234.5, -678.25};
    expect_round_trip(InputMessage{base});

    expect_round_trip(Release{id(9, 10), 42});
    expect_round_trip(Revoke{id(9, 10), 42, RevokeReason::kProtocolError});
    expect_round_trip(ErrorMessage{17, "protocol error"});
}

TEST(ProtocolWire, EncodesGoldenHeaderInNetworkOrder) {
    auto encoded = encode(Heartbeat{0x0102030405060708ULL});
    ASSERT_TRUE(encoded.ok());
    const std::vector<std::uint8_t> expected = {
        'L', 'L', 'N', 'K', kProtocolVersion,
        static_cast<std::uint8_t>(MessageType::kHeartbeat),
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
    bytes[4] = static_cast<std::uint8_t>(kProtocolVersion + 1);
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
    EXPECT_FALSE(encode(Hello{id(1, 1),
                              static_cast<std::uint16_t>(kProtocolVersion + 1)}).ok());
    EXPECT_FALSE(encode(DisplayInfo{{}, Rect{0.0, 0.0, 1920.0, 1080.0}, 1.0}).ok());
    EXPECT_FALSE(encode(DisplayInfo{id(1, 1), Rect{0.0, 0.0, 0.0, 1080.0}, 1.0}).ok());
    EXPECT_FALSE(encode(DisplayInfo{id(1, 1), Rect{0.0, 0.0, 1920.0, 1080.0}, 0.0}).ok());
    EXPECT_FALSE(encode(DisplayInfo{id(1, 1),
                                    Rect{0.0, 0.0, kMaxDisplayCoordinate + 1.0, 1080.0},
                                    1.0})
                     .ok());
    EXPECT_FALSE(encode(DisplayInfo{id(1, 1), Rect{0.0, 0.0, 1920.0, 1080.0},
                                    kMaxDisplayScale + 1.0})
                     .ok());
    EXPECT_FALSE(encode(AcquireRequest{{}, id(1, 1), id(2, 2), Point{0, 0}}).ok());
    EXPECT_FALSE(encode(Revoke{{}, 1, RevokeReason::kProtocolError}).ok());
    EXPECT_FALSE(encode(PointerPosition{{}, 1, Point{0, 0}}).ok());
    EXPECT_FALSE(encode(PointerPosition{id(1, 1), 0, Point{0, 0}}).ok());
    EXPECT_FALSE(encode(PointerPosition{id(1, 1), 1,
                                        Point{kMaxDisplayCoordinate + 1.0, 0}})
                     .ok());
    EXPECT_FALSE(encode(AcquireResponse{id(1, 1), true, {}, 0, ""}).ok());
    EXPECT_FALSE(encode(InputMessage{InputEvent{
                            {}, id(1, 1), 1, 1, MouseDelta{0, 0}, InputOrigin::kPhysical}})
                     .ok());
    EXPECT_FALSE(encode(InputMessage{InputEvent{id(1, 1), id(1, 1), 1, 1, MouseDelta{NAN, 0},
                                                InputOrigin::kPhysical}})
                     .ok());
    EXPECT_FALSE(encode(InputMessage{InputEvent{id(1, 1), id(1, 1), 1, 1, PointerMove{0, NAN},
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

TEST(ProtocolWire, RejectsZeroControlIdentifiers) {
    EXPECT_FALSE(encode(AcquireResponse{{}, false, {}, 0, "denied"}).ok());
    EXPECT_FALSE(encode(ActivateRequest{{}, 0}).ok());
    EXPECT_FALSE(encode(RenewRequest{{}, 0}).ok());
    EXPECT_FALSE(encode(Release{{}, 0}).ok());
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
        'L', 'L', 'N',  'K',  kProtocolVersion,
        static_cast<std::uint8_t>(MessageType::kHeartbeat),
        0,   0,   0xff, 0xff, 0xff, 0xff};
    auto result = decoder.feed(header);
    EXPECT_FALSE(result.ok());
    EXPECT_EQ(decoder.buffered_bytes(), 0u);
    EXPECT_TRUE(decoder.finish().ok());
}

TEST(ProtocolWire, FrameDecoderDropsMalformedClipboardButKeepsFollowingInput) {
    auto malformed = encode(ClipboardBegin{id(4, 4), 1, 0x01, 1, 1, 0, 0});
    ASSERT_TRUE(malformed.ok());
    // Keep the LLNK header/type valid but make the format mask invalid.
    malformed.value()[kHeaderSize + 16 + 8] = 0x80;
    auto following = encode(Heartbeat{99});
    ASSERT_TRUE(following.ok());
    malformed.value().insert(malformed.value().end(), following.value().begin(),
                             following.value().end());

    FrameDecoder decoder;
    auto result = decoder.feed(malformed.value());
    ASSERT_TRUE(result.ok()) << result.error();
    ASSERT_EQ(result.value().size(), 1u);
    EXPECT_EQ(std::get<Heartbeat>(result.value().front()).counter, 99u);
    EXPECT_EQ(decoder.buffered_bytes(), 0u);
}

TEST(ProtocolWire, FrameDecoderBoundsLargeInputBeforeBuffering) {
    FrameDecoder decoder;
    std::vector<std::uint8_t> input(kMaxFrameSize + 1, 0);
    auto result = decoder.feed(input);
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

TEST(ProtocolWire, RoundTripsClipboardFrames) {
    expect_round_trip(ClipboardBegin{id(21, 22), 1, 0, 0, 0, 0, 0});
    expect_round_trip(ClipboardBegin{id(21, 22), 2, 0x07, 12, 3, 4, 5});
    expect_round_trip(ClipboardChunk{id(21, 22), 2, ClipboardFormat::kText, 0,
                                     {'h', 'i'}});
    expect_round_trip(ClipboardChunk{id(21, 22), 2, ClipboardFormat::kHtml, 0,
                                     {'<', 'p', '>'}});
    expect_round_trip(ClipboardChunk{id(21, 22), 2, ClipboardFormat::kPng, 0,
                                     {0x89, 'P', 'N', 'G', 0x0d, 0x0a, 0x1a, 0x0a}});
    expect_round_trip(ClipboardEnd{id(21, 22), 2});
}

TEST(ProtocolWire, RejectsMalformedClipboardMetadataAndPayloads) {
    EXPECT_FALSE(encode(ClipboardBegin{{}, 1, 0, 0, 0, 0, 0}).ok());
    EXPECT_FALSE(encode(ClipboardBegin{id(1, 1), 0, 0, 0, 0, 0, 0}).ok());
    EXPECT_FALSE(encode(ClipboardEnd{{}, 1}).ok());
    EXPECT_FALSE(encode(ClipboardEnd{id(1, 1), 0}).ok());
    EXPECT_FALSE(encode(ClipboardChunk{{}, 1, ClipboardFormat::kText, 0, {'x'}}).ok());
    EXPECT_FALSE(encode(ClipboardChunk{id(1, 1), 0, ClipboardFormat::kText, 0, {'x'}}).ok());
    EXPECT_FALSE(encode(ClipboardChunk{id(1, 1), 1,
                                       static_cast<ClipboardFormat>(99), 0, {'x'}})
                     .ok());
    EXPECT_FALSE(encode(ClipboardBegin{id(1, 1), 1, 0x80, 0, 0, 0, 0}).ok());
    EXPECT_FALSE(encode(ClipboardBegin{id(1, 1), 1, 0x01, 9, 2, 3, 3}).ok());
    EXPECT_FALSE(encode(ClipboardBegin{id(1, 1), 1, 0x01, kMaxClipboardPackageSize + 1, 0, 0,
                                       0})
                     .ok());
    EXPECT_FALSE(encode(ClipboardChunk{id(1, 1), 1, ClipboardFormat::kText, 0,
                                       {0xff}})
                     .ok());
    EXPECT_FALSE(encode(ClipboardChunk{id(1, 1), 1, ClipboardFormat::kHtml, 0,
                                       {0xc3, 0x28}})
                     .ok());
    EXPECT_FALSE(encode(ClipboardChunk{id(1, 1), 1, ClipboardFormat::kPng, 0,
                                       {0x00, 'P', 'N', 'G', 0x0d, 0x0a, 0x1a, 0x0a}})
                     .ok());
    EXPECT_FALSE(encode(ClipboardChunk{id(1, 1), 1, ClipboardFormat::kText,
                                       std::numeric_limits<std::uint32_t>::max(), {'x'}})
                     .ok());
    EXPECT_FALSE(encode(ClipboardChunk{id(1, 1), 1, ClipboardFormat::kText, 0,
                                       std::vector<std::uint8_t>(kMaxClipboardChunkDataSize + 1)})
                     .ok());
}

TEST(ProtocolWire, AllowsUtf8CodePointsToCrossChunkBoundaries) {
    const ClipboardChunk first{id(2, 2), 1, ClipboardFormat::kText, 0, {0xe2, 0x82}};
    const ClipboardChunk second{id(2, 2), 1, ClipboardFormat::kText, 2, {0xac}};
    EXPECT_TRUE(encode(first).ok());
    EXPECT_TRUE(encode(second).ok());
}

TEST(ProtocolWire, AllowsPngSignatureToCrossChunkBoundaries) {
    const ClipboardChunk first{id(2, 2), 1, ClipboardFormat::kPng, 0, {0x89, 'P', 'N'}};
    const ClipboardChunk second{id(2, 2), 1, ClipboardFormat::kPng, 3,
                                {'G', 0x0d, 0x0a, 0x1a, 0x0a}};
    EXPECT_TRUE(encode(first).ok());
    EXPECT_TRUE(encode(second).ok());
}

TEST(ProtocolWire, RejectsMalformedClipboardFramesOnDecode) {
    auto begin = encode(ClipboardBegin{id(3, 3), 1, 0x01, 1, 1, 0, 0});
    ASSERT_TRUE(begin.ok());
    auto begin_bytes = begin.value();
    begin_bytes[kHeaderSize + 16 + 8] = 0x80;
    EXPECT_FALSE(decode(begin_bytes).ok());

    auto chunk = encode(ClipboardChunk{id(3, 3), 1, ClipboardFormat::kText, 0, {'x'}});
    ASSERT_TRUE(chunk.ok());
    auto chunk_bytes = chunk.value();
    chunk_bytes[kHeaderSize + 16 + 8] = 0xff;
    EXPECT_FALSE(decode(chunk_bytes).ok());
    chunk_bytes = chunk.value();
    chunk_bytes[kHeaderSize + 16 + 8 + 1 + 4 + 3] = 0xff;
    EXPECT_FALSE(decode(chunk_bytes).ok());
}

TEST(ProtocolWire, FrameDecoderHandlesFragmentedClipboardSequence) {
    const ClipboardBegin begin{id(30, 31), 7, 0x01, 5, 5, 0, 0};
    const ClipboardChunk chunk{id(30, 31), 7, ClipboardFormat::kText, 0,
                               {'h', 'e', 'l', 'l', 'o'}};
    const ClipboardEnd end{id(30, 31), 7};
    auto first = encode(begin);
    auto second = encode(chunk);
    auto third = encode(end);
    ASSERT_TRUE(first.ok());
    ASSERT_TRUE(second.ok());
    ASSERT_TRUE(third.ok());
    std::vector<std::uint8_t> bytes = first.value();
    bytes.insert(bytes.end(), second.value().begin(), second.value().end());
    bytes.insert(bytes.end(), third.value().begin(), third.value().end());

    FrameDecoder decoder;
    std::vector<Message> messages;
    for (std::size_t offset = 0; offset < bytes.size();) {
        const std::size_t count = std::min<std::size_t>(3, bytes.size() - offset);
        auto result = decoder.feed(std::span<const std::uint8_t>(bytes.data() + offset, count));
        ASSERT_TRUE(result.ok()) << result.error();
        messages.insert(messages.end(), result.value().begin(), result.value().end());
        offset += count;
    }
    ASSERT_EQ(messages.size(), 3u);
    EXPECT_EQ(std::get<ClipboardBegin>(messages[0]), begin);
    EXPECT_EQ(std::get<ClipboardChunk>(messages[1]), chunk);
    EXPECT_EQ(std::get<ClipboardEnd>(messages[2]), end);
}

} // namespace

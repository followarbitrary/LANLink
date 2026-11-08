#include "lanlink/protocol/wire.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <type_traits>
#include <utility>

namespace lanlink::protocol {
namespace {

constexpr std::uint8_t to_u8(MessageType type) { return static_cast<std::uint8_t>(type); }

bool valid_type(std::uint8_t value) {
    return value >= to_u8(MessageType::kHello) && value <= to_u8(MessageType::kDisplayInfo);
}
bool valid_reason(std::uint8_t value) {
    return value <= static_cast<std::uint8_t>(RevokeReason::kProtocolError);
}
bool valid_edge(std::uint8_t value) {
    return value <= static_cast<std::uint8_t>(Edge::kDown);
}
bool zero(const Uuid128& id) { return id.high == 0 && id.low == 0; }
bool finite(double value) { return std::isfinite(value); }

class Writer {
  public:
    void u8(std::uint8_t v) { data_.push_back(v); }
    void u16(std::uint16_t v) {
        data_.push_back(static_cast<std::uint8_t>(v >> 8));
        data_.push_back(static_cast<std::uint8_t>(v));
    }
    void u32(std::uint32_t v) {
        for (int s = 24; s >= 0; s -= 8)
            data_.push_back(static_cast<std::uint8_t>(v >> s));
    }
    void u64(std::uint64_t v) {
        for (int s = 56; s >= 0; s -= 8)
            data_.push_back(static_cast<std::uint8_t>(v >> s));
    }
    void id(const Uuid128& value) {
        u64(value.high);
        u64(value.low);
    }
    void f64(double value) {
        std::uint64_t bits{};
        std::memcpy(&bits, &value, sizeof(bits));
        u64(bits);
    }
    bool string(const std::string& value) {
        if (value.size() > kMaxErrorText ||
            value.size() > std::numeric_limits<std::uint32_t>::max())
            return false;
        u32(static_cast<std::uint32_t>(value.size()));
        data_.insert(data_.end(), value.begin(), value.end());
        return true;
    }
    std::vector<std::uint8_t> take() && { return std::move(data_); }

  private:
    std::vector<std::uint8_t> data_;
};

class Reader {
  public:
    explicit Reader(std::span<const std::uint8_t> bytes) : bytes_(bytes) {}
    bool u8(std::uint8_t& out) {
        if (remaining() < 1)
            return false;
        out = bytes_[offset_++];
        return true;
    }
    bool u16(std::uint16_t& out) {
        if (remaining() < 2)
            return false;
        out = (std::uint16_t(bytes_[offset_]) << 8) | bytes_[offset_ + 1];
        offset_ += 2;
        return true;
    }
    bool u32(std::uint32_t& out) {
        if (remaining() < 4)
            return false;
        out = 0;
        for (int i = 0; i < 4; ++i)
            out = (out << 8) | bytes_[offset_++];
        return true;
    }
    bool u64(std::uint64_t& out) {
        if (remaining() < 8)
            return false;
        out = 0;
        for (int i = 0; i < 8; ++i)
            out = (out << 8) | bytes_[offset_++];
        return true;
    }
    bool id(Uuid128& out) { return u64(out.high) && u64(out.low); }
    bool f64(double& out) {
        std::uint64_t bits{};
        if (!u64(bits))
            return false;
        std::memcpy(&out, &bits, sizeof(out));
        return finite(out);
    }
    bool string(std::string& out) {
        std::uint32_t length{};
        if (!u32(length) || length > kMaxErrorText || remaining() < length)
            return false;
        out.assign(reinterpret_cast<const char*>(bytes_.data() + offset_), length);
        offset_ += length;
        return true;
    }
    std::size_t remaining() const { return bytes_.size() - offset_; }
    bool done() const { return offset_ == bytes_.size(); }

  private:
    std::span<const std::uint8_t> bytes_;
    std::size_t offset_{};
};

void write_input(Writer& w, const InputEvent& event) {
    w.id(event.source);
    w.id(event.session);
    w.u64(event.generation);
    w.u64(event.seq);
    std::visit(
        [&](const auto& payload) {
            using T = std::decay_t<decltype(payload)>;
            if constexpr (std::is_same_v<T, MouseDelta>) {
                w.u8(1);
                w.f64(payload.dx);
                w.f64(payload.dy);
            } else if constexpr (std::is_same_v<T, MouseButton>) {
                w.u8(2);
                w.u32(payload.button);
                w.u8(payload.pressed ? 1 : 0);
            } else if constexpr (std::is_same_v<T, MouseWheel>) {
                w.u8(3);
                w.f64(payload.delta);
            } else if constexpr (std::is_same_v<T, Key>) {
                w.u8(4);
                w.u32(payload.code);
                w.u8(payload.pressed ? 1 : 0);
            } else if constexpr (std::is_same_v<T, PointerWarp>) {
                w.u8(5);
                w.f64(payload.x);
                w.f64(payload.y);
            } else if constexpr (std::is_same_v<T, PointerMove>) {
                w.u8(6);
                w.f64(payload.x);
                w.f64(payload.y);
            }
        },
        event.payload);
}

bool read_bool(Reader& r, bool& value) {
    std::uint8_t v{};
    if (!r.u8(v) || v > 1)
        return false;
    value = v != 0;
    return true;
}

bool read_input(Reader& r, InputEvent& event) {
    if (!r.id(event.source) || !r.id(event.session) || !r.u64(event.generation) ||
        !r.u64(event.seq))
        return false;
    if (zero(event.source) || zero(event.session) || event.generation == 0 || event.seq == 0)
        return false;
    std::uint8_t tag{};
    if (!r.u8(tag))
        return false;
    event.origin = InputOrigin::kRemoteInjected;
    switch (tag) {
    case 1: {
        double x{}, y{};
        if (!r.f64(x) || !r.f64(y))
            return false;
        event.payload = MouseDelta{x, y};
        return true;
    }
    case 2: {
        std::uint32_t button{};
        bool pressed{};
        if (!r.u32(button) || !read_bool(r, pressed))
            return false;
        event.payload = MouseButton{button, pressed};
        return true;
    }
    case 3: {
        double delta{};
        if (!r.f64(delta))
            return false;
        event.payload = MouseWheel{delta};
        return true;
    }
    case 4: {
        std::uint32_t code{};
        bool pressed{};
        if (!r.u32(code) || !read_bool(r, pressed))
            return false;
        event.payload = Key{code, pressed};
        return true;
    }
    case 5: {
        double x{}, y{};
        if (!r.f64(x) || !r.f64(y))
            return false;
        event.payload = PointerWarp{x, y};
        return true;
    }
    case 6: {
        double x{}, y{};
        if (!r.f64(x) || !r.f64(y))
            return false;
        event.payload = PointerMove{x, y};
        return true;
    }
    default:
        return false;
    }
}

Result<std::vector<std::uint8_t>> make_frame(MessageType type, Writer&& payload) {
    auto body = std::move(payload).take();
    if (body.size() > kMaxFrameSize - kHeaderSize)
        return Result<std::vector<std::uint8_t>>::failure("frame too large");
    std::vector<std::uint8_t> frame;
    frame.reserve(kHeaderSize + body.size());
    frame.insert(frame.end(), {'L', 'L', 'N', 'K', kProtocolVersion, to_u8(type), 0, 0});
    const auto length = static_cast<std::uint32_t>(body.size());
    frame.push_back(static_cast<std::uint8_t>(length >> 24));
    frame.push_back(static_cast<std::uint8_t>(length >> 16));
    frame.push_back(static_cast<std::uint8_t>(length >> 8));
    frame.push_back(static_cast<std::uint8_t>(length));
    frame.insert(frame.end(), body.begin(), body.end());
    return Result<std::vector<std::uint8_t>>::success(std::move(frame));
}

Result<Message> fail(const char* message) { return Result<Message>::failure(message); }

} // namespace

bool operator==(const InputMessage& a, const InputMessage& b) {
    if (a.event.source != b.event.source || a.event.session != b.event.session ||
        a.event.generation != b.event.generation || a.event.seq != b.event.seq ||
        a.event.origin != b.event.origin || a.event.payload.index() != b.event.payload.index())
        return false;
    return std::visit(
        [](const auto& x, const auto& y) -> bool {
            using X = std::decay_t<decltype(x)>;
            using Y = std::decay_t<decltype(y)>;
            if constexpr (!std::is_same_v<X, Y>)
                return false;
            else if constexpr (std::is_same_v<X, MouseDelta>)
                return x.dx == y.dx && x.dy == y.dy;
            else if constexpr (std::is_same_v<X, MouseButton>)
                return x.button == y.button && x.pressed == y.pressed;
            else if constexpr (std::is_same_v<X, MouseWheel>)
                return x.delta == y.delta;
            else if constexpr (std::is_same_v<X, Key>)
                return x.code == y.code && x.pressed == y.pressed;
            else
                return x.x == y.x && x.y == y.y;
        },
        a.event.payload, b.event.payload);
}

Result<std::vector<std::uint8_t>> encode(const Message& message) {
    return std::visit(
        [](const auto& value) -> Result<std::vector<std::uint8_t>> {
            using T = std::decay_t<decltype(value)>;
            Writer w;
            if constexpr (std::is_same_v<T, Hello>) {
                if (zero(value.device) || value.advertised_version != kProtocolVersion)
                    return Result<std::vector<std::uint8_t>>::failure("invalid hello");
                w.id(value.device);
                w.u16(value.advertised_version);
                return make_frame(MessageType::kHello, std::move(w));
            } else if constexpr (std::is_same_v<T, DisplayInfo>) {
                const auto& bounds = value.logical_bounds;
                if (zero(value.display) || !finite(bounds.left) || !finite(bounds.top) ||
                    !finite(bounds.right) || !finite(bounds.bottom) ||
                    !(bounds.left < bounds.right) || !(bounds.top < bounds.bottom) ||
                    !finite(value.scale) || value.scale <= 0.0)
                    return Result<std::vector<std::uint8_t>>::failure("invalid display info");
                w.id(value.display);
                w.f64(bounds.left);
                w.f64(bounds.top);
                w.f64(bounds.right);
                w.f64(bounds.bottom);
                w.f64(value.scale);
                return make_frame(MessageType::kDisplayInfo, std::move(w));
            } else if constexpr (std::is_same_v<T, Heartbeat>) {
                w.u64(value.counter);
                return make_frame(MessageType::kHeartbeat, std::move(w));
            } else if constexpr (std::is_same_v<T, AcquireRequest>) {
                if (zero(value.source) || zero(value.target_display) || zero(value.request) ||
                    !finite(value.entry.x) || !finite(value.entry.y) ||
                    !valid_edge(static_cast<std::uint8_t>(value.target_edge)) ||
                    !finite(value.tangent_ratio) || value.tangent_ratio < 0.0 ||
                    value.tangent_ratio > 1.0)
                    return Result<std::vector<std::uint8_t>>::failure("invalid acquire request");
                w.id(value.source);
                w.id(value.target_display);
                w.id(value.request);
                w.f64(value.entry.x);
                w.f64(value.entry.y);
                w.u8(static_cast<std::uint8_t>(value.target_edge));
                w.f64(value.tangent_ratio);
                w.u8(value.entry_normalized ? 1 : 0);
                return make_frame(MessageType::kAcquireRequest, std::move(w));
            } else if constexpr (std::is_same_v<T, AcquireResponse>) {
                if (zero(value.request) ||
                    (value.granted && (zero(value.session) || value.generation == 0)))
                    return Result<std::vector<std::uint8_t>>::failure("invalid acquire response");
                w.id(value.request);
                w.u8(value.granted ? 1 : 0);
                w.id(value.session);
                w.u64(value.generation);
                if (!w.string(value.error))
                    return Result<std::vector<std::uint8_t>>::failure("error text too long");
                return make_frame(MessageType::kAcquireResponse, std::move(w));
            } else if constexpr (std::is_same_v<T, ActivateRequest>) {
                if (zero(value.session) || value.generation == 0)
                    return Result<std::vector<std::uint8_t>>::failure("invalid activate request");
                w.id(value.session);
                w.u64(value.generation);
                return make_frame(MessageType::kActivateRequest, std::move(w));
            } else if constexpr (std::is_same_v<T, ActivateAck>) {
                if (value.success && (zero(value.session) || value.generation == 0))
                    return Result<std::vector<std::uint8_t>>::failure("invalid activate ack");
                w.id(value.session);
                w.u64(value.generation);
                w.u8(value.success ? 1 : 0);
                if (!w.string(value.error))
                    return Result<std::vector<std::uint8_t>>::failure("error text too long");
                return make_frame(MessageType::kActivateAck, std::move(w));
            } else if constexpr (std::is_same_v<T, RenewRequest>) {
                if (zero(value.session) || value.generation == 0)
                    return Result<std::vector<std::uint8_t>>::failure("invalid renew request");
                w.id(value.session);
                w.u64(value.generation);
                return make_frame(MessageType::kRenewRequest, std::move(w));
            } else if constexpr (std::is_same_v<T, RenewAck>) {
                if (value.success && (zero(value.session) || value.generation == 0))
                    return Result<std::vector<std::uint8_t>>::failure("invalid renew ack");
                w.id(value.session);
                w.u64(value.generation);
                w.u8(value.success ? 1 : 0);
                if (!w.string(value.error))
                    return Result<std::vector<std::uint8_t>>::failure("error text too long");
                return make_frame(MessageType::kRenewAck, std::move(w));
            } else if constexpr (std::is_same_v<T, InputMessage>) {
                if (zero(value.event.source) || zero(value.event.session) ||
                    value.event.generation == 0 || value.event.seq == 0)
                    return Result<std::vector<std::uint8_t>>::failure("invalid input ids");
                bool valid = true;
                std::visit(
                    [&](const auto& payload) {
                        using P = std::decay_t<decltype(payload)>;
                        if constexpr (std::is_same_v<P, MouseDelta>)
                            valid = finite(payload.dx) && finite(payload.dy);
                        else if constexpr (std::is_same_v<P, MouseWheel>)
                            valid = finite(payload.delta);
                        else if constexpr (std::is_same_v<P, PointerWarp>)
                            valid = finite(payload.x) && finite(payload.y);
                        else if constexpr (std::is_same_v<P, PointerMove>)
                            valid = finite(payload.x) && finite(payload.y);
                    },
                    value.event.payload);
                if (!valid)
                    return Result<std::vector<std::uint8_t>>::failure("non-finite input");
                write_input(w, value.event);
                return make_frame(MessageType::kInput, std::move(w));
            } else if constexpr (std::is_same_v<T, Release>) {
                if (zero(value.session) || value.generation == 0)
                    return Result<std::vector<std::uint8_t>>::failure("invalid release");
                w.id(value.session);
                w.u64(value.generation);
                return make_frame(MessageType::kRelease, std::move(w));
            } else if constexpr (std::is_same_v<T, Revoke>) {
                if (zero(value.session) || value.generation == 0 ||
                    static_cast<std::uint8_t>(value.reason) >
                        static_cast<std::uint8_t>(RevokeReason::kProtocolError))
                    return Result<std::vector<std::uint8_t>>::failure("invalid revoke");
                w.id(value.session);
                w.u64(value.generation);
                w.u8(static_cast<std::uint8_t>(value.reason));
                return make_frame(MessageType::kRevoke, std::move(w));
            } else {
                w.u16(value.code);
                if (!w.string(value.message))
                    return Result<std::vector<std::uint8_t>>::failure("error text too long");
                return make_frame(MessageType::kError, std::move(w));
            }
        },
        message);
}

Result<Message> decode(std::span<const std::uint8_t> frame) {
    if (frame.size() < kHeaderSize || frame.size() > kMaxFrameSize)
        return fail("invalid frame size");
    if (std::memcmp(frame.data(), "LLNK", 4) != 0 || frame[4] != kProtocolVersion ||
        frame[6] != 0 || frame[7] != 0 || !valid_type(frame[5]))
        return fail("invalid header");
    const std::uint32_t length = (std::uint32_t(frame[8]) << 24) | (std::uint32_t(frame[9]) << 16) |
                                 (std::uint32_t(frame[10]) << 8) | frame[11];
    if (length > kMaxFrameSize - kHeaderSize || frame.size() != kHeaderSize + length)
        return fail("invalid payload length");
    Reader r(frame.subspan(kHeaderSize));
    Message message;
    switch (static_cast<MessageType>(frame[5])) {
    case MessageType::kHello: {
        Hello value;
        if (!r.id(value.device) || !r.u16(value.advertised_version) || zero(value.device) ||
            value.advertised_version != kProtocolVersion)
            return fail("invalid hello");
        message = value;
        break;
    }
    case MessageType::kDisplayInfo: {
        DisplayInfo value;
        if (!r.id(value.display) || !r.f64(value.logical_bounds.left) ||
            !r.f64(value.logical_bounds.top) || !r.f64(value.logical_bounds.right) ||
            !r.f64(value.logical_bounds.bottom) || !r.f64(value.scale) ||
            zero(value.display) || !finite(value.logical_bounds.left) ||
            !finite(value.logical_bounds.top) || !finite(value.logical_bounds.right) ||
            !finite(value.logical_bounds.bottom) ||
            !(value.logical_bounds.left < value.logical_bounds.right) ||
            !(value.logical_bounds.top < value.logical_bounds.bottom) || !finite(value.scale) ||
            value.scale <= 0.0)
            return fail("invalid display info");
        message = value;
        break;
    }
    case MessageType::kHeartbeat: {
        Heartbeat value;
        if (!r.u64(value.counter))
            return fail("truncated heartbeat");
        message = value;
        break;
    }
    case MessageType::kAcquireRequest: {
        AcquireRequest value;
        std::uint8_t edge{};
        bool normalized{};
        if (!r.id(value.source) || !r.id(value.target_display) || !r.id(value.request) ||
            !r.f64(value.entry.x) || !r.f64(value.entry.y) || !r.u8(edge) ||
            !r.f64(value.tangent_ratio) || !read_bool(r, normalized) ||
            zero(value.source) || zero(value.target_display) || zero(value.request) ||
            !valid_edge(edge) || value.tangent_ratio < 0.0 || value.tangent_ratio > 1.0)
            return fail("invalid acquire request");
        value.target_edge = static_cast<Edge>(edge);
        value.entry_normalized = normalized;
        message = value;
        break;
    }
    case MessageType::kAcquireResponse: {
        AcquireResponse value;
        std::uint8_t granted_byte{};
        if (!r.id(value.request) || zero(value.request) || !r.u8(granted_byte) ||
            granted_byte > 1 || !r.id(value.session) || !r.u64(value.generation) ||
            !r.string(value.error))
            return fail("invalid acquire response");
        value.granted = granted_byte != 0;
        if (value.granted && (zero(value.session) || value.generation == 0))
            return fail("invalid acquire response");
        message = value;
        break;
    }
    case MessageType::kActivateRequest: {
        ActivateRequest value;
        if (!r.id(value.session) || !r.u64(value.generation) || zero(value.session) ||
            value.generation == 0)
            return fail("invalid activate request");
        message = value;
        break;
    }
    case MessageType::kActivateAck: {
        ActivateAck value;
        bool success{};
        if (!r.id(value.session) || !r.u64(value.generation) || !read_bool(r, success) ||
            !r.string(value.error))
            return fail("invalid activate ack");
        value.success = success;
        if (value.success && (zero(value.session) || value.generation == 0))
            return fail("invalid activate ack");
        message = value;
        break;
    }
    case MessageType::kRenewRequest: {
        RenewRequest value;
        if (!r.id(value.session) || !r.u64(value.generation) || zero(value.session) ||
            value.generation == 0)
            return fail("invalid renew request");
        message = value;
        break;
    }
    case MessageType::kRenewAck: {
        RenewAck value;
        bool success{};
        if (!r.id(value.session) || !r.u64(value.generation) || !read_bool(r, success) ||
            !r.string(value.error))
            return fail("invalid renew ack");
        value.success = success;
        if (value.success && (zero(value.session) || value.generation == 0))
            return fail("invalid renew ack");
        message = value;
        break;
    }
    case MessageType::kInput: {
        InputMessage value;
        if (!read_input(r, value.event))
            return fail("invalid input");
        message = value;
        break;
    }
    case MessageType::kRelease: {
        Release value;
        if (!r.id(value.session) || !r.u64(value.generation) || zero(value.session) ||
            value.generation == 0)
            return fail("invalid release");
        message = value;
        break;
    }
    case MessageType::kRevoke: {
        Revoke value;
        std::uint8_t reason{};
        if (!r.id(value.session) || !r.u64(value.generation) || !r.u8(reason) ||
            !valid_reason(reason) || zero(value.session) || value.generation == 0)
            return fail("invalid revoke");
        value.reason = static_cast<RevokeReason>(reason);
        message = value;
        break;
    }
    case MessageType::kError: {
        ErrorMessage value;
        if (!r.u16(value.code) || !r.string(value.message))
            return fail("invalid error");
        message = value;
        break;
    }
    default:
        return fail("invalid type");
    }
    if (!r.done())
        return fail("trailing payload");
    return Result<Message>::success(std::move(message));
}

Result<std::vector<Message>> FrameDecoder::feed(std::span<const std::uint8_t> bytes) {
    std::vector<Message> messages;
    const auto drain = [&]() -> Result<void> {
        while (true) {
            if (buffer_.size() < 4)
                break;
            if (std::memcmp(buffer_.data(), "LLNK", 4) != 0)
                return Result<void>::failure("invalid magic");
            if (buffer_.size() < kHeaderSize)
                break;
            if (buffer_[4] != kProtocolVersion || buffer_[6] != 0 || buffer_[7] != 0 ||
                !valid_type(buffer_[5]))
                return Result<void>::failure("invalid header");
            const std::uint32_t length = (std::uint32_t(buffer_[8]) << 24) |
                                         (std::uint32_t(buffer_[9]) << 16) |
                                         (std::uint32_t(buffer_[10]) << 8) | buffer_[11];
            if (length > kMaxFrameSize - kHeaderSize)
                return Result<void>::failure("frame too large");
            const std::size_t total = kHeaderSize + length;
            if (buffer_.size() < total)
                break;
            auto result = decode(std::span<const std::uint8_t>(buffer_.data(), total));
            if (!result.ok())
                return Result<void>::failure(result.error());
            messages.push_back(std::move(result.value()));
            buffer_.erase(buffer_.begin(), buffer_.begin() + static_cast<std::ptrdiff_t>(total));
        }
        return Result<void>::success();
    };

    std::size_t offset = 0;
    while (offset < bytes.size()) {
        auto drained = drain();
        if (!drained.ok()) {
            buffer_.clear();
            return Result<std::vector<Message>>::failure(drained.error());
        }
        if (buffer_.size() == kMaxFrameSize) {
            buffer_.clear();
            return Result<std::vector<Message>>::failure("frame buffer too large");
        }
        const std::size_t room = kMaxFrameSize - buffer_.size();
        const std::size_t count = std::min(room, bytes.size() - offset);
        buffer_.insert(buffer_.end(), bytes.begin() + static_cast<std::ptrdiff_t>(offset),
                       bytes.begin() + static_cast<std::ptrdiff_t>(offset + count));
        offset += count;
    }
    auto drained = drain();
    if (!drained.ok()) {
        buffer_.clear();
        return Result<std::vector<Message>>::failure(drained.error());
    }
    return Result<std::vector<Message>>::success(std::move(messages));
}

Result<void> FrameDecoder::finish() {
    if (!buffer_.empty()) {
        buffer_.clear();
        return Result<void>::failure("truncated frame");
    }
    return Result<void>::success();
}

} // namespace lanlink::protocol

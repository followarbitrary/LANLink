#include "lanlink/transport/session.hpp"

#include "lanlink/common/config.hpp"
#include "lanlink/common/debug.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <utility>

namespace lanlink::transport {
namespace {

bool is_timeout(const std::string& error) { return error == "read timeout"; }

bool input_forwarding_enabled(const PeerSessionConfig& config) {
    return !config.input_forwarding_enabled || config.input_forwarding_enabled();
}

bool clipboard_sync_enabled(const PeerSessionConfig& config) {
    return !config.clipboard_sync_enabled || config.clipboard_sync_enabled();
}

std::uint8_t clipboard_format_bit(protocol::ClipboardFormat format) {
    return static_cast<std::uint8_t>(1U << (static_cast<std::uint8_t>(format) - 1U));
}

bool format_present(std::uint8_t mask, protocol::ClipboardFormat format) {
    return (mask & clipboard_format_bit(format)) != 0;
}

bool complete(const std::vector<std::uint8_t>& coverage) {
    return std::all_of(coverage.begin(), coverage.end(), [](std::uint8_t value) {
        return value != 0;
    });
}

} // namespace

PeerSession::PeerSession(Connection connection, PeerSessionConfig config)
    : connection_(std::move(connection)), config_(config) {}

Result<void> PeerSession::start(IClock::time_point now) {
    if (started_)
        return Result<void>::success();
    if (config_.router)
        config_.router->clear_peer_display_info();
    local_display_info_.reset();
    debug::log("send hello local_device=", config_.local_device.low,
               " protocol=", static_cast<unsigned>(protocol::kProtocolVersion));
    auto sent = connection_.send(protocol::Hello{config_.local_device, protocol::kProtocolVersion});
    if (!sent.ok())
        return fail_and_disconnect("hello send failed", now);
    auto display = send_display_info_if_changed(true, now);
    if (!display.ok())
        return display;
    started_ = true;
    next_heartbeat_ = now + m0::kHeartbeatInterval;
    next_display_info_check_ = now + m0::kHeartbeatInterval;
    next_pointer_position_ = now;
    handshake_deadline_ = now + m0::kConnectionTimeout;
    return Result<void>::success();
}

Result<void> PeerSession::send_display_info_if_changed(bool force, IClock::time_point now) {
    if (config_.local_display == DisplayId{} || config_.backend == nullptr)
        return Result<void>::success();
    const auto bounds = config_.backend->display_bounds();
    if (!bounds.has_value())
        return Result<void>::success();
    auto scale = config_.backend->display_scale();
    if (!std::isfinite(scale) || scale <= 0.0 || scale > protocol::kMaxDisplayScale)
        scale = 1.0;
    protocol::DisplayInfo info{config_.local_display, *bounds, scale,
                               config_.backend->supports_absolute_pointer_moves()};
    if (!force && local_display_info_.has_value() && *local_display_info_ == info)
        return Result<void>::success();
    auto sent = connection_.send(info);
    if (!sent.ok())
        return fail_and_disconnect("display info send failed", now);
    local_display_info_ = info;
    debug::log("send display info display=", info.display.low,
               " bounds=", info.logical_bounds.left, ",", info.logical_bounds.top, " ",
               info.logical_bounds.right, "x", info.logical_bounds.bottom,
               " scale=", info.scale,
               " absolute_pointer_moves=", info.absolute_pointer_moves);
    return Result<void>::success();
}

Result<void> PeerSession::send_pointer_position_if_due(IClock::time_point now) {
    if (now < next_pointer_position_)
        return Result<void>::success();
    next_pointer_position_ = now + m0::kPointerPositionInterval;
    if (config_.arbiter == nullptr || config_.backend == nullptr ||
        !config_.arbiter->has_active_lease())
        return Result<void>::success();
    const auto lease = config_.arbiter->lease();
    if (!lease.has_value() || now >= lease->expires_at)
        return Result<void>::success();
    const auto position = config_.backend->pointer_position();
    if (!position.has_value() || !std::isfinite(position->x) ||
        !std::isfinite(position->y))
        return Result<void>::success();
    debug::log("send pointer position session=", lease->session.low,
               " generation=", lease->generation, " point=", position->x, ",", position->y);
    auto sent = connection_.send(
        protocol::PointerPosition{lease->session, lease->generation, *position});
    if (!sent.ok())
        return fail_and_disconnect("pointer position send failed", now);
    return Result<void>::success();
}

Result<void> PeerSession::send_actions(const std::vector<RouterAction>& actions) {
    if (!started_ || disconnected_)
        return Result<void>::failure("session is closed");
    return dispatch_actions(actions, IClock::time_point{});
}


void PeerSession::emit_file_update(const FileTransferStatus& update) const {
    if (config_.file_status_callback)
        config_.file_status_callback(update);
}

void PeerSession::clear_file_state() noexcept {
    if (file_receive_.has_value()) {
        file_receive_->stream.close();
        std::error_code ec;
        std::filesystem::remove(file_receive_->temporary_path, ec);
    }
    if (file_send_.has_value())
        file_send_->stream.close();
    file_receive_.reset();
    file_send_.reset();
}

void PeerSession::fail_file(const std::string& message, bool notify_peer, IClock::time_point now) {
    if (file_send_.has_value()) {
        const auto& state = *file_send_;
        if (notify_peer && connection_.connected())
            (void)connection_.send(protocol::FileStatus{config_.local_device, state.transfer,
                                                         protocol::FileStatusCode::kFailed,
                                                         state.offset, state.total_size, message});
        emit_file_update(FileTransferStatus{state.transfer, state.name,
                                            protocol::FileStatusCode::kFailed, state.offset,
                                            state.total_size, false, message});
    } else if (file_receive_.has_value()) {
        const auto& state = *file_receive_;
        if (notify_peer && connection_.connected())
            (void)connection_.send(protocol::FileStatus{config_.local_device, state.transfer,
                                                         protocol::FileStatusCode::kFailed,
                                                         state.offset, state.total_size, message});
        emit_file_update(FileTransferStatus{state.transfer, state.name,
                                            protocol::FileStatusCode::kFailed, state.offset,
                                            state.total_size, true, message});
    }
    (void)now;
    clear_file_state();
}

Result<Uuid128> PeerSession::send_file(const std::filesystem::path& path,
                                       std::string display_name) {
    if (!started_ || disconnected_)
        return Result<Uuid128>::failure("session is closed");
    if (file_send_.has_value() || file_receive_.has_value())
        return Result<Uuid128>::failure("file transfer already active");
    std::error_code ec;
    if (!std::filesystem::is_regular_file(path, ec) || ec)
        return Result<Uuid128>::failure("file does not exist");
    const auto size = std::filesystem::file_size(path, ec);
    if (ec || size > protocol::kMaxFileSize)
        return Result<Uuid128>::failure("file size is invalid");
    if (display_name.empty()) display_name = path.filename().string();
    if (display_name.empty() || display_name.size() > protocol::kMaxFileNameSize ||
        display_name.find('/') != std::string::npos || display_name.find('\\') != std::string::npos)
        return Result<Uuid128>::failure("file name is invalid");
    std::ifstream stream(path, std::ios::binary);
    if (!stream)
        return Result<Uuid128>::failure("file cannot be opened");
    Uuid128 transfer{config_.local_device.high, next_file_transfer_++};
    if (transfer.low == 0) transfer.low = next_file_transfer_++;
    file_send_ = FileSendState{transfer, path, std::move(display_name), std::move(stream), size, 0, false};
    auto sent = connection_.send(protocol::FileBegin{config_.local_device, transfer,
                                                      file_send_->name, size});
    if (!sent.ok()) {
        clear_file_state();
        return Result<Uuid128>::failure(sent.error());
    }
    emit_file_update(FileTransferStatus{transfer, file_send_->name,
                                        protocol::FileStatusCode::kAccepted, 0, size, false, {}});
    return Result<Uuid128>::success(transfer);
}

Result<void> PeerSession::cancel_file(const Uuid128& transfer) {
    if ((!file_send_.has_value() || file_send_->transfer != transfer) &&
        (!file_receive_.has_value() || file_receive_->transfer != transfer))
        return Result<void>::failure("file transfer is not active");
    auto sent = connection_.send(protocol::FileCancel{config_.local_device, transfer, "canceled"});
    if (file_send_.has_value() && file_send_->transfer == transfer) {
        const auto name = file_send_->name;
        const auto offset = file_send_->offset;
        const auto total = file_send_->total_size;
        clear_file_state();
        emit_file_update(FileTransferStatus{transfer, name,
                                            protocol::FileStatusCode::kCanceled, offset,
                                            total, false, "canceled"});
    } else {
        const auto name = file_receive_->name;
        const auto offset = file_receive_->offset;
        const auto total = file_receive_->total_size;
        clear_file_state();
        emit_file_update(FileTransferStatus{transfer, name,
                                            protocol::FileStatusCode::kCanceled, offset,
                                            total, true, "canceled"});
    }
    return sent;
}

Result<void> PeerSession::send_clipboard(const platform::ClipboardData& data) {
    if (!started_ || disconnected_)
        return Result<void>::failure("session is closed");
    if (!clipboard_sync_enabled(config_))
        return Result<void>::failure("clipboard synchronization disabled");
    const auto valid = platform::validate_clipboard_data(data);
    if (!valid.ok())
        return valid;
    if (clipboard_fingerprint_.has_value() && *clipboard_fingerprint_ == data)
        return Result<void>::success();

    auto sequence = next_clipboard_sequence_++;
    if (sequence == 0) {
        sequence = next_clipboard_sequence_++;
    }
    std::uint8_t format_mask = 0;
    if (data.text.has_value()) format_mask |= clipboard_format_bit(protocol::ClipboardFormat::kText);
    if (data.html.has_value()) format_mask |= clipboard_format_bit(protocol::ClipboardFormat::kHtml);
    if (data.png.has_value()) format_mask |= clipboard_format_bit(protocol::ClipboardFormat::kPng);
    const auto text_size = static_cast<std::uint32_t>(data.text ? data.text->size() : 0U);
    const auto html_size = static_cast<std::uint32_t>(data.html ? data.html->size() : 0U);
    const auto png_size = static_cast<std::uint32_t>(data.png ? data.png->size() : 0U);
    const auto total_size = text_size + html_size + png_size;
    auto sent = connection_.send(protocol::ClipboardBegin{
        config_.local_device, sequence, format_mask, total_size, text_size, html_size, png_size});
    if (!sent.ok())
        return fail_and_disconnect("clipboard begin send failed", IClock::time_point{});

    const auto send_format = [&](protocol::ClipboardFormat format,
                                 const std::vector<std::uint8_t>& bytes) -> Result<void> {
        for (std::size_t offset = 0; offset < bytes.size();) {
            const auto length = std::min<std::size_t>(protocol::kMaxClipboardChunkDataSize,
                                                       bytes.size() - offset);
            std::vector<std::uint8_t> chunk(bytes.begin() + static_cast<std::ptrdiff_t>(offset),
                                            bytes.begin() +
                                                static_cast<std::ptrdiff_t>(offset + length));
            auto result = connection_.send(protocol::ClipboardChunk{
                config_.local_device, sequence, format,
                static_cast<std::uint32_t>(offset), std::move(chunk)});
            if (!result.ok())
                return fail_and_disconnect("clipboard chunk send failed", IClock::time_point{});
            offset += length;
        }
        return Result<void>::success();
    };
    if (data.text.has_value()) {
        const std::vector<std::uint8_t> bytes(data.text->begin(), data.text->end());
        sent = send_format(protocol::ClipboardFormat::kText, bytes);
        if (!sent.ok()) return sent;
    }
    if (data.html.has_value()) {
        const std::vector<std::uint8_t> bytes(data.html->begin(), data.html->end());
        sent = send_format(protocol::ClipboardFormat::kHtml, bytes);
        if (!sent.ok()) return sent;
    }
    if (data.png.has_value()) {
        sent = send_format(protocol::ClipboardFormat::kPng, *data.png);
        if (!sent.ok()) return sent;
    }
    sent = connection_.send(protocol::ClipboardEnd{config_.local_device, sequence});
    if (!sent.ok())
        return fail_and_disconnect("clipboard end send failed", IClock::time_point{});
    clipboard_fingerprint_ = data;
    return Result<void>::success();
}

void PeerSession::discard_clipboard_state() noexcept {
    clipboard_receive_.reset();
    pending_clipboard_apply_.reset();
}

Result<void> PeerSession::dispatch_clipboard_begin(const protocol::ClipboardBegin& begin,
                                                   IClock::time_point now) {
    (void)now;
    if (config_.peer.id != DeviceId{} && begin.source != config_.peer.id)
        return drop_clipboard("clipboard source identity mismatch");
    const auto expected_total = static_cast<std::uint64_t>(begin.text_size) + begin.html_size +
                                begin.png_size;
    if ((begin.format_mask & ~0x07U) != 0 || expected_total != begin.total_size ||
        begin.total_size > protocol::kMaxClipboardPayloadSize ||
        (!format_present(begin.format_mask, protocol::ClipboardFormat::kText) &&
         begin.text_size != 0) ||
        (!format_present(begin.format_mask, protocol::ClipboardFormat::kHtml) &&
         begin.html_size != 0) ||
        (!format_present(begin.format_mask, protocol::ClipboardFormat::kPng) &&
         begin.png_size != 0)) {
        return drop_clipboard("invalid clipboard begin");
    }
    ClipboardReceiveState transfer;
    transfer.source = begin.source;
    transfer.sequence = begin.sequence;
    transfer.format_mask = begin.format_mask;
    transfer.text_size = begin.text_size;
    transfer.html_size = begin.html_size;
    transfer.png_size = begin.png_size;
    transfer.text.resize(begin.text_size);
    transfer.html.resize(begin.html_size);
    transfer.png.resize(begin.png_size);
    transfer.text_coverage.resize(begin.text_size);
    transfer.html_coverage.resize(begin.html_size);
    transfer.png_coverage.resize(begin.png_size);
    transfer.started_at = now;
    clipboard_receive_ = std::move(transfer);
    return Result<void>::success();
}

Result<void> PeerSession::dispatch_clipboard_chunk(const protocol::ClipboardChunk& chunk,
                                                   IClock::time_point now) {
    (void)now;
    if (config_.peer.id != DeviceId{} && chunk.source != config_.peer.id)
        return drop_clipboard("clipboard source identity mismatch");
    if (!clipboard_receive_.has_value() || clipboard_receive_->source != chunk.source ||
        clipboard_receive_->sequence != chunk.sequence)
        return drop_clipboard("clipboard chunk without active transfer");

    auto& transfer = *clipboard_receive_;
    std::vector<std::uint8_t>* bytes = nullptr;
    std::vector<std::uint8_t>* coverage = nullptr;
    std::uint32_t expected_size = 0;
    switch (chunk.format) {
    case protocol::ClipboardFormat::kText:
        bytes = &transfer.text;
        coverage = &transfer.text_coverage;
        expected_size = transfer.text_size;
        break;
    case protocol::ClipboardFormat::kHtml:
        bytes = &transfer.html;
        coverage = &transfer.html_coverage;
        expected_size = transfer.html_size;
        break;
    case protocol::ClipboardFormat::kPng:
        bytes = &transfer.png;
        coverage = &transfer.png_coverage;
        expected_size = transfer.png_size;
        break;
    }
    if (!format_present(transfer.format_mask, chunk.format) || bytes == nullptr ||
        static_cast<std::uint64_t>(chunk.offset) + chunk.data.size() > expected_size)
        return drop_clipboard("clipboard chunk range is invalid");
    for (std::size_t index = 0; index < chunk.data.size(); ++index) {
        if ((*coverage)[chunk.offset + index] != 0)
            return drop_clipboard("clipboard chunk overlaps previous data");
    }
    std::copy(chunk.data.begin(), chunk.data.end(), bytes->begin() + chunk.offset);
    std::fill(coverage->begin() + chunk.offset,
              coverage->begin() + chunk.offset + chunk.data.size(), 1);
    return Result<void>::success();
}

Result<void> PeerSession::dispatch_clipboard_end(const protocol::ClipboardEnd& end,
                                                 IClock::time_point now) {
    (void)now;
    if (config_.peer.id != DeviceId{} && end.source != config_.peer.id)
        return drop_clipboard("clipboard source identity mismatch");
    if (!clipboard_receive_.has_value() || clipboard_receive_->source != end.source ||
        clipboard_receive_->sequence != end.sequence)
        return drop_clipboard("clipboard end without active transfer");
    auto transfer = std::move(*clipboard_receive_);
    clipboard_receive_.reset();
    if (!complete(transfer.text_coverage) || !complete(transfer.html_coverage) ||
        !complete(transfer.png_coverage))
        return drop_clipboard("incomplete clipboard transfer");

    platform::ClipboardData data;
    if (format_present(transfer.format_mask, protocol::ClipboardFormat::kText))
        data.text = std::string(transfer.text.begin(), transfer.text.end());
    if (format_present(transfer.format_mask, protocol::ClipboardFormat::kHtml))
        data.html = std::string(transfer.html.begin(), transfer.html.end());
    if (format_present(transfer.format_mask, protocol::ClipboardFormat::kPng))
        data.png = std::move(transfer.png);
    const auto valid = platform::validate_clipboard_data(data);
    if (!valid.ok())
        return drop_clipboard(valid.error().c_str());
    if (clipboard_fingerprint_.has_value() && *clipboard_fingerprint_ == data)
        return Result<void>::success();
    pending_clipboard_apply_ = std::move(data);
    return retry_pending_clipboard();
}

Result<void> PeerSession::drop_clipboard(const char* error) {
    debug::log("drop clipboard transfer peer=", config_.peer.id.low, " error=", error);
    clipboard_receive_.reset();
    return Result<void>::success();
}

Result<void> PeerSession::retry_pending_clipboard() {
    if (!pending_clipboard_apply_.has_value()) return Result<void>::success();
    if (!clipboard_sync_enabled(config_)) {
        pending_clipboard_apply_.reset();
        return Result<void>::success();
    }
    const auto& data = *pending_clipboard_apply_;
    if (config_.clipboard_backend != nullptr) {
        auto applied = data.text.has_value() || data.html.has_value() || data.png.has_value()
                           ? config_.clipboard_backend->write(data)
                           : config_.clipboard_backend->clear();
        if (!applied.ok()) {
            debug::log("clipboard backend apply failed peer=", config_.peer.id.low,
                       " error=", applied.error());
            return Result<void>::success();
        }
    }
    clipboard_fingerprint_ = data;
    pending_clipboard_apply_.reset();
    return Result<void>::success();
}


Result<void> PeerSession::dispatch_file_begin(const protocol::FileBegin& begin,
                                              IClock::time_point now) {
    if (config_.peer.id != DeviceId{} && begin.source != config_.peer.id)
        return Result<void>::success();
    if (file_send_.has_value() || file_receive_.has_value()) {
        const std::string message = "file transfer busy";
        (void)connection_.send(protocol::FileStatus{config_.local_device, begin.transfer,
                                                     protocol::FileStatusCode::kFailed, 0,
                                                     begin.total_size, message});
        emit_file_update(FileTransferStatus{begin.transfer, begin.name,
                                            protocol::FileStatusCode::kFailed, 0,
                                            begin.total_size, true, message});
        return Result<void>::success();
    }
    if (config_.receive_directory.empty()) {
        const std::string message = "receive directory unavailable";
        (void)connection_.send(protocol::FileStatus{config_.local_device, begin.transfer,
                                                     protocol::FileStatusCode::kFailed, 0,
                                                     begin.total_size, message});
        emit_file_update(FileTransferStatus{begin.transfer, begin.name,
                                            protocol::FileStatusCode::kFailed, 0,
                                            begin.total_size, true, message});
        return Result<void>::success();
    }
    std::error_code ec;
    std::filesystem::create_directories(config_.receive_directory, ec);
    if (ec) {
        const std::string message = "cannot create receive directory";
        (void)connection_.send(protocol::FileStatus{config_.local_device, begin.transfer,
                                                     protocol::FileStatusCode::kFailed, 0,
                                                     begin.total_size, message});
        emit_file_update(FileTransferStatus{begin.transfer, begin.name,
                                            protocol::FileStatusCode::kFailed, 0,
                                            begin.total_size, true, message});
        return Result<void>::success();
    }
    const auto target = config_.receive_directory / std::filesystem::path(begin.name);
    if (std::filesystem::exists(target, ec) || ec) {
        const std::string message = "destination file exists";
        (void)connection_.send(protocol::FileStatus{config_.local_device, begin.transfer,
                                                     protocol::FileStatusCode::kFailed, 0,
                                                     begin.total_size, message});
        emit_file_update(FileTransferStatus{begin.transfer, begin.name,
                                            protocol::FileStatusCode::kFailed, 0,
                                            begin.total_size, true, message});
        return Result<void>::success();
    }
    const auto temporary = config_.receive_directory /
                           (std::string{"."} + begin.name + ".lanlink-" +
                            std::to_string(begin.source.high) + "-" +
                            std::to_string(begin.source.low) + "-" +
                            std::to_string(begin.transfer.high) + "-" +
                            std::to_string(begin.transfer.low) + ".part");
    std::ofstream stream(temporary, std::ios::binary | std::ios::trunc);
    if (!stream) {
        const std::string message = "cannot open destination file";
        (void)connection_.send(protocol::FileStatus{config_.local_device, begin.transfer,
                                                     protocol::FileStatusCode::kFailed, 0,
                                                     begin.total_size, message});
        emit_file_update(FileTransferStatus{begin.transfer, begin.name,
                                            protocol::FileStatusCode::kFailed, 0,
                                            begin.total_size, true, message});
        return Result<void>::success();
    }
    file_receive_ = FileReceiveState{begin.transfer, begin.name, temporary, target,
                                     std::move(stream), begin.total_size, 0, now};
    emit_file_update(FileTransferStatus{begin.transfer, begin.name,
                                        protocol::FileStatusCode::kAccepted, 0,
                                        begin.total_size, true, {}});
    auto sent = connection_.send(protocol::FileStatus{config_.local_device, begin.transfer,
                                                      protocol::FileStatusCode::kAccepted, 0,
                                                      begin.total_size, {}});
    if (!sent.ok()) return sent;
    return Result<void>::success();
}

Result<void> PeerSession::dispatch_file_chunk(const protocol::FileChunk& chunk,
                                               IClock::time_point now) {
    (void)now;
    if (!file_receive_.has_value() || file_receive_->transfer != chunk.transfer)
        return Result<void>::success();
    auto& state = *file_receive_;
    if (chunk.offset != state.offset || (chunk.data.empty() && state.offset != state.total_size)) {
        fail_file("file chunk offset is invalid", true, now);
        return Result<void>::success();
    }
    if (state.offset > state.total_size ||
        chunk.data.size() > state.total_size - state.offset) {
        fail_file("file chunk exceeds file size", true, now);
        return Result<void>::success();
    }
    state.stream.write(reinterpret_cast<const char*>(chunk.data.data()),
                       static_cast<std::streamsize>(chunk.data.size()));
    if (!state.stream) {
        fail_file("file write failed", true, now);
        return Result<void>::success();
    }
    state.offset += chunk.data.size();
    state.started_at = now;
    emit_file_update(FileTransferStatus{state.transfer, state.name,
                                        protocol::FileStatusCode::kProgress, state.offset,
                                        state.total_size, true, {}});
    auto sent = connection_.send(protocol::FileStatus{config_.local_device, state.transfer,
                                                      protocol::FileStatusCode::kProgress,
                                                      state.offset, state.total_size, {}});
    if (!sent.ok()) return sent;
    return Result<void>::success();
}

Result<void> PeerSession::dispatch_file_end(const protocol::FileEnd& end,
                                             IClock::time_point now) {
    if (!file_receive_.has_value() || file_receive_->transfer != end.transfer)
        return Result<void>::success();
    auto& state = *file_receive_;
    if (end.total_size != state.total_size || state.offset != state.total_size) {
        fail_file("file size mismatch", true, now);
        return Result<void>::success();
    }
    state.stream.flush();
    if (!state.stream) {
        fail_file("file flush failed", true, now);
        return Result<void>::success();
    }
    state.stream.close();
    std::error_code ec;
    std::filesystem::rename(state.temporary_path, state.target_path, ec);
    if (ec) {
        fail_file("file rename failed", true, now);
        return Result<void>::success();
    }
    const auto completed = FileTransferStatus{state.transfer, state.name,
                                              protocol::FileStatusCode::kCompleted,
                                              state.offset, state.total_size, true, {}};
    auto sent = connection_.send(protocol::FileStatus{config_.local_device, state.transfer,
                                                      protocol::FileStatusCode::kCompleted,
                                                      state.offset, state.total_size, {}});
    emit_file_update(completed);
    file_receive_.reset();
    return sent;
}

Result<void> PeerSession::dispatch_file_cancel(const protocol::FileCancel& cancel,
                                                IClock::time_point now) {
    if (config_.peer.id != DeviceId{} && cancel.source != config_.peer.id)
        return Result<void>::success();
    if (file_receive_.has_value() && file_receive_->transfer == cancel.transfer) {
        const auto transfer_id = file_receive_->transfer;
        const auto name = file_receive_->name;
        const auto offset = file_receive_->offset;
        const auto total = file_receive_->total_size;
        clear_file_state();
        (void)connection_.send(protocol::FileStatus{config_.local_device, transfer_id,
                                                     protocol::FileStatusCode::kCanceled,
                                                     offset, total, cancel.reason});
        emit_file_update(FileTransferStatus{transfer_id, name,
                                            protocol::FileStatusCode::kCanceled, offset,
                                            total, true, cancel.reason});
    } else if (file_send_.has_value() && file_send_->transfer == cancel.transfer) {
        const auto transfer_id = file_send_->transfer;
        const auto name = file_send_->name;
        const auto offset = file_send_->offset;
        const auto total = file_send_->total_size;
        clear_file_state();
        emit_file_update(FileTransferStatus{transfer_id, name,
                                            protocol::FileStatusCode::kCanceled, offset,
                                            total, false, cancel.reason});
    }
    (void)now;
    return Result<void>::success();
}

Result<void> PeerSession::dispatch_file_status(const protocol::FileStatus& status,
                                                IClock::time_point now) {
    if (config_.peer.id != DeviceId{} && status.source != config_.peer.id)
        return Result<void>::success();
    if (!file_send_.has_value() || file_send_->transfer != status.transfer)
        return Result<void>::success();
    auto& state = *file_send_;
    if (status.total_size != state.total_size || status.bytes > state.total_size)
        return Result<void>::success();
    emit_file_update(FileTransferStatus{state.transfer, state.name, status.status,
                                        status.bytes, status.total_size, false, status.message});
    if (status.status == protocol::FileStatusCode::kFailed ||
        status.status == protocol::FileStatusCode::kCanceled ||
        status.status == protocol::FileStatusCode::kCompleted)
        clear_file_state();
    (void)now;
    return Result<void>::success();
}

Result<void> PeerSession::dispatch_actions(const std::vector<RouterAction>& actions,
                                           IClock::time_point now) {
    for (const auto& action : actions) {
        Result<void> result = Result<void>::success();
        switch (action.kind) {
        case RouterActionKind::kArmLocalSuppression:
            debug::log("suppression on session=", action.session.low,
                       " generation=", action.generation);
            if (config_.backend)
                result = config_.backend->set_suppression(true);
            break;
        case RouterActionKind::kReleaseInjectedState:
            if (config_.backend)
                config_.backend->release_injected_state(action.session);
            break;
        case RouterActionKind::kRestoreLocal:
            debug::log("suppression off/recover");
            if (config_.backend)
                result = config_.backend->recover();
            break;
        case RouterActionKind::kAcquire:
            if (!hello_received_) {
                debug::log("acquire deferred: peer hello not received request=",
                           action.request.low);
                return fail_and_disconnect("peer handshake incomplete", now);
            }
            debug::log("send acquire target_display=", action.target_display.low,
                       " request=", action.request.low, " entry=", action.entry.x, ",",
                       action.entry.y, " target_edge=", static_cast<unsigned>(action.target_edge),
                       " tangent_ratio=", action.tangent_ratio,
                       " normalized=", action.entry_normalized);
            pending_acquire_ = action;
            result = connection_.send(protocol::AcquireRequest{action.source, action.target_display,
                                                               action.request, action.entry,
                                                               action.target_edge,
                                                               action.tangent_ratio,
                                                               action.entry_normalized});
            break;
        case RouterActionKind::kActivate:
            debug::log("send activate session=", action.session.low,
                       " generation=", action.generation);
            result = connection_.send(protocol::ActivateRequest{action.session, action.generation});
            break;
        case RouterActionKind::kRenew:
            debug::log("send renew session=", action.session.low,
                       " generation=", action.generation);
            result = connection_.send(protocol::RenewRequest{action.session, action.generation});
            break;
        case RouterActionKind::kSendInput:
            result = connection_.send(protocol::InputMessage{action.event});
            break;
        case RouterActionKind::kRelease:
            result = connection_.send(protocol::Release{action.session, action.generation});
            break;
        case RouterActionKind::kRevoke:
            debug::log("send lease revoke session=", action.session.low,
                       " generation=", action.generation,
                       " reason=", static_cast<unsigned>(action.reason));
            if (action.has_pointer_position) {
                result = connection_.send(protocol::PointerPosition{
                    action.session, action.generation, action.pointer_position});
            }
            if (result.ok()) {
                result = connection_.send(
                    protocol::Revoke{action.session, action.generation, action.reason});
            }
            break;
        case RouterActionKind::kDrop:
            break;
        }
        if (!result.ok())
            return fail_and_disconnect("message send failed", now);
    }
    return Result<void>::success();
}

Result<void> PeerSession::pump(IClock::time_point now, std::chrono::milliseconds timeout) {
    if (!started_ || disconnected_)
        return Result<void>::failure("session is closed");
    auto received = connection_.receive(timeout);
    if (!received.ok()) {
        if (is_timeout(received.error()))
            return Result<void>::success();
        return fail_and_disconnect(received.error().c_str(), now);
    }
    for (const auto& message : received.value()) {
        auto result = dispatch_message(message, now);
        if (!result.ok())
            return result;
    }
    return Result<void>::success();
}

Result<void> PeerSession::dispatch_message(const protocol::Message& message,
                                           IClock::time_point now) {
    if (const auto* hello = std::get_if<protocol::Hello>(&message)) {
        debug::log("receive hello device=", hello->device.low,
                   " protocol=", static_cast<unsigned>(hello->advertised_version));
        if (config_.peer.id != DeviceId{} && config_.peer.id != hello->device)
            return fail_and_disconnect("peer identity mismatch", now);
        hello_received_ = true;
        liveness_.mark_alive(now);
        return Result<void>::success();
    }
    if (const auto* display = std::get_if<protocol::DisplayInfo>(&message)) {
        if (!hello_received_)
            return fail_and_disconnect("hello required", now);
        debug::log("receive display info display=", display->display.low,
                   " bounds=", display->logical_bounds.left, ",", display->logical_bounds.top,
                   " ", display->logical_bounds.right, "x", display->logical_bounds.bottom,
                   " scale=", display->scale,
                   " absolute_pointer_moves=", display->absolute_pointer_moves);
        if (config_.router)
            config_.router->set_peer_display_info(display->display, display->logical_bounds,
                                                  display->scale, display->absolute_pointer_moves);
        return Result<void>::success();
    }
    if (const auto* position = std::get_if<protocol::PointerPosition>(&message)) {
        if (!hello_received_)
            return fail_and_disconnect("hello required", now);
        liveness_.mark_alive(now);
        debug::log("receive pointer position session=", position->session.low,
                   " generation=", position->generation, " point=", position->position.x, ",",
                   position->position.y);
        if (config_.pointer_position_callback)
            config_.pointer_position_callback(position->session, position->generation,
                                              position->position);
        return Result<void>::success();
    }
    if (const auto* heartbeat = std::get_if<protocol::Heartbeat>(&message)) {
        if (!hello_received_)
            return fail_and_disconnect("hello required", now);
        (void)heartbeat;
        liveness_.mark_alive(now);
        return Result<void>::success();
    }
    if (!hello_received_)
        return fail_and_disconnect("hello required", now);
    liveness_.mark_alive(now);

    if (std::holds_alternative<protocol::FileBegin>(message) ||
        std::holds_alternative<protocol::FileChunk>(message) ||
        std::holds_alternative<protocol::FileEnd>(message) ||
        std::holds_alternative<protocol::FileCancel>(message) ||
        std::holds_alternative<protocol::FileStatus>(message)) {
        if (const auto* begin = std::get_if<protocol::FileBegin>(&message))
            return dispatch_file_begin(*begin, now);
        if (const auto* chunk = std::get_if<protocol::FileChunk>(&message))
            return dispatch_file_chunk(*chunk, now);
        if (const auto* end = std::get_if<protocol::FileEnd>(&message))
            return dispatch_file_end(*end, now);
        if (const auto* cancel = std::get_if<protocol::FileCancel>(&message))
            return dispatch_file_cancel(*cancel, now);
        return dispatch_file_status(std::get<protocol::FileStatus>(message), now);
    }

    if (std::holds_alternative<protocol::ClipboardBegin>(message) ||
        std::holds_alternative<protocol::ClipboardChunk>(message) ||
        std::holds_alternative<protocol::ClipboardEnd>(message)) {
        if (!clipboard_sync_enabled(config_)) {
            clipboard_receive_.reset();
            pending_clipboard_apply_.reset();
            return Result<void>::success();
        }
        if (const auto* begin = std::get_if<protocol::ClipboardBegin>(&message))
            return dispatch_clipboard_begin(*begin, now);
        if (const auto* chunk = std::get_if<protocol::ClipboardChunk>(&message))
            return dispatch_clipboard_chunk(*chunk, now);
        return dispatch_clipboard_end(std::get<protocol::ClipboardEnd>(message), now);
    }

    if (std::holds_alternative<protocol::ErrorMessage>(message))
        return fail_and_disconnect("peer reported protocol error", now);

    if (const auto* request = std::get_if<protocol::AcquireRequest>(&message)) {
        if (!config_.arbiter)
            return fail_and_disconnect("unexpected acquire request", now);
        if (!input_forwarding_enabled(config_)) {
            auto sent = connection_.send(protocol::AcquireResponse{
                request->request, false, {}, 0, "input forwarding disabled"});
            if (!sent.ok())
                return fail_and_disconnect("acquire response failed", now);
            return Result<void>::success();
        }
        debug::log("receive acquire source=", request->source.low,
                   " target_display=", request->target_display.low,
                   " request=", request->request.low,
                   " target_edge=", static_cast<unsigned>(request->target_edge),
                   " tangent_ratio=", request->tangent_ratio,
                   " normalized=", request->entry_normalized);
        pending_target_acquire_ = *request;
        const auto result = config_.arbiter->acquire(
            config_.peer, request->source, request->target_display, request->request, now);
        protocol::AcquireResponse response{
            request->request, result.granted(), {}, 0, result.error_message};
        if (result.granted()) {
            response.session = result.lease.session;
            response.generation = result.lease.generation;
        } else {
            pending_target_acquire_.reset();
        }
        debug::log("acquire ", result.granted() ? "granted" : "rejected",
                   " request=", request->request.low,
                   result.granted() ? "" : " error=" + result.error_message);
        auto sent = connection_.send(response);
        if (!sent.ok())
            return fail_and_disconnect("acquire response failed", now);
        return Result<void>::success();
    }
    if (const auto* response = std::get_if<protocol::AcquireResponse>(&message)) {
        if (!config_.router || !pending_acquire_ || pending_acquire_->request != response->request)
            return fail_and_disconnect("unexpected acquire response", now);
        debug::log("receive acquire response request=", response->request.low,
                   " granted=", response->granted,
                   response->granted ? "" : " error=" + response->error);
        AcquireResult result;
        result.granted_value = response->granted;
        result.error_message = response->error;
        if (response->granted) {
            result.lease.peer = config_.peer;
            result.lease.source = pending_acquire_->source;
            result.lease.target_display = pending_acquire_->target_display;
            result.lease.request = response->request;
            result.lease.session = response->session;
            result.lease.generation = response->generation;
            result.lease.expires_at = now + m0::kLeaseDuration;
        }
        pending_acquire_.reset();
        return dispatch_actions(config_.router->on_grant(std::move(result), now), now);
    }
    if (const auto* request = std::get_if<protocol::ActivateRequest>(&message)) {
        if (!config_.arbiter)
            return fail_and_disconnect("unexpected activate request", now);
        if (!input_forwarding_enabled(config_)) {
            if (config_.arbiter->has_lease()) {
                config_.arbiter->revoke_current(RevokeReason::kExplicitRelease, now);
                if (config_.backend)
                    config_.backend->release_injected_state(request->session);
            }
            pending_target_acquire_.reset();
            auto sent = connection_.send(protocol::ActivateAck{
                request->session, request->generation, false, "input forwarding disabled"});
            if (!sent.ok())
                return fail_and_disconnect("activate response failed", now);
            return Result<void>::success();
        }
        debug::log("receive activate session=", request->session.low,
                   " generation=", request->generation);
        const auto result = config_.arbiter->activate(request->session, request->generation, now);
        bool success = result.ok();
        std::string error = result.ok() ? std::string{} : result.error();
        const bool resume_pointer =
            success && config_.arbiter->lease().has_value() &&
            config_.arbiter->lease()->session == request->session &&
            config_.arbiter->lease()->generation == request->generation &&
            config_.arbiter->lease()->resume_pointer;
        if (success && config_.backend && pending_target_acquire_.has_value() &&
            pending_target_acquire_->request == request->session && !resume_pointer) {
            Point entry = pending_target_acquire_->entry;
            if (pending_target_acquire_->entry_normalized) {
                const auto bounds = config_.backend->display_bounds();
                if (bounds.has_value()) {
                    debug::log("target display bounds=", bounds->left, ",", bounds->top,
                               " ", bounds->right, "x", bounds->bottom);
                    entry = edge_entry_point(*bounds, pending_target_acquire_->target_edge,
                                             pending_target_acquire_->tangent_ratio);
                } else {
                    debug::log("target display bounds unavailable; using wire entry");
                }
            }
            InputEvent warp{pending_target_acquire_->source,
                            request->session,
                            request->generation,
                            0,
                            PointerWarp{entry.x, entry.y},
                            InputOrigin::kRemoteInjected};
            debug::log("target warp session=", request->session.low, " point=", entry.x, ",",
                       entry.y);
            const auto injected = config_.backend->inject(warp);
            if (!injected.ok()) {
                debug::log("target warp failed session=", request->session.low,
                           " error=", injected.error());
                config_.arbiter->revoke_current(RevokeReason::kProtocolError, now);
                config_.backend->release_injected_state(request->session);
                success = false;
                error = injected.error();
            }
        } else if (resume_pointer) {
            debug::log("target pointer resume session=", request->session.low);
        }
        pending_target_acquire_.reset();
        auto sent = connection_.send(protocol::ActivateAck{
            request->session, request->generation, success, error});
        debug::log("activate ", success ? "completed" : "failed",
                   " session=", request->session.low,
                   success ? "" : " error=" + error);
        if (!sent.ok())
            return fail_and_disconnect("activate response failed", now);
        return Result<void>::success();
    }
    if (const auto* ack = std::get_if<protocol::ActivateAck>(&message)) {
        if (!config_.router)
            return fail_and_disconnect("unexpected activate response", now);
        debug::log("receive activate ack session=", ack->session.low,
                   " success=", ack->success,
                   ack->success ? "" : " error=" + ack->error);
        return dispatch_actions(
            config_.router->on_activation_ack(ack->session, ack->generation, ack->success, now),
            now);
    }
    if (const auto* request = std::get_if<protocol::RenewRequest>(&message)) {
        if (!config_.arbiter)
            return fail_and_disconnect("unexpected renew request", now);
        if (!input_forwarding_enabled(config_)) {
            if (config_.arbiter->has_lease()) {
                config_.arbiter->revoke_current(RevokeReason::kExplicitRelease, now);
                if (config_.backend)
                    config_.backend->release_injected_state(request->session);
            }
            auto sent = connection_.send(protocol::RenewAck{
                request->session, request->generation, false, "input forwarding disabled"});
            if (!sent.ok())
                return fail_and_disconnect("renew response failed", now);
            return Result<void>::success();
        }
        debug::log("receive renew session=", request->session.low,
                   " generation=", request->generation);
        const auto result =
            config_.arbiter->renew(config_.peer, request->session, request->generation, now);
        debug::log("renew ", result.ok() ? "accepted" : "rejected",
                   " session=", request->session.low,
                   result.ok() ? "" : " error=" + result.error());
        auto sent = connection_.send(protocol::RenewAck{
            request->session, request->generation, result.ok(), result.ok() ? "" : result.error()});
        if (!sent.ok())
            return fail_and_disconnect("renew response failed", now);
        return Result<void>::success();
    }
    if (const auto* ack = std::get_if<protocol::RenewAck>(&message)) {
        if (!config_.router || !config_.router->lease() ||
            config_.router->lease()->session != ack->session ||
            config_.router->lease()->generation != ack->generation)
            return Result<void>::success();
        debug::log("receive renew ack session=", ack->session.low,
                   " generation=", ack->generation,
                   " success=", ack->success,
                   ack->success ? "" : " error=" + ack->error);
        if (!ack->success)
            return dispatch_actions(config_.router->on_revoke(RevokeReason::kProtocolError, now),
                                    now);
        auto lease = *config_.router->lease();
        lease.expires_at = now + m0::kLeaseDuration;
        return dispatch_actions(config_.router->on_renew_ack(lease, now), now);
    }
    if (const auto* input = std::get_if<protocol::InputMessage>(&message)) {
        if (!config_.arbiter)
            return fail_and_disconnect("unexpected input message", now);
        if (!input_forwarding_enabled(config_)) {
            if (config_.arbiter->has_lease()) {
                config_.arbiter->revoke_current(RevokeReason::kExplicitRelease, now);
                if (config_.backend)
                    config_.backend->release_injected_state(input->event.session);
            }
            auto sent = connection_.send(protocol::Revoke{
                input->event.session, input->event.generation, RevokeReason::kExplicitRelease});
            if (!sent.ok())
                return fail_and_disconnect("revoke send failed", now);
            return Result<void>::success();
        }
        auto validation = config_.arbiter->validate(config_.peer, input->event, now);
        if (validation.revoked()) {
            debug::log("lease revoke on input session=", input->event.session.low,
                       " generation=", input->event.generation,
                       " reason=", static_cast<unsigned>(validation.reason()),
                       " error=", validation.error());
            auto sent = connection_.send(protocol::Revoke{
                input->event.session, input->event.generation, validation.reason()});
            if (!sent.ok())
                return fail_and_disconnect("revoke send failed", now);
            return Result<void>::success();
        }
        if (validation.accepted() && config_.backend) {
            auto injected = config_.backend->inject(input->event);
            if (!injected.ok()) {
                debug::log("remote input injection failed session=", input->event.session.low,
                           " error=", injected.error());
                config_.arbiter->revoke_current(RevokeReason::kProtocolError, now);
                auto sent = connection_.send(protocol::Revoke{
                    input->event.session, input->event.generation, RevokeReason::kProtocolError});
                if (!sent.ok())
                    return fail_and_disconnect("revoke send failed", now);
            }
        }
        return Result<void>::success();
    }
    if (const auto* release = std::get_if<protocol::Release>(&message)) {
        if (!config_.arbiter)
            return fail_and_disconnect("unexpected release message", now);
        debug::log("lease release session=", release->session.low,
                   " generation=", release->generation);
        config_.arbiter->release(release->session, now);
        if (config_.backend)
            config_.backend->release_injected_state(release->session);
        return Result<void>::success();
    }
    if (const auto* revoke = std::get_if<protocol::Revoke>(&message)) {
        if (!config_.router)
            return fail_and_disconnect("unexpected revoke message", now);
        debug::log("receive lease revoke session=", revoke->session.low,
                   " generation=", revoke->generation,
                   " reason=", static_cast<unsigned>(revoke->reason));
        return dispatch_actions(config_.router->on_revoke(revoke->reason, now), now);
    }
    return fail_and_disconnect("unsupported protocol message", now);
}

Result<void> PeerSession::apply_local_actions(const std::vector<RouterAction>& actions) {
    for (const auto& action : actions) {
        if (action.kind == RouterActionKind::kArmLocalSuppression) {
            if (config_.backend) {
                auto result = config_.backend->set_suppression(true);
                if (!result.ok())
                    return result;
            }
        } else if (action.kind == RouterActionKind::kReleaseInjectedState) {
            if (config_.backend)
                config_.backend->release_injected_state(action.session);
        } else if (action.kind == RouterActionKind::kRestoreLocal && config_.backend) {
            debug::log("suppression off/recover (session cleanup)");
            auto result = config_.backend->recover();
            if (!result.ok())
                return result;
        }
    }
    return Result<void>::success();
}

Result<void> PeerSession::disconnect(IClock::time_point now) {
    if (disconnected_)
        return Result<void>::success();
    debug::log("session disconnect peer=", config_.peer.id.low);
    disconnected_ = true;
    if (file_send_.has_value() || file_receive_.has_value())
        fail_file("connection closed", false, now);
    connection_.close();
    if (config_.router) {
        config_.router->clear_peer_display_info();
        auto actions = config_.router->on_disconnect(now);
        auto local = apply_local_actions(actions);
        if (!local.ok())
            return local;
    }
    if (config_.arbiter && config_.arbiter->has_lease()) {
        const auto session = config_.arbiter->lease()->session;
        config_.arbiter->revoke_current(RevokeReason::kProtocolError, now);
        if (config_.backend)
            config_.backend->release_injected_state(session);
    }
    if (config_.arbiter)
        config_.arbiter->discard_resume_pointer();
    if (config_.backend && !config_.router)
        return config_.backend->recover();
    return Result<void>::success();
}

Result<void> PeerSession::fail_and_disconnect(const char* error, IClock::time_point now) {
    debug::log("session failure peer=", config_.peer.id.low, " error=", error);
    disconnect(now);
    return Result<void>::failure(error);
}

Result<void> PeerSession::tick(IClock::time_point now) {
    if (!started_ || disconnected_)
        return Result<void>::failure("session is closed");
    if (!hello_received_ && now >= handshake_deadline_)
        return fail_and_disconnect("hello timeout", now);
    if (hello_received_ && liveness_.expired(now))
        return fail_and_disconnect("connection timeout", now);
    if (clipboard_receive_.has_value() &&
        now - clipboard_receive_->started_at >= m0::kConnectionTimeout) {
        debug::log("clipboard transfer timed out peer=", config_.peer.id.low,
                   " sequence=", clipboard_receive_->sequence);
        clipboard_receive_.reset();
    }
    if (file_receive_.has_value() &&
        now - file_receive_->started_at >= m0::kConnectionTimeout) {
        fail_file("file transfer timeout", true, now);
    }
    (void)retry_pending_clipboard();
    if (now >= next_display_info_check_) {
        auto display = send_display_info_if_changed(false, now);
        if (!display.ok())
            return display;
        next_display_info_check_ = now + m0::kHeartbeatInterval;
    }
    auto pointer = send_pointer_position_if_due(now);
    if (!pointer.ok())
        return pointer;
    if (file_send_.has_value()) {
        auto& state = *file_send_;
        if (state.offset < state.total_size) {
            const auto length = static_cast<std::size_t>(std::min<std::uint64_t>(
                protocol::kMaxFileChunkDataSize, state.total_size - state.offset));
            std::vector<std::uint8_t> data(length);
            state.stream.read(reinterpret_cast<char*>(data.data()),
                              static_cast<std::streamsize>(length));
            const auto read = static_cast<std::size_t>(state.stream.gcount());
            if (read != length) {
                fail_file("file read failed", true, now);
            } else {
                auto sent = connection_.send(protocol::FileChunk{config_.local_device,
                                                                   state.transfer, state.offset,
                                                                   std::move(data)});
                if (!sent.ok()) return fail_and_disconnect("file chunk send failed", now);
                state.offset += length;
                emit_file_update(FileTransferStatus{state.transfer, state.name,
                                                    protocol::FileStatusCode::kProgress,
                                                    state.offset, state.total_size, false, {}});
            }
        } else if (!state.end_sent) {
            auto sent = connection_.send(protocol::FileEnd{config_.local_device, state.transfer,
                                                             state.total_size});
            if (!sent.ok()) return fail_and_disconnect("file end send failed", now);
            emit_file_update(FileTransferStatus{state.transfer, state.name,
                                                protocol::FileStatusCode::kProgress,
                                                state.offset, state.total_size, false, {}});
            // Keep the send state until the receiver acknowledges completion.
            state.end_sent = true;
        }
    }
    if (now >= next_heartbeat_) {
        auto result = connection_.send(protocol::Heartbeat{++heartbeat_counter_});
        if (!result.ok())
            return fail_and_disconnect("heartbeat send failed", now);
        next_heartbeat_ = now + m0::kHeartbeatInterval;
    }
    return Result<void>::success();
}

} // namespace lanlink::transport

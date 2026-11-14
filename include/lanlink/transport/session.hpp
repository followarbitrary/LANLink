#pragma once

#include "lanlink/common/clock.hpp"
#include "lanlink/common/ids.hpp"
#include "lanlink/common/liveness.hpp"
#include "lanlink/common/result.hpp"
#include "lanlink/input/arbiter.hpp"
#include "lanlink/input/router.hpp"
#include "lanlink/platform/clipboard_backend.hpp"
#include "lanlink/platform/input_backend.hpp"
#include "lanlink/transport/connection.hpp"

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <fstream>
#include <optional>
#include <vector>

namespace lanlink::transport {

struct FileTransferStatus {
    using State = protocol::FileStatusCode;
    Uuid128 transfer;
    std::string name;
    State state{State::kAccepted};
    std::uint64_t bytes{};
    std::uint64_t total_size{};
    bool incoming{false};
    std::string message;
};
using FileTransferUpdate = FileTransferStatus;

struct PeerSessionConfig {
    DeviceId local_device;
    AuthenticatedPeer peer;
    InputRouter* router{nullptr};
    InputArbiter* arbiter{nullptr};
    platform::IInputBackend* backend{nullptr};
    DisplayId local_display{};
    std::function<void(SessionId, std::uint64_t, Point)> pointer_position_callback;
    std::function<bool()> input_forwarding_enabled;
    platform::IClipboardBackend* clipboard_backend{nullptr};
    std::function<bool()> clipboard_sync_enabled;
    std::filesystem::path receive_directory;
    std::function<void(const FileTransferStatus&)> file_status_callback;
};

class PeerSession {
  public:
    PeerSession(Connection connection, PeerSessionConfig config);

    Result<void> start(IClock::time_point now);
    Result<void> send_actions(const std::vector<RouterAction>& actions);
    Result<void> send_clipboard(const platform::ClipboardData& data);
    Result<Uuid128> send_file(const std::filesystem::path& path,
                              std::string display_name = {});
    Result<void> cancel_file(const Uuid128& transfer);
    void discard_clipboard_state() noexcept;
    Result<void> pump(IClock::time_point now,
                      std::chrono::milliseconds timeout = std::chrono::milliseconds{-1});
    Result<void> tick(IClock::time_point now);
    Result<void> disconnect(IClock::time_point now);

    bool connected() const noexcept { return connection_.connected() && !disconnected_; }
    bool hello_received() const noexcept { return hello_received_; }
    // A TCP connection can be established while the peer is still
    // reconnecting. Handoffs are safe only after the hello exchange.
    bool ready() const noexcept { return connected() && hello_received_; }

  private:
    Result<void> dispatch_actions(const std::vector<RouterAction>& actions, IClock::time_point now);
    Result<void> send_display_info_if_changed(bool force, IClock::time_point now);
    Result<void> send_pointer_position_if_due(IClock::time_point now);
    Result<void> dispatch_message(const protocol::Message& message, IClock::time_point now);
    Result<void> apply_local_actions(const std::vector<RouterAction>& actions);
    Result<void> fail_and_disconnect(const char* error, IClock::time_point now);
    Result<void> dispatch_clipboard_begin(const protocol::ClipboardBegin& begin,
                                           IClock::time_point now);
    Result<void> dispatch_clipboard_chunk(const protocol::ClipboardChunk& chunk,
                                          IClock::time_point now);
    Result<void> dispatch_clipboard_end(const protocol::ClipboardEnd& end,
                                        IClock::time_point now);
    Result<void> drop_clipboard(const char* error);
    Result<void> retry_pending_clipboard();
    Result<void> dispatch_file_begin(const protocol::FileBegin& begin, IClock::time_point now);
    Result<void> dispatch_file_chunk(const protocol::FileChunk& chunk, IClock::time_point now);
    Result<void> dispatch_file_end(const protocol::FileEnd& end, IClock::time_point now);
    Result<void> dispatch_file_cancel(const protocol::FileCancel& cancel, IClock::time_point now);
    Result<void> dispatch_file_status(const protocol::FileStatus& status, IClock::time_point now);
    void emit_file_update(const FileTransferUpdate& update) const;
    void fail_file(const std::string& message, bool notify_peer, IClock::time_point now);
    void clear_file_state() noexcept;

    Connection connection_;
    PeerSessionConfig config_;
    ConnectionLiveness liveness_;
    std::optional<RouterAction> pending_acquire_;
    std::optional<protocol::AcquireRequest> pending_target_acquire_;
    std::optional<protocol::DisplayInfo> local_display_info_;
    std::uint64_t heartbeat_counter_{0};
    IClock::time_point next_heartbeat_{};
    IClock::time_point next_display_info_check_{};
    IClock::time_point next_pointer_position_{};
    IClock::time_point handshake_deadline_{};
    bool started_{false};
    bool hello_received_{false};
    bool disconnected_{false};
    std::uint64_t next_clipboard_sequence_{1};
    std::optional<platform::ClipboardData> clipboard_fingerprint_;

    struct ClipboardReceiveState {
        DeviceId source;
        std::uint64_t sequence{};
        std::uint8_t format_mask{};
        std::uint32_t text_size{};
        std::uint32_t html_size{};
        std::uint32_t png_size{};
        std::vector<std::uint8_t> text;
        std::vector<std::uint8_t> html;
        std::vector<std::uint8_t> png;
        std::vector<std::uint8_t> text_coverage;
        std::vector<std::uint8_t> html_coverage;
        std::vector<std::uint8_t> png_coverage;
        IClock::time_point started_at{};
    };
    std::optional<ClipboardReceiveState> clipboard_receive_;
    std::optional<platform::ClipboardData> pending_clipboard_apply_;

    struct FileSendState {
        Uuid128 transfer;
        std::filesystem::path path;
        std::string name;
        std::ifstream stream;
        std::uint64_t total_size{};
        std::uint64_t offset{};
        bool end_sent{false};
    };
    struct FileReceiveState {
        Uuid128 transfer;
        std::string name;
        std::filesystem::path temporary_path;
        std::filesystem::path target_path;
        std::ofstream stream;
        std::uint64_t total_size{};
        std::uint64_t offset{};
        IClock::time_point started_at{};
    };
    std::optional<FileSendState> file_send_;
    std::optional<FileReceiveState> file_receive_;
    std::uint64_t next_file_transfer_{1};
};

} // namespace lanlink::transport

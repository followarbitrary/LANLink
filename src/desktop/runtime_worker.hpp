#pragma once

#include <QObject>
#include <QMetaType>

#include <functional>
#include <filesystem>
#include <memory>
#include <optional>
#include <cstdint>
#include <utility>

#include "lanlink/app/options.hpp"
#include "lanlink/common/result.hpp"
#include "lanlink/discovery/discovery.hpp"
#include "lanlink/platform/clipboard_backend.hpp"
#include "lanlink/runtime/node.hpp"

class QTimer;

Q_DECLARE_METATYPE(lanlink::discovery::TopologySnapshot)

namespace lanlink::desktop {

class RuntimeWorker final : public QObject {
  Q_OBJECT

 public:
  using BackendFactory = std::function<std::unique_ptr<platform::IInputBackend>()>;
  using ClipboardBackendFactory =
      std::function<std::unique_ptr<platform::IClipboardBackend>()>;

  explicit RuntimeWorker(QObject* parent = nullptr, BackendFactory backend_factory = {},
                        bool input_forwarding_enabled = true,
                        ClipboardBackendFactory clipboard_backend_factory = {});

  static Result<void> validate_options(const app::AppOptions& options);

 public slots:
  void start(app::AppOptions options);
  void stop();
  void setRemoteSide(app::RemoteSide side);
  void setInputForwardingEnabled(bool enabled);
  void setClipboardSyncEnabled(bool enabled);
  void recoverLocalInput();
  void sendFile(const QString& path);
  void cancelFile();

  std::uint64_t topology_revision() const noexcept { return local_topology_revision_; }

 signals:
  void statusChanged(const QString& status, bool running, bool connected);
  void peerChanged(const QString& name, const QString& platform, const QString& address,
                   qulonglong deviceId, qulonglong displayId, bool online);
  void logMessage(const QString& message);
  void errorOccurred(const QString& message);
  void fileTransferChanged(const QString& name, qulonglong done, qulonglong total,
                           const QString& status, bool outgoing);
  void remoteTopologyChanged(const discovery::TopologySnapshot& snapshot);
  void finished();

 private slots:
  void pump();

 private:
  void fail(const QString& message);
  void emit_status(const QString& status, bool running, bool connected);
  bool local_id_less(const DeviceId& peer) const;
  bool select_peer(const std::vector<discovery::DiscoveryPeer>& peers);
  bool start_runtime();
  bool update_topology_from_peer(const discovery::DiscoveryPeer& peer);
  bool broadcast_local_topology();
  bool apply_remote_topology_snapshots();
  void emit_peer_snapshot(bool online);
  void stop_runtime();

  QTimer* timer_{nullptr};
  BackendFactory backend_factory_;
  ClipboardBackendFactory clipboard_backend_factory_;
  app::AppOptions options_;
  std::unique_ptr<platform::IInputBackend> backend_;
  std::unique_ptr<platform::IClipboardBackend> clipboard_backend_;
  std::optional<discovery::DiscoveryService> discovery_;
  std::unique_ptr<runtime::InputNodeRuntime> runtime_;
  std::optional<discovery::DiscoveryPeer> selected_peer_;
  std::optional<DeviceId> selected_device_id_;
  bool topology_selected_{false};
  bool outgoing_requested_{false};
  bool running_{false};
  bool was_connected_{false};
  bool input_forwarding_enabled_{true};
  std::optional<Uuid128> active_file_transfer_;
  QString last_status_;
  bool last_status_running_{false};
  bool last_status_connected_{false};
  std::optional<ScreenTopology> local_topology_;
  std::uint64_t local_topology_revision_{0};
  std::optional<std::pair<std::uint64_t, DeviceId>> latest_remote_topology_;
};

}  // namespace lanlink::desktop

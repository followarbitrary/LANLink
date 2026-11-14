#pragma once

#include <QObject>

#include <functional>
#include <memory>
#include <optional>

#include "lanlink/app/options.hpp"
#include "lanlink/common/result.hpp"
#include "lanlink/discovery/discovery.hpp"
#include "lanlink/platform/clipboard_backend.hpp"
#include "lanlink/runtime/node.hpp"

class QTimer;

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
  void setInputForwardingEnabled(bool enabled);
  void setClipboardSyncEnabled(bool enabled);
  void recoverLocalInput();

 signals:
  void statusChanged(const QString& status, bool running, bool connected);
  void peerChanged(const QString& name, const QString& platform, const QString& address,
                   qulonglong deviceId, qulonglong displayId, bool online);
  void logMessage(const QString& message);
  void errorOccurred(const QString& message);
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
  QString last_status_;
  bool last_status_running_{false};
  bool last_status_connected_{false};
};

}  // namespace lanlink::desktop

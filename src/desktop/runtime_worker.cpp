#include "runtime_worker.hpp"

#include <QTimer>
#include <QDir>
#include <QFileInfo>
#include <QStandardPaths>

#include <cctype>
#include <cmath>
#include <algorithm>
#include <memory>
#include <filesystem>
#include <string>
#include <utility>

#if defined(_WIN32)
#include "lanlink/platform/windows_clipboard_backend.hpp"
#include "lanlink/platform/windows_input_backend.hpp"
#elif defined(__APPLE__)
#include "lanlink/platform/macos_clipboard_backend.hpp"
#include "lanlink/platform/macos_input_backend.hpp"
#endif

namespace lanlink::desktop {
namespace {

bool valid_group(const std::string& group) {
  if (group.empty() || group.size() > 32) return false;
  for (const unsigned char character : group) {
    if (!std::isalnum(character) && character != '-' && character != '_' && character != '.')
      return false;
  }
  return true;
}

QString endpoint_text(const transport::Endpoint& endpoint) {
  return QString::fromStdString(endpoint.host) + QStringLiteral(":") +
         QString::number(endpoint.port);
}

bool device_id_less(const DeviceId& left, const DeviceId& right) {
  return left.high < right.high || (left.high == right.high && left.low < right.low);
}

Result<ScreenTopology> topology_with_revision(const ScreenTopology& source,
                                               std::uint64_t revision) {
  auto displays = source.displays();
  auto links = source.links();
  for (auto& link : links) link.version = revision;
  ScreenTopology updated;
  auto set = updated.set_snapshot(revision, std::move(displays), std::move(links));
  if (!set.ok()) return Result<ScreenTopology>::failure(set.error());
  return Result<ScreenTopology>::success(std::move(updated));
}

}  // namespace

RuntimeWorker::RuntimeWorker(QObject* parent, BackendFactory backend_factory,
                             bool input_forwarding_enabled,
                             ClipboardBackendFactory clipboard_backend_factory)
    : QObject(parent), timer_(new QTimer(this)), backend_factory_(std::move(backend_factory)),
      clipboard_backend_factory_(std::move(clipboard_backend_factory)),
      input_forwarding_enabled_(input_forwarding_enabled) {
  qRegisterMetaType<discovery::TopologySnapshot>("lanlink::discovery::TopologySnapshot");
  if (!backend_factory_) {
    backend_factory_ = [] {
#if defined(_WIN32)
      return std::unique_ptr<platform::IInputBackend>(
          std::make_unique<platform::WindowsInputBackend>());
#elif defined(__APPLE__)
      return std::unique_ptr<platform::IInputBackend>(
          std::make_unique<platform::MacOSInputBackend>());
#else
      return std::unique_ptr<platform::IInputBackend>{};
#endif
    };
  }
  if (!clipboard_backend_factory_) {
    clipboard_backend_factory_ = [] {
#if defined(_WIN32)
      return std::unique_ptr<platform::IClipboardBackend>(
          std::make_unique<platform::WindowsClipboardBackend>());
#elif defined(__APPLE__)
      return std::unique_ptr<platform::IClipboardBackend>(
          std::make_unique<platform::MacOSClipboardBackend>());
#else
      return std::unique_ptr<platform::IClipboardBackend>{};
#endif
    };
  }
  timer_->setInterval(10);
  connect(timer_, &QTimer::timeout, this, &RuntimeWorker::pump);
}

void RuntimeWorker::emit_status(const QString& status, bool running, bool connected) {
  if (status == last_status_ && running == last_status_running_ &&
      connected == last_status_connected_)
    return;
  last_status_ = status;
  last_status_running_ = running;
  last_status_connected_ = connected;
  emit statusChanged(status, running, connected);
}

Result<void> RuntimeWorker::validate_options(const app::AppOptions& options) {
  if (options.device_id == 0) return Result<void>::failure("device id must be non-zero");
  if (options.listen_port == 0) return Result<void>::failure("listen port must be non-zero");
  if (options.discovery_port == 0) return Result<void>::failure("discovery port must be non-zero");
  if (options.peer_id.has_value() && *options.peer_id == 0)
    return Result<void>::failure("peer id must be non-zero");
  if (options.connect.has_value() && !options.peer_id.has_value())
    return Result<void>::failure("peer id is required with an explicit connection");
  if (options.connect.has_value() &&
      (options.connect->host.empty() || options.connect->port == 0))
    return Result<void>::failure("explicit connection endpoint is invalid");
  if (!valid_group(options.group)) return Result<void>::failure("discovery group is invalid");
  if (options.display_id == 0 || options.peer_display_id == 0)
    return Result<void>::failure("display ids must be non-zero");
  const auto valid_dimension = [](double value) {
    return std::isfinite(value) && value > 0.0;
  };
  if ((options.width_explicit && !valid_dimension(options.width)) ||
      (options.height_explicit && !valid_dimension(options.height)))
    return Result<void>::failure("display dimensions must be positive and finite");
  if ((options.peer_width > 0.0 && !valid_dimension(options.peer_width)) ||
      (options.peer_height > 0.0 && !valid_dimension(options.peer_height)))
    return Result<void>::failure("peer dimensions must be positive and finite");
  return Result<void>::success();
}

void RuntimeWorker::start(app::AppOptions options) {
  if (running_) return;
  last_status_.clear();
  last_status_running_ = false;
  last_status_connected_ = false;
  const auto valid = validate_options(options);
  if (!valid.ok()) {
    fail(QString::fromStdString(valid.error()));
    return;
  }
  options_ = std::move(options);

  backend_ = backend_factory_();
  if (!backend_) {
    fail(QStringLiteral("LANLink requires Windows or macOS for native input forwarding"));
    return;
  }
  clipboard_backend_ = clipboard_backend_factory_();

  const auto bounds = backend_->display_bounds();
  if (!bounds.has_value()) {
    fail(QStringLiteral("unable to detect the local display bounds"));
    return;
  }
  options_.left = bounds->left;
  options_.top = bounds->top;
  if (!options_.width_explicit) options_.width = bounds->width();
  if (!options_.height_explicit) options_.height = bounds->height();

  discovery::DiscoveryConfig discovery_config;
  discovery_config.group = options_.group;
  discovery_config.discovery_port = options_.discovery_port;
  discovery_config.device_id = app::numeric_id(options_.device_id);
  discovery_config.display_id = app::numeric_id(options_.display_id);
  discovery_config.tcp_port = options_.listen_port;
  auto discovery_result = discovery::DiscoveryService::bind(discovery_config);
  if (!discovery_result.ok()) {
    fail(QString::fromStdString(discovery_result.error()));
    return;
  }
  discovery_.emplace(std::move(discovery_result.value()));
  running_ = true;
  was_connected_ = false;
  selected_peer_.reset();
  selected_device_id_.reset();
  local_topology_.reset();
  local_topology_revision_ = 0;
  latest_remote_topology_.reset();
  active_file_transfer_.reset();
  topology_selected_ = false;
  outgoing_requested_ = false;
  emit_status(QStringLiteral("正在发现局域网设备"), true, false);
  emit logMessage(QStringLiteral("已监听 %1，发现组 %2:%3")
                      .arg(options_.listen_port)
                      .arg(QString::fromStdString(options_.group))
                      .arg(options_.discovery_port));

  if (options_.connect.has_value()) {
    selected_peer_ = discovery::DiscoveryPeer{app::numeric_id(*options_.peer_id),
                                               app::numeric_id(options_.peer_display_id),
                                               *options_.connect, IClock::Clock::now()};
    selected_device_id_ = selected_peer_->device;
    if (!start_runtime()) return;
    emit_peer_snapshot(false);
  }
  timer_->start();
}

void RuntimeWorker::stop() {
  if (timer_ != nullptr) timer_->stop();
  if (!running_ && !runtime_ && !discovery_.has_value() && !backend_) return;
  stop_runtime();
  if (discovery_.has_value()) {
    const auto stopped = discovery_->stop();
    if (!stopped.ok()) emit logMessage(QString::fromStdString(stopped.error()));
    discovery_.reset();
  }
  backend_.reset();
  clipboard_backend_.reset();
  active_file_transfer_.reset();
  running_ = false;
  selected_peer_.reset();
  selected_device_id_.reset();
  local_topology_.reset();
  local_topology_revision_ = 0;
  latest_remote_topology_.reset();
  topology_selected_ = false;
  outgoing_requested_ = false;
  emit_status(QStringLiteral("已停止"), false, false);
  emit finished();
}

void RuntimeWorker::setRemoteSide(app::RemoteSide side) {
  options_.remote_side = side;
  options_.remote_side_explicit = true;
  if (!runtime_) return;

  auto topology = app::make_topology(options_);
  if (!topology.ok()) {
    emit logMessage(QStringLiteral("拓扑方向更新失败：%1")
                        .arg(QString::fromStdString(topology.error())));
    return;
  }
  const auto next_revision = std::max(local_topology_revision_, local_topology_.has_value()
                                                                  ? local_topology_->version()
                                                                  : std::uint64_t{0}) + 1;
  auto revised = topology_with_revision(topology.value(), next_revision);
  if (!revised.ok()) {
    emit errorOccurred(QString::fromStdString(revised.error()));
    return;
  }
  auto revised_update = runtime_->set_topology(revised.value());
  if (!revised_update.ok()) {
    emit errorOccurred(QString::fromStdString(revised_update.error()));
    return;
  }
  local_topology_ = revised.value();
  local_topology_revision_ = next_revision;
  if (!broadcast_local_topology()) return;
  topology_selected_ = true;
  emit logMessage(side == app::RemoteSide::kLeft ? QStringLiteral("拓扑已更新：对端在左侧")
                                                 : QStringLiteral("拓扑已更新：对端在右侧"));
}

void RuntimeWorker::sendFile(const QString& path) {
  if (path.isEmpty()) return;
  if (!runtime_ || !runtime_->connected()) {
    emit fileTransferChanged(QFileInfo(path).fileName(), 0, 0,
                             QStringLiteral("无在线对端"), true);
    return;
  }
  const QFileInfo info(path);
  if (!info.exists() || !info.isFile()) {
    emit fileTransferChanged(info.fileName(), 0, 0, QStringLiteral("文件不存在"), true);
    return;
  }
  const auto result = runtime_->send_file(
      std::filesystem::path(path.toStdString()), info.fileName().toStdString());
  if (!result.ok()) {
    emit fileTransferChanged(info.fileName(), 0, 0,
                             QString::fromStdString(result.error()), true);
    return;
  }
  active_file_transfer_ = result.value();
}

void RuntimeWorker::cancelFile() {
  if (!runtime_ || !active_file_transfer_.has_value()) return;
  const auto transfer = *active_file_transfer_;
  const auto result = runtime_->cancel_file(transfer);
  if (!result.ok()) {
    emit errorOccurred(QString::fromStdString(result.error()));
    emit fileTransferChanged(QString(), 0, 0,
                             QString::fromStdString(result.error()), true);
  }
  active_file_transfer_.reset();
}

void RuntimeWorker::setInputForwardingEnabled(bool enabled) {
  input_forwarding_enabled_ = enabled;
  if (!runtime_) return;
  const auto updated = runtime_->set_input_forwarding_enabled(enabled, IClock::Clock::now());
  if (!updated.ok()) {
    const auto error = QString::fromStdString(updated.error());
    emit errorOccurred(error);
    emit logMessage(QStringLiteral("输入转发状态更新失败：%1").arg(error));
  }
}

void RuntimeWorker::setClipboardSyncEnabled(bool enabled) {
  options_.clipboard_enabled = enabled;
  if (!runtime_) return;
  const auto updated = runtime_->set_clipboard_sync_enabled(enabled, IClock::Clock::now());
  if (!updated.ok()) {
    const auto error = QString::fromStdString(updated.error());
    options_.clipboard_enabled = !enabled;
    emit errorOccurred(error);
    emit logMessage(QStringLiteral("剪贴板同步状态更新失败：%1").arg(error));
  }
}

void RuntimeWorker::recoverLocalInput() {
  if (!backend_) return;
  const auto recovered = backend_->recover();
  if (!recovered.ok()) {
    emit errorOccurred(QString::fromStdString(recovered.error()));
    emit logMessage(QStringLiteral("本地输入恢复失败：%1").arg(QString::fromStdString(recovered.error())));
    return;
  }
  emit logMessage(QStringLiteral("本地输入已恢复"));
}

void RuntimeWorker::pump() {
  if (!running_ || !discovery_.has_value()) return;
  const auto now = IClock::Clock::now();
  auto discovered = discovery_->poll(now);
  if (!discovered.ok()) {
    emit logMessage(QStringLiteral("发现错误：%1").arg(QString::fromStdString(discovered.error())));
  } else {
    select_peer(discovered.value());
    (void)apply_remote_topology_snapshots();
  }

  if (!runtime_) return;
  auto polled = runtime_->poll(now, std::chrono::milliseconds{0});
  if (!polled.ok())
    emit logMessage(QStringLiteral("运行时错误：%1").arg(QString::fromStdString(polled.error())));
  auto ticked = runtime_->tick(now);
  if (!ticked.ok())
    emit logMessage(QStringLiteral("运行时错误：%1").arg(QString::fromStdString(ticked.error())));

  const bool connected = runtime_->connected();
  if (connected != was_connected_) {
    was_connected_ = connected;
    emit_status(connected ? QStringLiteral("已连接") : QStringLiteral("等待连接"), true, connected);
    emit logMessage(connected ? QStringLiteral("LANLink 已连接") : QStringLiteral("LANLink 已断开"));
  }
  if (selected_peer_.has_value()) emit_peer_snapshot(connected);
}

bool RuntimeWorker::local_id_less(const DeviceId& peer) const {
  return peer.high == 0 && options_.device_id < peer.low;
}

bool RuntimeWorker::broadcast_local_topology() {
  if (!discovery_.has_value() || !local_topology_.has_value()) return true;
  const auto result = discovery_->broadcast_topology(
      *local_topology_, local_topology_revision_);
  if (!result.ok()) {
    emit logMessage(QStringLiteral("拓扑广播失败：%1")
                        .arg(QString::fromStdString(result.error())));
    return false;
  }
  return true;
}

bool RuntimeWorker::apply_remote_topology_snapshots() {
  if (!runtime_ || !discovery_.has_value()) return false;
  const discovery::TopologySnapshot* newest = nullptr;
  for (const auto& snapshot : discovery_->topology_snapshots()) {
    if (snapshot.origin == app::numeric_id(options_.device_id)) continue;
    const std::pair<std::uint64_t, DeviceId> key{snapshot.revision, snapshot.origin};
    if (latest_remote_topology_.has_value() &&
        !(latest_remote_topology_.value().first < key.first ||
          (latest_remote_topology_.value().first == key.first &&
           device_id_less(latest_remote_topology_.value().second, key.second)))) {
      continue;
    }
    if (newest == nullptr || newest->revision < snapshot.revision ||
        (newest->revision == snapshot.revision && device_id_less(newest->origin, snapshot.origin))) {
      newest = &snapshot;
    }
  }
  if (newest == nullptr) return false;
  const auto updated = runtime_->set_topology(newest->topology);
  if (!updated.ok()) {
    emit logMessage(QStringLiteral("远端拓扑应用失败：%1")
                        .arg(QString::fromStdString(updated.error())));
    return false;
  }
  latest_remote_topology_ = std::pair<std::uint64_t, DeviceId>{newest->revision, newest->origin};
  local_topology_revision_ = std::max(local_topology_revision_, newest->revision);
  emit remoteTopologyChanged(*newest);
  return true;
}

bool RuntimeWorker::select_peer(const std::vector<discovery::DiscoveryPeer>& peers) {
  const auto selected = app::select_discovery_peer(peers, options_.peer_id);
  if (!selected.has_value()) {
    if (!runtime_) emit_status(QStringLiteral("正在等待对端设备"), true, false);
    if (selected_peer_.has_value()) emit_peer_snapshot(false);
    return false;
  }

  if (selected_device_id_.has_value() && selected->device != *selected_device_id_)
    return false;

  selected_peer_ = *selected;
  if (!selected_device_id_.has_value()) selected_device_id_ = selected->device;
  if (!options_.peer_id.has_value()) options_.peer_id = selected->device.low;
  if (!options_.peer_display_id_explicit) options_.peer_display_id = selected->display.low;
  if (!options_.remote_side_explicit)
    options_.remote_side = local_id_less(selected->device) ? app::RemoteSide::kRight
                                                            : app::RemoteSide::kLeft;
  if (!runtime_) {
    if (!start_runtime()) return false;
  } else if (!topology_selected_) {
    if (!update_topology_from_peer(*selected)) return false;
    topology_selected_ = true;
    (void)runtime_->set_accept_incoming(true);
  }

  if (runtime_ && !outgoing_requested_) {
    if (local_id_less(selected->device)) {
      auto connected = runtime_->connect_peer(selected->endpoint);
      if (!connected.ok()) {
        emit logMessage(QStringLiteral("连接错误：%1").arg(QString::fromStdString(connected.error())));
      } else {
        outgoing_requested_ = true;
        emit_status(QStringLiteral("正在连接 %1").arg(endpoint_text(selected->endpoint)), true, false);
      }
    } else {
      outgoing_requested_ = true;
    }
  } else if (runtime_ && local_id_less(selected->device)) {
    (void)runtime_->update_peer_endpoint(selected->endpoint);
  }
  emit_peer_snapshot(runtime_ && runtime_->connected());
  return true;
}

bool RuntimeWorker::update_topology_from_peer(const discovery::DiscoveryPeer&) {
  auto topology = app::make_topology(options_);
  if (!topology.ok()) {
    fail(QString::fromStdString(topology.error()));
    return false;
  }
  const auto next_revision = std::max(local_topology_revision_, local_topology_.has_value()
                                                                  ? local_topology_->version()
                                                                  : std::uint64_t{0}) + 1;
  auto revised = topology_with_revision(topology.value(), next_revision);
  if (!revised.ok()) {
    fail(QString::fromStdString(revised.error()));
    return false;
  }
  const auto updated = runtime_->set_topology(revised.value());
  if (!updated.ok()) {
    fail(QString::fromStdString(updated.error()));
    return false;
  }
  local_topology_ = revised.value();
  local_topology_revision_ = next_revision;
  if (!broadcast_local_topology()) return false;
  return true;
}

bool RuntimeWorker::start_runtime() {
  if (runtime_) return true;
  if (!options_.peer_id.has_value()) return false;
  auto topology = app::make_topology(options_);
  if (!topology.ok()) {
    fail(QString::fromStdString(topology.error()));
    return false;
  }

  runtime::InputRuntimeConfig config;
  config.local_device = app::numeric_id(options_.device_id);
  config.source = app::numeric_id(options_.device_id);
  config.peer = AuthenticatedPeer{app::numeric_id(*options_.peer_id)};
  config.local_display = app::numeric_id(options_.display_id);
  config.listen_endpoint = transport::Endpoint{"0.0.0.0", options_.listen_port};
  config.backend = backend_.get();
  config.clipboard_backend = clipboard_backend_.get();
  config.clipboard_sync_enabled = options_.clipboard_enabled;
  config.queue_capacity = 256;
  config.accept_incoming = options_.connect.has_value() || selected_peer_.has_value();
  auto receive_directory = QStandardPaths::writableLocation(QStandardPaths::DownloadLocation);
  if (receive_directory.isEmpty()) receive_directory = QDir::tempPath();
  config.receive_directory = std::filesystem::path(receive_directory.toStdString());
  config.file_status_callback = [this](const transport::FileTransferStatus& status) {
    const auto state = [&status] {
      switch (status.state) {
      case protocol::FileStatusCode::kAccepted: return QStringLiteral("已接受");
      case protocol::FileStatusCode::kProgress: return QStringLiteral("传输中");
      case protocol::FileStatusCode::kCompleted: return QStringLiteral("已完成");
      case protocol::FileStatusCode::kFailed: return QStringLiteral("失败");
      case protocol::FileStatusCode::kCanceled: return QStringLiteral("已取消");
      }
      return QStringLiteral("未知状态");
    }();
    const auto message = status.message.empty()
                             ? state
                             : state + QStringLiteral("：") +
                                   QString::fromStdString(status.message);
    emit fileTransferChanged(QString::fromStdString(status.name),
                             static_cast<qulonglong>(status.bytes),
                             static_cast<qulonglong>(status.total_size), message,
                             !status.incoming);
    if (status.state == protocol::FileStatusCode::kCompleted ||
        status.state == protocol::FileStatusCode::kFailed ||
        status.state == protocol::FileStatusCode::kCanceled) {
      if (active_file_transfer_.has_value() && active_file_transfer_.value() == status.transfer)
        active_file_transfer_.reset();
    } else {
      active_file_transfer_ = status.transfer;
    }
  };
  auto created = runtime::InputNodeRuntime::create(std::move(config));
  if (!created.ok()) {
    fail(QString::fromStdString(created.error()));
    return false;
  }
  runtime_ = std::move(created.value());
  auto set_topology = runtime_->set_topology(std::move(topology.value()));
  if (!set_topology.ok()) {
    fail(QString::fromStdString(set_topology.error()));
    return false;
  }
  local_topology_ = runtime_->topology();
  local_topology_revision_ = local_topology_->version();
  auto started = runtime_->start();
  if (!started.ok()) {
    fail(QString::fromStdString(started.error()));
    return false;
  }
  auto forwarding = runtime_->set_input_forwarding_enabled(input_forwarding_enabled_,
                                                            IClock::Clock::now());
  if (!forwarding.ok()) {
    fail(QString::fromStdString(forwarding.error()));
    return false;
  }
  topology_selected_ = true;
  if (!broadcast_local_topology()) return false;
  emit_status(QStringLiteral("正在监听 %1").arg(options_.listen_port), true, false);

  if (options_.connect.has_value()) {
    auto connected = runtime_->connect_peer(*options_.connect);
    if (!connected.ok()) {
      fail(QString::fromStdString(connected.error()));
      return false;
    }
    outgoing_requested_ = true;
    emit_status(QStringLiteral("正在连接 %1").arg(endpoint_text(*options_.connect)), true, false);
  }
  return true;
}

void RuntimeWorker::emit_peer_snapshot(bool online) {
  if (!selected_peer_.has_value()) return;
  const auto& peer = *selected_peer_;
  emit peerChanged(QStringLiteral("设备 %1").arg(peer.device.low), QStringLiteral("LANLink 节点"),
                   endpoint_text(peer.endpoint), peer.device.low, peer.display.low, online);
}

void RuntimeWorker::stop_runtime() {
  if (!runtime_) return;
  const auto stopped = runtime_->stop(IClock::Clock::now());
  if (!stopped.ok())
    emit logMessage(QStringLiteral("运行时停止错误：%1").arg(QString::fromStdString(stopped.error())));
  runtime_.reset();
}

void RuntimeWorker::fail(const QString& message) {
  emit errorOccurred(message);
  emit logMessage(QStringLiteral("错误：%1").arg(message));
  if (timer_ != nullptr) timer_->stop();
  stop_runtime();
  if (discovery_.has_value()) {
    (void)discovery_->stop();
    discovery_.reset();
  }
  backend_.reset();
  clipboard_backend_.reset();
  running_ = false;
  local_topology_.reset();
  local_topology_revision_ = 0;
  latest_remote_topology_.reset();
  emit_status(QStringLiteral("错误"), false, false);
  emit finished();
}

}  // namespace lanlink::desktop

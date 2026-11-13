#include "runtime_worker.hpp"

#include <QTimer>

#include <cctype>
#include <cmath>
#include <memory>
#include <string>
#include <utility>

#if defined(_WIN32)
#include "lanlink/platform/windows_input_backend.hpp"
#elif defined(__APPLE__)
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

}  // namespace

RuntimeWorker::RuntimeWorker(QObject* parent, BackendFactory backend_factory)
    : QObject(parent), timer_(new QTimer(this)), backend_factory_(std::move(backend_factory)) {
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
  running_ = false;
  selected_peer_.reset();
  selected_device_id_.reset();
  topology_selected_ = false;
  outgoing_requested_ = false;
  emit_status(QStringLiteral("已停止"), false, false);
  emit finished();
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
  const auto updated = runtime_->set_topology(std::move(topology.value()));
  if (!updated.ok()) {
    fail(QString::fromStdString(updated.error()));
    return false;
  }
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
  config.queue_capacity = 256;
  config.accept_incoming = options_.connect.has_value() || selected_peer_.has_value();
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
  auto started = runtime_->start();
  if (!started.ok()) {
    fail(QString::fromStdString(started.error()));
    return false;
  }
  topology_selected_ = true;
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
  running_ = false;
  emit_status(QStringLiteral("错误"), false, false);
  emit finished();
}

}  // namespace lanlink::desktop

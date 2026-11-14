#include "pages.hpp"

#include "topology_canvas.hpp"
#include "icons.hpp"

#include <QFileDialog>
#include <QFileInfo>
#include <QFrame>
#include <QCheckBox>
#include <QAbstractButton>
#include <QButtonGroup>
#include <QComboBox>
#include <QDragEnterEvent>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QMimeData>
#include <QLineEdit>
#include <QPushButton>
#include <QRadioButton>
#include <QScrollArea>
#include <QSizePolicy>
#include <QSpacerItem>
#include <QToolButton>
#include <QVBoxLayout>

#include <functional>

namespace lanlink::desktop {
namespace {

const QString kPanelStyle = QStringLiteral(
    "QWidget#page { background: #1e2935; }"
    "QFrame#panel { background: #172330; border: 1px solid #1c2b3a; border-radius: 14px; }"
    "QFrame#subpanel { background: #141e2a; border: 1px solid #1c2b3a; border-radius: 14px; }"
    "QLabel { color: #edf2f8; background: transparent; }"
    "QLabel#muted { color: #92a7bd; }"
    "QLabel#section { color: #eff4fa; font-size: 21px; font-weight: 600; }"
    "QPushButton { color: #dce6f2; background: #223347; border: 1px solid #3c526c; border-radius: 9px; padding: 9px 16px; }"
    "QPushButton:hover { background: #2b425b; }"
    "QPushButton#primary { color: #14202c; background: #f2f4f7; border: 0; border-radius: 22px; padding: 10px 18px; min-height: 42px; font-weight: 600; }"
    "QPushButton#secondary { color: #dce6f2; background: #1c3045; border: 1px solid #4b637d; border-radius: 22px; padding: 10px 18px; min-height: 42px; }"
    "QRadioButton, QCheckBox { color: #e7edf5; font-size: 17px; spacing: 10px; }"
    "QToolButton#settingToggle { color: #172330; background: #f2f4f7; border: 0; border-radius: 14px; font-size: 17px; font-weight: 700; min-width: 28px; max-width: 28px; min-height: 28px; max-height: 28px; }"
    "QToolButton#settingToggle:unchecked { color: #91a3b8; background: #223347; border: 1px solid #51647b; }"
    "QRadioButton::indicator, QCheckBox::indicator { width: 20px; height: 20px; }"
    "QCheckBox::indicator { border: 2px solid #7c8da2; border-radius: 10px; background: #202b39; }"
    "QCheckBox::indicator:checked { background: #f2f4f7; border-color: #f2f4f7; }"
    "QCheckBox::indicator:checked:after { color: #172330; }"
    "QRadioButton::indicator { border: 2px solid #647388; border-radius: 10px; background: #202b39; }"
    "QRadioButton::indicator:checked { border: 6px solid #f2f4f7; background: #172330; }"
    "QLineEdit { color: #e7edf5; background: #202b39; border: 1px solid #3e5268; border-radius: 8px; padding: 8px 10px; }"
);

QLabel* label(const QString& text, const QString& objectName = {}) {
  auto* result = new QLabel(text);
  if (!objectName.isEmpty()) result->setObjectName(objectName);
  return result;
}

QWidget* pageBase(const QString& title, const QString& subtitle, QWidget* parent) {
  auto* page = new QWidget(parent);
  page->setObjectName(QStringLiteral("page"));
  page->setAttribute(Qt::WA_StyledBackground, true);
  page->setStyleSheet(kPanelStyle);
  auto* layout = new QVBoxLayout(page);
  layout->setContentsMargins(24, 22, 24, 24);
  layout->setSpacing(14);

  auto* heading = label(title);
  heading->setStyleSheet(QStringLiteral("font-size: 30px; font-weight: 700;"));
  layout->addWidget(heading);
  auto* sub = label(subtitle, QStringLiteral("muted"));
  sub->setStyleSheet(QStringLiteral("font-size: 16px;"));
  layout->addWidget(sub);
  return page;
}

QVBoxLayout* pageLayout(QWidget* page) { return qobject_cast<QVBoxLayout*>(page->layout()); }

QFrame* panel(const QString& objectName = QStringLiteral("panel")) {
  auto* frame = new QFrame;
  frame->setObjectName(objectName);
  return frame;
}

QLabel* onlineBadge() {
  auto* badge = label(QStringLiteral("在线"));
  badge->setAlignment(Qt::AlignCenter);
  badge->setMinimumWidth(58);
  badge->setStyleSheet(QStringLiteral(
      "background: #174d3c; color: #53d39b; border-radius: 16px; padding: 7px 12px; font-size: 15px;"));
  return badge;
}

QWidget* titleRow(const QString& title, const QString& actionText = {}) {
  auto* row = new QWidget;
  auto* layout = new QHBoxLayout(row);
  layout->setContentsMargins(0, 0, 0, 0);
  auto* section = label(title, QStringLiteral("section"));
  layout->addWidget(section);
  layout->addStretch();
  if (!actionText.isEmpty()) {
    auto* action = label(actionText);
    action->setStyleSheet(QStringLiteral("color: #8fc3ff; font-size: 15px;"));
    layout->addWidget(action);
  }
  return row;
}

QFrame* deviceCard(const DeviceState& device, bool selected) {
  auto* card = panel();
  card->setObjectName(QStringLiteral("deviceCard"));
  card->setMinimumHeight(146);
  card->setStyleSheet(QStringLiteral(
      "QFrame#deviceCard { background: %1; border: 1px solid %2; border-radius: 14px; }")
                          .arg(selected ? QStringLiteral("#2c425b") : QStringLiteral("#172330"))
                          .arg(selected ? QStringLiteral("#73b4fb") : QStringLiteral("#33485f")));
  auto* layout = new QVBoxLayout(card);
  layout->setContentsMargins(16, 14, 16, 14);
  auto* top = new QHBoxLayout;
  auto* icon = new QLabel;
  icon->setFixedSize(52, 52);
  icon->setAlignment(Qt::AlignCenter);
  icon->setPixmap(iconPixmap(device.local ? IconKind::Monitor : IconKind::Laptop, QSize(28, 28), device.accent));
  icon->setStyleSheet(QStringLiteral("border: 2px solid %1; border-radius: 26px; background: transparent;")
                          .arg(device.accent.name()));
  top->addWidget(icon);
  top->addStretch();
  auto* check = label(device.online ? QStringLiteral("✓") : QStringLiteral("–"));
  check->setFixedSize(28, 28);
  check->setAlignment(Qt::AlignCenter);
  check->setStyleSheet(QStringLiteral(
      "color: %1; border: 1.5px solid %1; border-radius: 14px; font-size: 15px;")
                          .arg(device.online ? QStringLiteral("#63d7a5") : QStringLiteral("#7c8da2")));
  top->addWidget(check);
  layout->addLayout(top);
  auto* name = label(device.name);
  name->setStyleSheet(QStringLiteral("font-size: 17px; font-weight: 600;"));
  layout->addWidget(name);
  layout->addWidget(label(device.address, QStringLiteral("muted")));
  return card;
}

QWidget* connectedDevices(const DesktopState& state) {
  auto* wrapper = new QWidget;
  auto* layout = new QVBoxLayout(wrapper);
  layout->setContentsMargins(0, 0, 0, 0);
  layout->setSpacing(10);
  layout->addWidget(titleRow(QStringLiteral("已连接设备"), QStringLiteral("自动发现开启")));
  auto* row = new QHBoxLayout;
  row->setSpacing(12);
  for (int index = 0; index < state.devices.size(); ++index) {
    auto* card = deviceCard(state.devices[index], index == state.activeDevice);
    row->addWidget(card, 1);
  }
  layout->addLayout(row);
  return wrapper;
}

QWidget* activeDevicePanel(const DesktopState& state) {
  const auto& device = state.devices[state.activeDevice];
  auto* frame = panel(QStringLiteral("subpanel"));
  auto* layout = new QVBoxLayout(frame);
  layout->setContentsMargins(18, 15, 18, 15);
  auto* title = label(device.name);
  title->setStyleSheet(QStringLiteral("font-size: 21px; font-weight: 600;"));
  layout->addWidget(title);
  auto* meta = label(device.platform + QStringLiteral(" · ") +
                        (device.online ? QStringLiteral("已连接") : QStringLiteral("等待连接")),
                    QStringLiteral("muted"));
  meta->setStyleSheet(QStringLiteral("font-size: 16px;"));
  layout->addWidget(meta);

  auto* actions = new QHBoxLayout;
  auto* control = new QPushButton(QStringLiteral("输入转发由运行时控制"));
  control->setObjectName(QStringLiteral("primary"));
  control->setEnabled(false);
  control->setIcon(icon(IconKind::Pointer, QColor(QStringLiteral("#172330"))));
  control->setIconSize(QSize(18, 18));
  auto* send = new QPushButton(QStringLiteral("文件传输未接入"));
  send->setObjectName(QStringLiteral("secondary"));
  send->setEnabled(false);
  send->setIcon(icon(IconKind::Upload, QColor(QStringLiteral("#dce6f2"))));
  send->setIconSize(QSize(18, 18));
  actions->addWidget(control);
  actions->addWidget(send);
  actions->addStretch();
  if (device.online) {
    actions->addWidget(onlineBadge());
  } else {
    auto* offline = label(QStringLiteral("未运行"));
    offline->setStyleSheet(QStringLiteral(
        "color: #f2c77d; background: #5a4321; border-radius: 16px; padding: 7px 12px; font-size: 15px;"));
    actions->addWidget(offline);
  }
  layout->addLayout(actions);
  layout->addWidget(label(QStringLiteral("运行后，指针越过配置边缘即可触发跨屏输入转发。"), QStringLiteral("muted")));
  return frame;
}

QWidget* topologySection(DesktopState* state, bool layoutMode) {
  auto* wrapper = panel(QStringLiteral("subpanel"));
  auto* layout = new QVBoxLayout(wrapper);
  layout->setContentsMargins(20, 16, 20, 18);
  auto* header = new QHBoxLayout;
  header->addWidget(label(QStringLiteral("设备拓扑"), QStringLiteral("section")));
  header->addStretch();
  auto* horizontal = new QPushButton(QStringLiteral("横向"));
  horizontal->setCheckable(true);
  horizontal->setChecked(true);
  horizontal->setStyleSheet(QStringLiteral("background: #252a31; border: 0; border-radius: 16px; padding: 7px 15px;"));
  auto* lShape = new QPushButton(QStringLiteral("L 型"));
  lShape->setCheckable(true);
  lShape->setFlat(true);
  auto* reset = new QPushButton(QStringLiteral("重置布局"));
  reset->setFlat(true);
  header->addWidget(horizontal);
  header->addWidget(lShape);
  header->addSpacing(8);
  header->addWidget(reset);
  layout->addLayout(header);
  auto* canvas = new TopologyCanvas;
  canvas->setState(state);
  canvas->setLayoutMode(layoutMode);
  horizontal->setChecked(layoutMode);
  lShape->setChecked(!layoutMode);
  QObject::connect(horizontal, &QPushButton::clicked, canvas, [canvas, horizontal, lShape]() {
    horizontal->setChecked(true);
    lShape->setChecked(false);
    canvas->setLayoutMode(true);
  });
  QObject::connect(lShape, &QPushButton::clicked, canvas, [canvas, horizontal, lShape]() {
    horizontal->setChecked(false);
    lShape->setChecked(true);
    canvas->setLayoutMode(false);
  });
  QObject::connect(reset, &QPushButton::clicked, canvas, &TopologyCanvas::resetLayout);
  layout->addWidget(canvas);
  layout->addWidget(label(QStringLiteral("示意布局 · 支持拖动屏幕卡片，滚轮缩放，中键平移"), QStringLiteral("muted")), 0,
                    Qt::AlignCenter);
  return wrapper;
}

class DropZone final : public QFrame {
  Q_OBJECT

 public:
  explicit DropZone(QWidget* parent = nullptr) : QFrame(parent) {
    setAcceptDrops(true);
    setMinimumHeight(220);
    setObjectName(QStringLiteral("dropZone"));
    setStyleSheet(QStringLiteral(
        "QFrame#dropZone { background: #172330; border: 2px dashed #4d6a87; border-radius: 16px; }"));
    auto* layout = new QVBoxLayout(this);
    layout->setAlignment(Qt::AlignCenter);
    auto* icon = new QLabel;
    icon->setPixmap(iconPixmap(IconKind::ArrowUp, QSize(42, 42), QColor(QStringLiteral("#8fc3ff"))));
    icon->setAlignment(Qt::AlignCenter);
    layout->addWidget(icon);
    auto* title = label(QStringLiteral("文件发送区域"));
    title->setStyleSheet(QStringLiteral("font-size: 23px; font-weight: 600;"));
    title->setAlignment(Qt::AlignCenter);
    layout->addWidget(title);
    auto* hint = label(QStringLiteral("正式应用支持拖放文件或点击浏览"), QStringLiteral("muted"));
    hint->setAlignment(Qt::AlignCenter);
    layout->addWidget(hint);
  }

 signals:
  void fileDropped(const QString& path);

 protected:
  void dragEnterEvent(QDragEnterEvent* event) override {
    if (event->mimeData()->hasUrls()) event->acceptProposedAction();
  }
  void dropEvent(QDropEvent* event) override {
    const auto urls = event->mimeData()->urls();
    if (!urls.isEmpty()) emit fileDropped(urls.first().toLocalFile());
    event->acceptProposedAction();
  }
};

class TargetOption final : public QAbstractButton {
 public:
  explicit TargetOption(const QString& text, QWidget* parent = nullptr) : QAbstractButton(parent) {
    setText(text);
    setCheckable(true);
    setCursor(Qt::PointingHandCursor);
    setMinimumHeight(42);
  }

  QSize sizeHint() const override { return QSize(280, 42); }

 protected:
  void paintEvent(QPaintEvent*) override {
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);
    const QPointF center(13.0, height() / 2.0);
    painter.setPen(QPen(isChecked() ? QColor(QStringLiteral("#f2f4f7")) : QColor(QStringLiteral("#68798e")), 2.0));
    painter.setBrush(isChecked() ? QColor(QStringLiteral("#f2f4f7")) : Qt::NoBrush);
    painter.drawEllipse(center, 10.0, 10.0);
    if (isChecked()) {
      painter.setPen(Qt::NoPen);
      painter.setBrush(QColor(QStringLiteral("#172330")));
      painter.drawEllipse(center, 3.5, 3.5);
    }
    painter.setPen(QColor(QStringLiteral("#e7edf5")));
    QFont optionFont = painter.font();
    optionFont.setPointSize(16);
    painter.setFont(optionFont);
    painter.drawText(QRectF(34.0, 0.0, width() - 34.0, height()), Qt::AlignVCenter | Qt::AlignLeft, text());
  }
};

}  // namespace

QWidget* buildOverviewPage(DesktopState* state, QWidget* parent) {
  auto* page = pageBase(QStringLiteral("设备概览"), QStringLiteral("你的局域网设备已准备就绪"), parent);
  auto* layout = pageLayout(page);
  layout->addWidget(topologySection(state, false));
  layout->addWidget(connectedDevices(*state));
  layout->addWidget(activeDevicePanel(*state));
  layout->addStretch();
  return page;
}

QWidget* buildLayoutPage(DesktopState* state, QWidget* parent) {
  auto* page = pageBase(QStringLiteral("屏幕布局"), QStringLiteral("设置电脑之间的空间位置与跨屏方向"), parent);
  auto* layout = pageLayout(page);
  layout->addWidget(topologySection(state, true));
  layout->addWidget(connectedDevices(*state));
  layout->addWidget(activeDevicePanel(*state));
  layout->addStretch();
  return page;
}

QWidget* buildDevicesPage(const DesktopState& state, QWidget* parent) {
  auto* page = pageBase(QStringLiteral("设备管理"), QStringLiteral("查看身份、权限以及配对状态。"), parent);
  auto* layout = pageLayout(page);
  for (const auto& device : state.devices) {
    auto* row = panel(QStringLiteral("subpanel"));
    row->setMinimumHeight(112);
    auto* rowLayout = new QHBoxLayout(row);
    rowLayout->setContentsMargins(24, 14, 22, 14);
    auto* icon = new QLabel;
    icon->setFixedSize(40, 40);
    icon->setAlignment(Qt::AlignCenter);
    icon->setPixmap(iconPixmap(device.local ? IconKind::Monitor : IconKind::Laptop, QSize(28, 28), device.accent));
    rowLayout->addWidget(icon);
    auto* details = new QVBoxLayout;
    auto* name = label(device.name);
    name->setStyleSheet(QStringLiteral("font-size: 23px; font-weight: 600;"));
    details->addWidget(name);
    details->addWidget(label(device.platform + QStringLiteral(" · ") + device.address, QStringLiteral("muted")));
    rowLayout->addLayout(details);
    rowLayout->addStretch();
    if (device.online) {
      rowLayout->addWidget(onlineBadge());
    } else {
      auto* offline = label(QStringLiteral("离线"));
      offline->setStyleSheet(QStringLiteral(
          "color: #f2c77d; background: #5a4321; border-radius: 16px; padding: 7px 12px; font-size: 15px;"));
      rowLayout->addWidget(offline);
    }
    layout->addWidget(row);
  }
  layout->addStretch();
  return page;
}

QWidget* buildTransferPage(DesktopState* state, QWidget* parent) {
  auto* page = pageBase(QStringLiteral("文件传输（未接入）"),
                        QStringLiteral("当前 LANLink 核心只提供键鼠跨屏转发，文件协议尚未实现。"), parent);
  auto* layout = pageLayout(page);
  auto* zone = new DropZone;
  zone->setEnabled(false);
  layout->addWidget(zone);
  layout->addWidget(label(QStringLiteral("目标设备"), QStringLiteral("section")));
  auto* targetBox = new QVBoxLayout;
  if (state->devices.isEmpty()) {
    layout->addWidget(label(QStringLiteral("暂无可用目标设备"), QStringLiteral("muted")));
    layout->addStretch();
    return page;
  }
  auto* group = new QButtonGroup(page);
  group->setExclusive(true);
  for (int index = 1; index < state->devices.size(); ++index) {
    auto* radio = new TargetOption(state->devices[index].name);
    group->addButton(radio);
    radio->setChecked(index == state->selectedTransferTarget);
    QObject::connect(radio, &QAbstractButton::toggled, page, [state, index](bool checked) {
      if (checked) state->selectedTransferTarget = index;
    });
    targetBox->addWidget(radio);
  }
  auto* local = new TargetOption(state->devices[0].name + QStringLiteral("（本机）"));
  group->addButton(local);
  local->setChecked(state->selectedTransferTarget == 0);
  QObject::connect(local, &QAbstractButton::toggled, page, [state](bool checked) {
    if (checked) state->selectedTransferTarget = 0;
  });
  targetBox->addWidget(local);
  layout->addLayout(targetBox);

  auto* browse = new QPushButton(QStringLiteral("选择文件"));
  browse->setEnabled(false);
  auto* status = label(QStringLiteral("传输进度、暂停、续传和接收记录将在这里显示。"), QStringLiteral("muted"));
  layout->addWidget(browse, 0, Qt::AlignLeft);
  layout->addWidget(status);
  auto chooseFile = [state, status, page]() {
    const QString path = QFileDialog::getOpenFileName(page, QStringLiteral("选择要发送的文件"));
    if (path.isEmpty()) return;
    state->droppedFile = path;
    status->setText(QStringLiteral("已选择：%1").arg(QFileInfo(path).fileName()));
  };
  QObject::connect(browse, &QPushButton::clicked, page, chooseFile);
  QObject::connect(zone, &DropZone::fileDropped, page, [state, status](const QString& path) {
    if (path.isEmpty()) return;
    state->droppedFile = path;
    status->setText(QStringLiteral("已选择：%1").arg(QFileInfo(path).fileName()));
  });
  layout->addStretch();
  return page;
}

QWidget* buildSettingsPage(DesktopState* state, QWidget* parent,
                           StartRuntimeCallback startRuntime,
                           StopRuntimeCallback stopRuntime,
                           TogglePreferenceCallback toggleCrossScreen) {
  auto* page = pageBase(QStringLiteral("连接设置"),
                        QStringLiteral("配置本机身份、发现参数和对端连接方式。"), parent);
  auto* layout = pageLayout(page);

  auto* connection = panel(QStringLiteral("subpanel"));
  auto* form = new QFormLayout(connection);
  form->setContentsMargins(18, 16, 18, 16);
  form->setHorizontalSpacing(18);
  form->setVerticalSpacing(10);
  const auto addField = [form](const QString& title, const QString& value,
                               const QString& objectName) {
    auto* edit = new QLineEdit(value);
    edit->setObjectName(objectName);
    edit->setClearButtonEnabled(true);
    form->addRow(label(title), edit);
    return edit;
  };
  auto* deviceId = addField(QStringLiteral("设备 ID"), QString::number(state->options.device_id),
                             QStringLiteral("deviceIdEdit"));
  auto* peerId = addField(QStringLiteral("对端 ID（可选）"),
                          state->options.peer_id.has_value()
                              ? QString::number(*state->options.peer_id)
                              : QString(),
                          QStringLiteral("peerIdEdit"));
  auto* listenPort = addField(QStringLiteral("监听端口"), QString::number(state->options.listen_port),
                              QStringLiteral("listenPortEdit"));
  auto* group = addField(QStringLiteral("发现组"), QString::fromStdString(state->options.group),
                         QStringLiteral("groupEdit"));
  auto* discoveryPort = addField(QStringLiteral("发现端口"),
                                 QString::number(state->options.discovery_port),
                                 QStringLiteral("discoveryPortEdit"));
  auto* connectHost = addField(QStringLiteral("手动连接主机（可选）"),
                               state->options.connect.has_value()
                                   ? QString::fromStdString(state->options.connect->host)
                                   : QString(),
                               QStringLiteral("connectHostEdit"));
  auto* connectPort = addField(QStringLiteral("手动连接端口"),
                               state->options.connect.has_value()
                                   ? QString::number(state->options.connect->port)
                                   : QStringLiteral("41000"),
                               QStringLiteral("connectPortEdit"));
  auto* displayId = addField(QStringLiteral("本机显示器 ID"),
                             QString::number(state->options.display_id),
                             QStringLiteral("displayIdEdit"));
  auto* peerDisplayId = addField(QStringLiteral("对端显示器 ID"),
                                 QString::number(state->options.peer_display_id),
                                 QStringLiteral("peerDisplayIdEdit"));
  auto* remoteSide = new QComboBox;
  remoteSide->setObjectName(QStringLiteral("remoteSideCombo"));
  remoteSide->addItem(QStringLiteral("对端在右侧"), QStringLiteral("right"));
  remoteSide->addItem(QStringLiteral("对端在左侧"), QStringLiteral("left"));
  remoteSide->setCurrentIndex(state->options.remote_side == app::RemoteSide::kLeft ? 1 : 0);
  form->addRow(label(QStringLiteral("对端方向")), remoteSide);
  auto* autoTopology = new QCheckBox(QStringLiteral("自动使用发现的显示器和方向"));
  autoTopology->setObjectName(QStringLiteral("autoTopologyCheck"));
  autoTopology->setChecked(!state->options.peer_display_id_explicit &&
                           !state->options.remote_side_explicit);
  form->addRow(QString(), autoTopology);
  layout->addWidget(connection);

  auto* actionRow = new QHBoxLayout;
  auto* toggle = new QPushButton;
  toggle->setObjectName(QStringLiteral("runtimeToggle"));
  toggle->setProperty("class", QStringLiteral("primary"));
  toggle->setText(state->running ? QStringLiteral("停止服务") : QStringLiteral("启动服务"));
  auto* status = label(state->status, QStringLiteral("muted"));
  status->setObjectName(QStringLiteral("runtimeFormStatus"));
  actionRow->addWidget(toggle);
  actionRow->addWidget(status);
  actionRow->addStretch();
  layout->addLayout(actionRow);

  QObject::connect(toggle, &QPushButton::clicked, page,
                   [state, startRuntime, stopRuntime, deviceId, peerId, listenPort, group,
                    discoveryPort, connectHost, connectPort, displayId, peerDisplayId,
                    remoteSide, autoTopology, status]() {
    if (state->running) {
      if (stopRuntime) stopRuntime();
      return;
    }
    app::AppOptions options = state->options;
    auto parse = [status](QLineEdit* edit, const QString& name, std::uint64_t& target) {
      bool ok = false;
      const auto value = edit->text().trimmed().toULongLong(&ok);
      if (!ok || value == 0) {
        status->setText(QStringLiteral("%1 必须是正整数").arg(name));
        return false;
      }
      target = value;
      return true;
    };
    if (!parse(deviceId, QStringLiteral("设备 ID"), options.device_id)) return;
    std::uint64_t listen = 0;
    if (!parse(listenPort, QStringLiteral("监听端口"), listen) || listen > 65535) {
      status->setText(QStringLiteral("监听端口必须在 1..65535"));
      return;
    }
    options.listen_port = static_cast<std::uint16_t>(listen);
    std::uint64_t discovery = 0;
    if (!parse(discoveryPort, QStringLiteral("发现端口"), discovery) || discovery > 65535) {
      status->setText(QStringLiteral("发现端口必须在 1..65535"));
      return;
    }
    options.discovery_port = static_cast<std::uint16_t>(discovery);
    if (!parse(displayId, QStringLiteral("本机显示器 ID"), options.display_id)) return;
    if (!parse(peerDisplayId, QStringLiteral("对端显示器 ID"), options.peer_display_id)) return;
    options.group = group->text().trimmed().toStdString();
    if (options.group.empty()) {
      status->setText(QStringLiteral("发现组不能为空"));
      return;
    }
    const auto peerText = peerId->text().trimmed();
    if (peerText.isEmpty()) {
      options.peer_id.reset();
    } else {
      std::uint64_t peer = 0;
      if (!parse(peerId, QStringLiteral("对端 ID"), peer)) return;
      options.peer_id = peer;
    }
    const auto host = connectHost->text().trimmed();
    if (host.isEmpty()) {
      options.connect.reset();
    } else {
      bool ok = false;
      const auto port = connectPort->text().trimmed().toUShort(&ok);
      if (!ok || port == 0) {
        status->setText(QStringLiteral("手动连接端口必须有效"));
        return;
      }
      options.connect = transport::Endpoint{host.toStdString(), port};
    }
    options.remote_side = remoteSide->currentData().toString() == QStringLiteral("left")
                              ? app::RemoteSide::kLeft
                              : app::RemoteSide::kRight;
    options.peer_display_id_explicit = !autoTopology->isChecked();
    options.remote_side_explicit = !autoTopology->isChecked();
    state->options = options;
    if (startRuntime) startRuntime(options);
  });

  auto* preferences = panel(QStringLiteral("subpanel"));
  auto* preferenceLayout = new QVBoxLayout(preferences);
  preferenceLayout->setContentsMargins(18, 16, 18, 16);
  const auto addPreference = [preferenceLayout](const QString& title, const QString& subtitle,
                                                 bool checked, bool enabled,
                                                 std::function<void(bool)> callback) {
    auto* row = new QWidget;
    auto* rowLayout = new QHBoxLayout(row);
    rowLayout->setContentsMargins(0, 0, 0, 0);
    auto* text = new QVBoxLayout;
    auto* titleLabel = label(title);
    titleLabel->setStyleSheet(QStringLiteral("font-size: 18px;"));
    text->addWidget(titleLabel);
    text->addWidget(label(subtitle, QStringLiteral("muted")));
    rowLayout->addLayout(text);
    rowLayout->addStretch();
    auto* check = new QCheckBox;
    check->setChecked(checked);
    check->setEnabled(enabled);
    if (callback) QObject::connect(check, &QCheckBox::toggled, row, std::move(callback));
    rowLayout->addWidget(check);
    preferenceLayout->addWidget(row);
  };
  addPreference(QStringLiteral("跨屏键鼠转发"), QStringLiteral("由 LANLink runtime 自动处理输入租约"),
                state->crossScreenEnabled, true, std::move(toggleCrossScreen));
  addPreference(QStringLiteral("剪贴板同步"), QStringLiteral("当前核心未提供剪贴板协议"), false, false, {});
  addPreference(QStringLiteral("文件传输"), QStringLiteral("当前核心未提供文件传输协议"), false, false, {});
  layout->addWidget(preferences);

  auto* m0 = panel(QStringLiteral("subpanel"));
  auto* m0Layout = new QVBoxLayout(m0);
  m0Layout->setContentsMargins(18, 16, 18, 16);
  auto* m0Title = label(QStringLiteral("M0 输入引擎参数"));
  m0Title->setStyleSheet(QStringLiteral("font-size: 21px; font-weight: 600;"));
  m0Layout->addWidget(m0Title);
  const QVector<QPair<QString, QString>> rows = {
      {QStringLiteral("心跳间隔"), QStringLiteral("1000 ms")},
      {QStringLiteral("失联超时"), QStringLiteral("3000 ms")},
      {QStringLiteral("输入租约"), QStringLiteral("3000 ms")},
      {QStringLiteral("越界阈值"), QStringLiteral("4 px")},
      {QStringLiteral("目标入口安全内距"), QStringLiteral("8 px")},
  };
  for (const auto& [name, value] : rows) {
    auto* row = new QHBoxLayout;
    row->addWidget(label(name, QStringLiteral("muted")));
    row->addStretch();
    row->addWidget(label(value));
    m0Layout->addLayout(row);
  }
  layout->addWidget(m0);
  layout->addStretch();
  return page;
}
}  // namespace lanlink::desktop

#include "pages.moc"

#pragma once

#include <QWidget>

#include "desktop_state.hpp"

namespace lanlink::desktop {

QWidget* buildOverviewPage(DesktopState* state, QWidget* parent = nullptr);
QWidget* buildLayoutPage(DesktopState* state, QWidget* parent = nullptr);
QWidget* buildDevicesPage(const DesktopState& state, QWidget* parent = nullptr);
QWidget* buildTransferPage(DesktopState* state, QWidget* parent = nullptr);
QWidget* buildSettingsPage(DesktopState* state, QWidget* parent = nullptr);

}  // namespace lanlink::desktop

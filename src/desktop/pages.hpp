#pragma once

#include <QWidget>

#include <functional>

#include "desktop_state.hpp"

namespace lanlink::desktop {

QWidget* buildOverviewPage(DesktopState* state, QWidget* parent = nullptr);
QWidget* buildLayoutPage(DesktopState* state, QWidget* parent = nullptr);
QWidget* buildDevicesPage(const DesktopState& state, QWidget* parent = nullptr);
QWidget* buildTransferPage(DesktopState* state, QWidget* parent = nullptr);
using StartRuntimeCallback = std::function<void(const app::AppOptions&)>;
using StopRuntimeCallback = std::function<void()>;
using TogglePreferenceCallback = std::function<void(bool)>;
QWidget* buildSettingsPage(DesktopState* state, QWidget* parent = nullptr,
                           StartRuntimeCallback startRuntime = {},
                           StopRuntimeCallback stopRuntime = {},
                           TogglePreferenceCallback toggleCrossScreen = {});

}  // namespace lanlink::desktop

#include "lanlink/platform/windows_input_backend.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <string>
#include <utility>
#include <vector>

#if defined(_WIN32)

namespace lanlink::platform {

std::atomic<WindowsInputBackend*> WindowsInputBackend::active_instance_{nullptr};

namespace {
std::uint32_t mouse_button_for_message(WPARAM message, DWORD mouse_data) {
  switch (message) {
    case WM_LBUTTONDOWN:
    case WM_LBUTTONUP:
      return 1;
    case WM_RBUTTONDOWN:
    case WM_RBUTTONUP:
      return 2;
    case WM_MBUTTONDOWN:
    case WM_MBUTTONUP:
      return 3;
    case WM_XBUTTONDOWN:
    case WM_XBUTTONUP:
      return 3U + (HIWORD(mouse_data) == XBUTTON2 ? 2U : 1U);
    default:
      return 0;
  }
}
}  // namespace

WindowsInputBackend::WindowsInputBackend() = default;

WindowsInputBackend::~WindowsInputBackend() { stop(); }

Result<void> WindowsInputBackend::start(EventCallback callback) {
  std::lock_guard operation_lock(operation_mutex_);
  if (hook_thread_.joinable() && hook_thread_.get_id() != std::this_thread::get_id())
    hook_thread_.join();
  {
    std::lock_guard lock(mutex_);
    if (status_.running) {
      callback_ = std::move(callback);
      return Result<void>::success();
    }
    if (active_instance_.load(std::memory_order_acquire) != nullptr &&
        active_instance_.load(std::memory_order_acquire) != this) {
      status_.last_error = "another Windows input backend is active";
      status_.degraded = true;
      return Result<void>::failure(status_.last_error);
    }
    callback_ = std::move(callback);
    status_.permission = true;
    status_.capture = false;
    status_.injection = true;
    status_.suppression = false;
    status_.last_error.clear();
    hook_ready_ = false;
    stop_requested_.store(false, std::memory_order_release);
    have_last_point_ = false;
    hook_thread_ = std::thread(&WindowsInputBackend::message_thread, this);
  }

  std::unique_lock lock(mutex_);
  const bool ready = hook_ready_cv_.wait_for(
      lock, std::chrono::seconds(2), [this] { return hook_ready_; });
  if (!ready) {
    stop_requested_.store(true, std::memory_order_release);
    const DWORD thread_id = hook_thread_id_;
    lock.unlock();
    if (thread_id != 0) PostThreadMessageW(thread_id, WM_QUIT, 0, 0);
    join_thread();
    std::lock_guard cleanup(mutex_);
    status_.running = false;
    status_.capture = false;
    status_.degraded = true;
    status_.last_error = "low-level hook startup timed out";
    return Result<void>::failure(status_.last_error);
  }
  if (!status_.capture) {
    lock.unlock();
    join_thread();
    std::lock_guard cleanup(mutex_);
    status_.running = false;
    status_.degraded = true;
    if (status_.last_error.empty()) status_.last_error = "low-level hooks unavailable";
    return Result<void>::failure(status_.last_error);
  }
  status_.running = true;
  status_.degraded = false;
  return Result<void>::success();
}

void WindowsInputBackend::stop() {
  std::lock_guard operation_lock(operation_mutex_);
  DWORD thread_id = 0;
  {
    std::lock_guard lock(mutex_);
    status_.running = false;
    status_.suppression = false;
    stop_requested_.store(true, std::memory_order_release);
    thread_id = hook_thread_id_;
  }
  if (thread_id != 0) PostThreadMessageW(thread_id, WM_QUIT, 0, 0);
  join_thread();
  std::lock_guard lock(mutex_);
  release_all_locked();
  status_.capture = false;
  status_.injection = true;
  status_.degraded = !status_.permission;
  if (active_instance_.load(std::memory_order_acquire) == this)
    active_instance_.store(nullptr, std::memory_order_release);
  hook_thread_id_ = 0;
  hook_ready_ = false;
  have_last_point_ = false;
}

Result<void> WindowsInputBackend::set_suppression(bool enabled) {
  std::lock_guard lock(mutex_);
  if (enabled && !status_.running) {
    status_.last_error = "capture unavailable";
    status_.degraded = true;
    return Result<void>::failure(status_.last_error);
  }
  status_.suppression = enabled;
  return Result<void>::success();
}

Result<void> WindowsInputBackend::inject(const InputEvent& event) {
  std::lock_guard operation_lock(operation_mutex_);
  INPUT input{};
  bool track_key = false;
  bool track_button = false;
  std::uint32_t code = 0;
  bool pressed = false;
  if (const auto* key = std::get_if<Key>(&event.payload)) {
    input.type = INPUT_KEYBOARD;
    input.ki.wVk = static_cast<WORD>(key->code & 0xFFFFU);
    input.ki.dwFlags = key->pressed ? 0 : KEYEVENTF_KEYUP;
    input.ki.dwExtraInfo = own_extra_info();
    track_key = true;
    code = key->code;
    pressed = key->pressed;
  } else if (const auto* button = std::get_if<MouseButton>(&event.payload)) {
    input.type = INPUT_MOUSE;
    code = button->button;
    pressed = button->pressed;
    track_button = true;
    switch (button->button) {
      case 1: input.mi.dwFlags = button->pressed ? MOUSEEVENTF_LEFTDOWN : MOUSEEVENTF_LEFTUP; break;
      case 2: input.mi.dwFlags = button->pressed ? MOUSEEVENTF_RIGHTDOWN : MOUSEEVENTF_RIGHTUP; break;
      case 3: input.mi.dwFlags = button->pressed ? MOUSEEVENTF_MIDDLEDOWN : MOUSEEVENTF_MIDDLEUP; break;
      case 4: input.mi.dwFlags = button->pressed ? MOUSEEVENTF_XDOWN : MOUSEEVENTF_XUP; input.mi.mouseData = XBUTTON1; break;
      case 5: input.mi.dwFlags = button->pressed ? MOUSEEVENTF_XDOWN : MOUSEEVENTF_XUP; input.mi.mouseData = XBUTTON2; break;
      default: return Result<void>::failure("unsupported mouse button");
    }
    input.mi.dwExtraInfo = own_extra_info();
  } else if (const auto* delta = std::get_if<MouseDelta>(&event.payload)) {
    input.type = INPUT_MOUSE;
    input.mi.dwFlags = MOUSEEVENTF_MOVE;
    input.mi.dx = static_cast<LONG>(std::llround(delta->dx));
    input.mi.dy = static_cast<LONG>(std::llround(delta->dy));
    input.mi.dwExtraInfo = own_extra_info();
  } else if (const auto* wheel = std::get_if<MouseWheel>(&event.payload)) {
    input.type = INPUT_MOUSE;
    input.mi.dwFlags = MOUSEEVENTF_WHEEL;
    input.mi.mouseData = static_cast<DWORD>(std::llround(wheel->delta * WHEEL_DELTA));
    input.mi.dwExtraInfo = own_extra_info();
  } else if (const auto* warp = std::get_if<PointerWarp>(&event.payload)) {
    input.type = INPUT_MOUSE;
    const auto width = std::max(1, GetSystemMetrics(SM_CXSCREEN) - 1);
    const auto height = std::max(1, GetSystemMetrics(SM_CYSCREEN) - 1);
    input.mi.dx = static_cast<LONG>(std::clamp(warp->x, 0.0, 1.0) * 65535.0);
    input.mi.dy = static_cast<LONG>(std::clamp(warp->y, 0.0, 1.0) * 65535.0);
    if (warp->x > 1.0 || warp->y > 1.0) {
      input.mi.dx = static_cast<LONG>(std::clamp(warp->x / width, 0.0, 1.0) * 65535.0);
      input.mi.dy = static_cast<LONG>(std::clamp(warp->y / height, 0.0, 1.0) * 65535.0);
    }
    input.mi.dwFlags = MOUSEEVENTF_MOVE | MOUSEEVENTF_ABSOLUTE;
    input.mi.dwExtraInfo = own_extra_info();
  }

  {
    std::lock_guard lock(mutex_);
    if (!status_.running || !status_.injection) {
      status_.injection = false;
      status_.degraded = true;
      status_.last_error = "injection unavailable";
      return Result<void>::failure(status_.last_error);
    }
  }
  if (!send_inputs(&input, 1)) {
    std::lock_guard lock(mutex_);
    status_.injection = false;
    status_.degraded = true;
    status_.last_error = "SendInput failed";
    return Result<void>::failure(status_.last_error);
  }
  if (track_key || track_button) {
    std::lock_guard lock(mutex_);
    auto& held = held_[event.session];
    auto& ledger = track_key ? held.keys : held.buttons;
    if (pressed) ledger.insert(code);
    else ledger.erase(code);
    if (held.keys.empty() && held.buttons.empty()) held_.erase(event.session);
  }
  return Result<void>::success();
}

void WindowsInputBackend::release_injected_state(const SessionId& session) {
  std::lock_guard operation_lock(operation_mutex_);
  std::lock_guard lock(mutex_);
  auto found = held_.find(session);
  if (found == held_.end()) return;
  std::vector<INPUT> inputs;
  for (auto code : found->second.keys) {
    INPUT input{}; input.type = INPUT_KEYBOARD; input.ki.wVk = static_cast<WORD>(code & 0xFFFFU);
    input.ki.dwFlags = KEYEVENTF_KEYUP; input.ki.dwExtraInfo = own_extra_info(); inputs.push_back(input);
  }
  for (auto button : found->second.buttons) {
    INPUT input{}; input.type = INPUT_MOUSE; input.mi.dwExtraInfo = own_extra_info();
    if (button == 1) input.mi.dwFlags = MOUSEEVENTF_LEFTUP;
    else if (button == 2) input.mi.dwFlags = MOUSEEVENTF_RIGHTUP;
    else if (button == 3) input.mi.dwFlags = MOUSEEVENTF_MIDDLEUP;
    else { input.mi.dwFlags = MOUSEEVENTF_XUP; input.mi.mouseData = button == 4 ? XBUTTON1 : XBUTTON2; }
    inputs.push_back(input);
  }
  if (!inputs.empty() && SendInput(static_cast<UINT>(inputs.size()), inputs.data(), sizeof(INPUT)) != static_cast<UINT>(inputs.size())) {
    status_.injection = false;
    status_.degraded = true;
    status_.last_error = "SendInput release failed";
  }
  held_.erase(found);
}

Result<void> WindowsInputBackend::recover() {
  std::lock_guard operation_lock(operation_mutex_);
  EventCallback callback;
  {
    std::lock_guard lock(mutex_);
    callback = callback_;
    ++status_.recovery_count;
  }
  stop();
  return start(std::move(callback));
}

LRESULT CALLBACK WindowsInputBackend::keyboard_proc(int code, WPARAM wparam, LPARAM lparam) {
  auto* backend = active_instance_.load(std::memory_order_acquire);
  if (code != HC_ACTION || backend == nullptr) return CallNextHookEx(nullptr, code, wparam, lparam);
  const auto& info = *reinterpret_cast<const KBDLLHOOKSTRUCT*>(lparam);
  if ((info.flags & LLKHF_INJECTED) != 0 || info.dwExtraInfo == reinterpret_cast<ULONG_PTR>(backend)) return CallNextHookEx(nullptr, code, wparam, lparam);
  backend->handle_keyboard(info, wparam);
  std::lock_guard lock(backend->mutex_);
  return backend->status_.suppression ? 1 : CallNextHookEx(nullptr, code, wparam, lparam);
}

LRESULT CALLBACK WindowsInputBackend::mouse_proc(int code, WPARAM wparam, LPARAM lparam) {
  auto* backend = active_instance_.load(std::memory_order_acquire);
  if (code != HC_ACTION || backend == nullptr) return CallNextHookEx(nullptr, code, wparam, lparam);
  const auto& info = *reinterpret_cast<const MSLLHOOKSTRUCT*>(lparam);
  if ((info.flags & LLMHF_INJECTED) != 0 || info.dwExtraInfo == reinterpret_cast<ULONG_PTR>(backend)) return CallNextHookEx(nullptr, code, wparam, lparam);
  backend->handle_mouse(info, wparam);
  std::lock_guard lock(backend->mutex_);
  return backend->status_.suppression ? 1 : CallNextHookEx(nullptr, code, wparam, lparam);
}

void WindowsInputBackend::handle_keyboard(const KBDLLHOOKSTRUCT& info, WPARAM message) {
  InputEvent event{};
  event.payload = Key{info.vkCode, message == WM_KEYDOWN || message == WM_SYSKEYDOWN};
  event.origin = InputOrigin::kPhysical;
  dispatch(std::move(event));
}

void WindowsInputBackend::handle_mouse(const MSLLHOOKSTRUCT& info, WPARAM message) {
  InputEvent event{};
  event.origin = InputOrigin::kPhysical;
  if (message == WM_MOUSEMOVE) {
    if (!have_last_point_) { last_point_ = info.pt; have_last_point_ = true; return; }
    event.payload = MouseDelta{static_cast<double>(info.pt.x - last_point_.x), static_cast<double>(info.pt.y - last_point_.y)};
    last_point_ = info.pt;
  } else if (message == WM_MOUSEWHEEL) {
    event.payload = MouseWheel{static_cast<double>(static_cast<short>(HIWORD(info.mouseData))) / WHEEL_DELTA};
  } else {
    const auto button = mouse_button_for_message(message, info.mouseData);
    if (button == 0) return;
    event.payload = MouseButton{button, message == WM_LBUTTONDOWN || message == WM_RBUTTONDOWN || message == WM_MBUTTONDOWN || message == WM_XBUTTONDOWN};
  }
  dispatch(std::move(event));
}

void WindowsInputBackend::dispatch(InputEvent event) {
  EventCallback callback;
  {
    std::lock_guard lock(mutex_);
    callback = callback_;
  }
  if (callback) callback(event);
}

bool WindowsInputBackend::send_inputs(INPUT* inputs, UINT count) {
  return SendInput(count, inputs, sizeof(INPUT)) == count;
}

void WindowsInputBackend::release_all_locked() {
  for (auto& [session, held] : held_) {
    (void)session;
    std::vector<INPUT> inputs;
    for (auto code : held.keys) { INPUT input{}; input.type = INPUT_KEYBOARD; input.ki.wVk = static_cast<WORD>(code & 0xFFFFU); input.ki.dwFlags = KEYEVENTF_KEYUP; input.ki.dwExtraInfo = own_extra_info(); inputs.push_back(input); }
    for (auto button : held.buttons) { INPUT input{}; input.type = INPUT_MOUSE; input.mi.dwExtraInfo = own_extra_info(); if (button == 1) input.mi.dwFlags = MOUSEEVENTF_LEFTUP; else if (button == 2) input.mi.dwFlags = MOUSEEVENTF_RIGHTUP; else if (button == 3) input.mi.dwFlags = MOUSEEVENTF_MIDDLEUP; else { input.mi.dwFlags = MOUSEEVENTF_XUP; input.mi.mouseData = button == 4 ? XBUTTON1 : XBUTTON2; } inputs.push_back(input); }
    if (!inputs.empty()) SendInput(static_cast<UINT>(inputs.size()), inputs.data(), sizeof(INPUT));
  }
  held_.clear();
}

void WindowsInputBackend::message_thread() {
  {
    std::lock_guard lock(mutex_);
    hook_thread_id_ = GetCurrentThreadId();
    PeekMessageW(nullptr, nullptr, WM_USER, WM_USER, PM_NOREMOVE);
    keyboard_hook_ = SetWindowsHookExW(WH_KEYBOARD_LL, &WindowsInputBackend::keyboard_proc, GetModuleHandleW(nullptr), 0);
    mouse_hook_ = SetWindowsHookExW(WH_MOUSE_LL, &WindowsInputBackend::mouse_proc, GetModuleHandleW(nullptr), 0);
    if (keyboard_hook_ == nullptr || mouse_hook_ == nullptr) {
      if (keyboard_hook_) UnhookWindowsHookEx(keyboard_hook_);
      if (mouse_hook_) UnhookWindowsHookEx(mouse_hook_);
      keyboard_hook_ = nullptr; mouse_hook_ = nullptr;
      status_.last_error = "SetWindowsHookEx failed";
      status_.capture = false;
    } else {
      status_.capture = true;
      active_instance_.store(this, std::memory_order_release);
    }
    hook_ready_ = true;
  }
  hook_ready_cv_.notify_all();
  bool capture_ready = false;
  {
    std::lock_guard lock(mutex_);
    capture_ready = status_.capture;
  }
  if (!capture_ready || stop_requested_.load(std::memory_order_acquire)) {
    std::lock_guard lock(mutex_);
    if (keyboard_hook_) UnhookWindowsHookEx(keyboard_hook_);
    if (mouse_hook_) UnhookWindowsHookEx(mouse_hook_);
    keyboard_hook_ = nullptr;
    mouse_hook_ = nullptr;
    status_.capture = false;
    status_.running = false;
    status_.suppression = false;
    if (active_instance_.load(std::memory_order_acquire) == this)
      active_instance_.store(nullptr, std::memory_order_release);
    return;
  }
  MSG message;
  while (GetMessageW(&message, nullptr, 0, 0) > 0) DispatchMessageW(&message);
  std::lock_guard lock(mutex_);
  if (keyboard_hook_) UnhookWindowsHookEx(keyboard_hook_);
  if (mouse_hook_) UnhookWindowsHookEx(mouse_hook_);
  const bool unexpected_exit = !stop_requested_.load(std::memory_order_acquire);
  keyboard_hook_ = nullptr; mouse_hook_ = nullptr;
  status_.capture = false;
  if (active_instance_.load(std::memory_order_acquire) == this)
    active_instance_.store(nullptr, std::memory_order_release);
  status_.running = false;
  status_.suppression = false;
  if (unexpected_exit) {
    status_.injection = false;
    status_.degraded = true;
    status_.last_error = "low-level hook thread exited unexpectedly";
  }
}

void WindowsInputBackend::set_hook_error(const char* operation) {
  status_.last_error = operation;
  status_.degraded = true;
}

void WindowsInputBackend::join_thread() {
  if (hook_thread_.joinable() && hook_thread_.get_id() != std::this_thread::get_id())
    hook_thread_.join();
}

}  // namespace lanlink::platform

#else

namespace lanlink::platform {
namespace {
constexpr const char* kUnsupported = "Windows input backend unsupported on this platform";
}

WindowsInputBackend::WindowsInputBackend() {
  status_.permission = false;
  status_.capture = false;
  status_.injection = false;
  status_.degraded = true;
  status_.last_error = kUnsupported;
}

WindowsInputBackend::~WindowsInputBackend() = default;

Result<void> WindowsInputBackend::start(EventCallback callback) {
  callback_ = std::move(callback);
  status_.last_error = kUnsupported;
  status_.degraded = true;
  return Result<void>::failure(status_.last_error);
}

void WindowsInputBackend::stop() {
  status_.running = false;
  status_.suppression = false;
  callback_ = {};
}

Result<void> WindowsInputBackend::set_suppression(bool) {
  status_.last_error = kUnsupported;
  status_.degraded = true;
  return Result<void>::failure(status_.last_error);
}

Result<void> WindowsInputBackend::inject(const InputEvent&) {
  status_.last_error = kUnsupported;
  status_.degraded = true;
  return Result<void>::failure(status_.last_error);
}

void WindowsInputBackend::release_injected_state(const SessionId&) {}

Result<void> WindowsInputBackend::recover() {
  ++status_.recovery_count;
  status_.last_error = kUnsupported;
  status_.degraded = true;
  return Result<void>::failure(status_.last_error);
}

}  // namespace lanlink::platform

#endif

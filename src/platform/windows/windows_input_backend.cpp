#include "lanlink/platform/windows_input_backend.hpp"

#include "lanlink/common/debug.hpp"
#include "lanlink/platform/key_mapping.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <iterator>
#include <string>
#include <utility>
#include <vector>

#if defined(_WIN32)

namespace lanlink::platform {

std::atomic<WindowsInputBackend*> WindowsInputBackend::active_instance_{nullptr};

namespace {
constexpr wchar_t kRawInputWindowClass[] = L"LANLink.RawInputWindow";
constexpr UINT kCursorVisibilityMessage = WM_APP + 1;
constexpr auto kInjectedMotionEchoTimeout = std::chrono::milliseconds{250};
constexpr std::uint8_t kInjectedEchoFromHook = 1U;
constexpr std::uint8_t kInjectedEchoFromRawInput = 2U;
constexpr std::uint8_t kInjectedEchoSources = kInjectedEchoFromHook | kInjectedEchoFromRawInput;

// Keep Win32 pointer hooks, cursor APIs, and screen metrics in one coordinate
// space. Without an explicit process DPI context, user32 virtualizes
// GetSystemMetrics/GetCursorPos while low-level hook points remain physical,
// which makes edge detection and handoff drift on scaled displays.
void enable_process_dpi_awareness() noexcept {
  HMODULE user32 = GetModuleHandleW(L"user32.dll");
  if (user32 == nullptr) return;

  using SetProcessDpiAwarenessContextFn = BOOL(WINAPI*)(HANDLE);
  const auto set_context = reinterpret_cast<SetProcessDpiAwarenessContextFn>(
      GetProcAddress(user32, "SetProcessDpiAwarenessContext"));
  if (set_context != nullptr) {
    // DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2 is the documented -4 handle.
    const HANDLE per_monitor_v2 = reinterpret_cast<HANDLE>(static_cast<std::intptr_t>(-4));
    if (set_context(per_monitor_v2) != FALSE || GetLastError() == ERROR_ACCESS_DENIED) return;
  }

  // Windows 7/8 do not expose Per-Monitor V2. System awareness still avoids
  // the DPI virtualization that mixes logical metrics with physical hook data.
  using SetProcessDpiAwareFn = BOOL(WINAPI*)();
  const auto set_system = reinterpret_cast<SetProcessDpiAwareFn>(
      GetProcAddress(user32, "SetProcessDPIAware"));
  if (set_system != nullptr) (void)set_system();
}

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

// ShowCursor's display count belongs to the calling thread's input queue.
// Keep every call on the hook/message thread and remember the exact number of
// decrements so restoring visibility returns that thread to its prior count.
bool hide_cursor_on_current_thread(int& adjustment) {
  if (adjustment != 0) return true;
  int count = 0;
  constexpr int kMaxAdjustments = 128;
  do {
    count = ShowCursor(FALSE);
    ++adjustment;
  } while (count >= 0 && adjustment < kMaxAdjustments);
  return count < 0;
}

bool show_cursor_on_current_thread(int& adjustment) {
  while (adjustment > 0) {
    (void)ShowCursor(TRUE);
    --adjustment;
  }
  return true;
}
}  // namespace

WindowsInputBackend::WindowsInputBackend() { enable_process_dpi_awareness(); }

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
      debug::log("windows backend start failed: another backend is active");
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
    raw_input_ready_ = false;
    have_last_point_ = false;
    pending_move_dx_ = 0.0;
    pending_move_dy_ = 0.0;
    pending_injected_motions_.clear();
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
    debug::log("windows backend start failed: low-level hook startup timed out");
    return Result<void>::failure(status_.last_error);
  }
  if (!status_.capture) {
    lock.unlock();
    join_thread();
    std::lock_guard cleanup(mutex_);
    status_.running = false;
    status_.degraded = true;
    if (status_.last_error.empty()) status_.last_error = "low-level hooks unavailable";
    debug::log("windows backend start failed: ", status_.last_error);
    return Result<void>::failure(status_.last_error);
  }
  status_.running = true;
  status_.degraded = false;
  debug::log("windows backend started capture=", status_.capture,
             " injection=", status_.injection);
  return Result<void>::success();
}

void WindowsInputBackend::stop() {
  std::lock_guard operation_lock(operation_mutex_);
  DWORD thread_id = 0;
  bool restore_cursor = false;
  {
    std::lock_guard lock(mutex_);
    restore_cursor = cursor_hidden_;
  }
  if (restore_cursor && !request_cursor_visibility(false)) {
    debug::log("windows cursor restore request failed during stop");
  }
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
  status_.injection = status_.permission && held_.empty();
  status_.degraded = !status_.permission || !status_.injection;
  if (active_instance_.load(std::memory_order_acquire) == this)
    active_instance_.store(nullptr, std::memory_order_release);
  hook_thread_id_ = 0;
  hook_ready_ = false;
  have_last_point_ = false;
  pending_move_dx_ = 0.0;
  pending_move_dy_ = 0.0;
  pending_injected_motions_.clear();
}

Result<void> WindowsInputBackend::set_suppression(bool enabled) {
  std::lock_guard operation_lock(operation_mutex_);
  bool changed = false;
  bool previous = false;
  {
    std::lock_guard lock(mutex_);
    if (enabled && !status_.running) {
      status_.last_error = "capture unavailable";
      status_.degraded = true;
      debug::log("windows suppression failed enabled=", enabled,
                 " reason=capture unavailable");
      return Result<void>::failure(status_.last_error);
    }
    previous = status_.suppression;
    changed = previous != enabled;
    status_.suppression = enabled;
  }
  if (!changed) return Result<void>::success();
  debug::log("windows suppression=", enabled ? "on" : "off");
  if (!request_cursor_visibility(enabled)) {
    std::lock_guard lock(mutex_);
    status_.suppression = previous;
    status_.last_error = "cursor visibility request failed";
    status_.degraded = true;
    return Result<void>::failure(status_.last_error);
  }
  debug::log("windows cursor ", enabled ? "hidden" : "shown");
  return Result<void>::success();
}

std::optional<Rect> WindowsInputBackend::display_bounds() const noexcept {
  enable_process_dpi_awareness();
  const int left = GetSystemMetrics(SM_XVIRTUALSCREEN);
  const int top = GetSystemMetrics(SM_YVIRTUALSCREEN);
  const int width = GetSystemMetrics(SM_CXVIRTUALSCREEN);
  const int height = GetSystemMetrics(SM_CYVIRTUALSCREEN);
  if (width <= 0 || height <= 0) {
    debug::log("windows display_bounds unavailable");
    return std::nullopt;
  }
  const Rect bounds{static_cast<double>(left), static_cast<double>(top),
                    static_cast<double>(left + width), static_cast<double>(top + height)};
  debug::log("windows display_bounds=", bounds.left, ",", bounds.top, " ", bounds.right,
             "x", bounds.bottom);
  return bounds;
}

double WindowsInputBackend::display_scale() const noexcept {
  enable_process_dpi_awareness();
  UINT dpi = 96;
  HMODULE user32 = GetModuleHandleW(L"user32.dll");
  if (user32 != nullptr) {
    using GetDpiForSystemFn = UINT(WINAPI*)();
    const auto get_dpi = reinterpret_cast<GetDpiForSystemFn>(
        GetProcAddress(user32, "GetDpiForSystem"));
    if (get_dpi != nullptr && get_dpi() > 0) dpi = get_dpi();
  }
  const double scale = static_cast<double>(dpi) / 96.0;
  debug::log("windows display_scale=", scale);
  return scale;
}

Result<void> WindowsInputBackend::warp_pointer(Point point) {
  return inject(InputEvent{{}, {}, 0, 0, PointerWarp{point.x, point.y},
                           InputOrigin::kSynthetic});
}

std::optional<Point> WindowsInputBackend::pointer_position() const noexcept {
  POINT current{};
  if (GetCursorPos(&current))
    return Point{static_cast<double>(current.x), static_cast<double>(current.y)};
  std::lock_guard lock(mutex_);
  if (!have_last_point_) return std::nullopt;
  return Point{static_cast<double>(last_point_.x), static_cast<double>(last_point_.y)};
}

Result<void> WindowsInputBackend::inject(const InputEvent& event) {
  std::lock_guard operation_lock(operation_mutex_);
  INPUT input{};
  bool track_key = false;
  bool track_button = false;
  std::uint32_t code = 0;
  bool pressed = false;
  bool has_pending_move = false;
  double next_pending_move_dx = 0.0;
  double next_pending_move_dy = 0.0;
  LONG injected_move_dx = 0;
  LONG injected_move_dy = 0;
  std::optional<POINT> expected_absolute_point;
  std::uint64_t injected_motion_id = 0;
  if (const auto* key = std::get_if<Key>(&event.payload)) {
    const auto vk = hid_to_windows_vk(key->code);
    if (!vk.has_value()) {
      std::lock_guard lock(mutex_);
      status_.degraded = true;
      status_.last_error = "unsupported HID key usage";
      return Result<void>::failure(status_.last_error);
    }
    input.type = INPUT_KEYBOARD;
    const auto stroke = windows_key_stroke_for_hid(key->code);
    input.ki.wVk = stroke.has_value() ? 0 : static_cast<WORD>(*vk & 0xFFFFU);
    input.ki.wScan = stroke.has_value() ? stroke->scan_code : 0;
    input.ki.dwFlags = key->pressed ? 0 : KEYEVENTF_KEYUP;
    if (stroke.has_value()) {
      input.ki.dwFlags |= KEYEVENTF_SCANCODE;
      if (stroke->extended) input.ki.dwFlags |= KEYEVENTF_EXTENDEDKEY;
    }
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
    {
      std::lock_guard lock(mutex_);
      const double total_dx = pending_move_dx_ + delta->dx;
      const double total_dy = pending_move_dy_ + delta->dy;
      injected_move_dx = static_cast<LONG>(std::llround(total_dx));
      injected_move_dy = static_cast<LONG>(std::llround(total_dy));
      next_pending_move_dx = total_dx - static_cast<double>(injected_move_dx);
      next_pending_move_dy = total_dy - static_cast<double>(injected_move_dy);
      has_pending_move = true;
    }
    input.mi.dx = injected_move_dx;
    input.mi.dy = injected_move_dy;
    input.mi.dwExtraInfo = own_extra_info();
  } else if (const auto* move = std::get_if<PointerMove>(&event.payload)) {
    input.type = INPUT_MOUSE;
    const auto origin_x = GetSystemMetrics(SM_XVIRTUALSCREEN);
    const auto origin_y = GetSystemMetrics(SM_YVIRTUALSCREEN);
    const auto width = GetSystemMetrics(SM_CXVIRTUALSCREEN);
    const auto height = GetSystemMetrics(SM_CYVIRTUALSCREEN);
    input.mi.dx = static_cast<LONG>(windows_absolute_mouse_coordinate(move->x, origin_x, width));
    input.mi.dy = static_cast<LONG>(windows_absolute_mouse_coordinate(move->y, origin_y, height));
    input.mi.dwFlags = MOUSEEVENTF_MOVE | MOUSEEVENTF_ABSOLUTE | MOUSEEVENTF_VIRTUALDESK;
    input.mi.dwExtraInfo = own_extra_info();
    expected_absolute_point = POINT{static_cast<LONG>(std::llround(move->x)),
                                    static_cast<LONG>(std::llround(move->y))};
    debug::log("windows pointer move point=", move->x, ",", move->y);
  } else if (const auto* wheel = std::get_if<MouseWheel>(&event.payload)) {
    input.type = INPUT_MOUSE;
    input.mi.dwFlags = MOUSEEVENTF_WHEEL;
    input.mi.mouseData = static_cast<DWORD>(std::llround(wheel->delta * WHEEL_DELTA));
    input.mi.dwExtraInfo = own_extra_info();
  } else if (const auto* warp = std::get_if<PointerWarp>(&event.payload)) {
    input.type = INPUT_MOUSE;
    const auto origin_x = GetSystemMetrics(SM_XVIRTUALSCREEN);
    const auto origin_y = GetSystemMetrics(SM_YVIRTUALSCREEN);
    const auto width = GetSystemMetrics(SM_CXVIRTUALSCREEN);
    const auto height = GetSystemMetrics(SM_CYVIRTUALSCREEN);
    input.mi.dx = static_cast<LONG>(windows_absolute_mouse_coordinate(warp->x, origin_x, width));
    input.mi.dy = static_cast<LONG>(windows_absolute_mouse_coordinate(warp->y, origin_y, height));
    input.mi.dwFlags = MOUSEEVENTF_MOVE | MOUSEEVENTF_ABSOLUTE | MOUSEEVENTF_VIRTUALDESK;
    input.mi.dwExtraInfo = own_extra_info();
    expected_absolute_point = POINT{static_cast<LONG>(std::llround(warp->x)),
                                    static_cast<LONG>(std::llround(warp->y))};
    debug::log("windows pointer warp point=", warp->x, ",", warp->y);
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
  // PointerWarp is a local handoff synchronization operation. Its native
  // echo can coincide with the user's first real edge movement, so do not put
  // it in the markerless remote-motion ledger. PointerMove is a regular
  // remote event and still needs the short echo correlation window.
  const bool track_markerless_echo = std::holds_alternative<PointerMove>(event.payload);
  if (expected_absolute_point.has_value() && track_markerless_echo) {
    std::lock_guard lock(mutex_);
    PendingInjectedMotion pending{};
    pending.id = ++next_injected_motion_id_;
    pending.point = *expected_absolute_point;
    if (have_last_point_) {
      pending.delta_x = pending.point.x - last_point_.x;
      pending.delta_y = pending.point.y - last_point_.y;
      pending.has_delta = true;
    }
    pending.deadline = std::chrono::steady_clock::now() + kInjectedMotionEchoTimeout;
    injected_motion_id = pending.id;
    pending_injected_motions_.push_back(pending);
    while (pending_injected_motions_.size() > 64)
      pending_injected_motions_.pop_front();
  }
  const bool sent = send_inputs(&input, 1) == 1;
  if (!sent) {
    std::lock_guard lock(mutex_);
    if (injected_motion_id != 0) {
      const auto it = std::find_if(
          pending_injected_motions_.begin(), pending_injected_motions_.end(),
          [injected_motion_id](const PendingInjectedMotion& pending) {
            return pending.id == injected_motion_id;
          });
      if (it != pending_injected_motions_.end()) pending_injected_motions_.erase(it);
    }
    status_.injection = false;
    status_.degraded = true;
    status_.last_error = "SendInput failed";
    debug::log("windows input injection failed: SendInput failed");
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
  if (const auto* delta = std::get_if<MouseDelta>(&event.payload)) {
    std::lock_guard lock(mutex_);
    if (has_pending_move) {
      pending_move_dx_ = next_pending_move_dx;
      pending_move_dy_ = next_pending_move_dy;
    }
    if (have_last_point_) {
      last_point_.x += injected_move_dx;
      last_point_.y += injected_move_dy;
    }
  } else if (const auto* warp = std::get_if<PointerWarp>(&event.payload)) {
    std::lock_guard lock(mutex_);
    pending_move_dx_ = 0.0;
    pending_move_dy_ = 0.0;
    last_point_.x = static_cast<LONG>(std::llround(warp->x));
    last_point_.y = static_cast<LONG>(std::llround(warp->y));
    have_last_point_ = true;
  } else if (const auto* move = std::get_if<PointerMove>(&event.payload)) {
    std::lock_guard lock(mutex_);
    pending_move_dx_ = 0.0;
    pending_move_dy_ = 0.0;
    last_point_.x = static_cast<LONG>(std::llround(move->x));
    last_point_.y = static_cast<LONG>(std::llround(move->y));
    have_last_point_ = true;
  }
  return Result<void>::success();
}

bool WindowsInputBackend::release_session_locked(const SessionId& session) {
  auto found = held_.find(session);
  if (found == held_.end()) return true;

  std::vector<INPUT> inputs;
  std::vector<std::pair<bool, std::uint32_t>> released;
  for (const auto code : found->second.keys) {
    INPUT input{};
    input.type = INPUT_KEYBOARD;
    const auto vk = windows_release_vk_for_hid(code);
    if (!vk.has_value()) continue;
    const auto stroke = windows_key_stroke_for_hid(code);
    input.ki.wVk = stroke.has_value() ? 0 : static_cast<WORD>(*vk & 0xFFFFU);
    input.ki.wScan = stroke.has_value() ? stroke->scan_code : 0;
    input.ki.dwFlags = KEYEVENTF_KEYUP;
    if (stroke.has_value()) {
      input.ki.dwFlags |= KEYEVENTF_SCANCODE;
      if (stroke->extended) input.ki.dwFlags |= KEYEVENTF_EXTENDEDKEY;
    }
    input.ki.dwExtraInfo = own_extra_info();
    inputs.push_back(input);
    released.emplace_back(true, code);
  }
  for (const auto button : found->second.buttons) {
    INPUT input{};
    input.type = INPUT_MOUSE;
    input.mi.dwExtraInfo = own_extra_info();
    if (button == 1) input.mi.dwFlags = MOUSEEVENTF_LEFTUP;
    else if (button == 2) input.mi.dwFlags = MOUSEEVENTF_RIGHTUP;
    else if (button == 3) input.mi.dwFlags = MOUSEEVENTF_MIDDLEUP;
    else { input.mi.dwFlags = MOUSEEVENTF_XUP; input.mi.mouseData = button == 4 ? XBUTTON1 : XBUTTON2; }
    inputs.push_back(input);
    released.emplace_back(false, button);
  }

  const auto input_count = static_cast<UINT>(inputs.size());
  const UINT sent = inputs.empty() ? 0 : send_inputs(inputs.data(), input_count);
  const auto released_count = std::min<std::size_t>(sent, released.size());
  for (std::size_t index = 0; index < released_count; ++index) {
    const auto [key, code] = released[index];
    if (key) found->second.keys.erase(code);
    else found->second.buttons.erase(code);
  }
  if (found->second.keys.empty() && found->second.buttons.empty()) held_.erase(found);

  if (sent != inputs.size()) {
    status_.injection = false;
    status_.degraded = true;
    status_.last_error = "SendInput release failed";
    return false;
  }
  return true;
}

void WindowsInputBackend::release_injected_state(const SessionId& session) {
  std::lock_guard operation_lock(operation_mutex_);
  std::lock_guard lock(mutex_);
  release_session_locked(session);
  // Motion echoes are not associated with a session. Drop them when the
  // corresponding lease is released so a later physical edge movement cannot
  // be mistaken for an old remote injection.
  pending_injected_motions_.clear();
  pending_move_dx_ = 0.0;
  pending_move_dy_ = 0.0;
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
  auto restarted = start(std::move(callback));
  if (!restarted.ok()) return restarted;
  std::lock_guard lock(mutex_);
  release_all_locked();
  if (!held_.empty()) {
    status_.injection = false;
    status_.degraded = true;
    if (status_.last_error.empty()) status_.last_error = "SendInput release failed";
    return Result<void>::failure(status_.last_error);
  }
  return Result<void>::success();
}

LRESULT CALLBACK WindowsInputBackend::keyboard_proc(int code, WPARAM wparam, LPARAM lparam) {
  auto* backend = active_instance_.load(std::memory_order_acquire);
  if (code != HC_ACTION || backend == nullptr) return CallNextHookEx(nullptr, code, wparam, lparam);
  const auto& info = *reinterpret_cast<const KBDLLHOOKSTRUCT*>(lparam);
  if ((info.flags & LLKHF_INJECTED) != 0 || info.dwExtraInfo == reinterpret_cast<ULONG_PTR>(backend)) return CallNextHookEx(nullptr, code, wparam, lparam);
  backend->handle_keyboard(info, wparam);
  return detail::dispatch_hook(
      [backend] {
        std::lock_guard lock(backend->mutex_);
        return backend->status_.suppression;
      },
      [=] { return CallNextHookEx(nullptr, code, wparam, lparam); });
}

LRESULT CALLBACK WindowsInputBackend::mouse_proc(int code, WPARAM wparam, LPARAM lparam) {
  auto* backend = active_instance_.load(std::memory_order_acquire);
  if (code != HC_ACTION || backend == nullptr) return CallNextHookEx(nullptr, code, wparam, lparam);
  const auto& info = *reinterpret_cast<const MSLLHOOKSTRUCT*>(lparam);
  if ((info.flags & LLMHF_INJECTED) != 0 || info.dwExtraInfo == reinterpret_cast<ULONG_PTR>(backend)) return CallNextHookEx(nullptr, code, wparam, lparam);
  backend->handle_mouse(info, wparam);
  return detail::dispatch_hook(
      [backend] {
        std::lock_guard lock(backend->mutex_);
        return backend->status_.suppression;
      },
      [=] { return CallNextHookEx(nullptr, code, wparam, lparam); });
}

LRESULT CALLBACK WindowsInputBackend::raw_input_proc(HWND window, UINT message, WPARAM wparam,
                                                     LPARAM lparam) {
  auto* backend = reinterpret_cast<WindowsInputBackend*>(
      GetWindowLongPtrW(window, GWLP_USERDATA));
  if (message == WM_NCCREATE) {
    const auto* create = reinterpret_cast<const CREATESTRUCTW*>(lparam);
    backend = static_cast<WindowsInputBackend*>(create->lpCreateParams);
    SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(backend));
  }
  if (backend != nullptr && message == WM_INPUT)
    backend->handle_raw_input(reinterpret_cast<HRAWINPUT>(lparam));
  return DefWindowProcW(window, message, wparam, lparam);
}

void WindowsInputBackend::handle_keyboard(const KBDLLHOOKSTRUCT& info, WPARAM message) {
  std::optional<std::uint32_t> hid;
  if (info.vkCode == VK_SHIFT) {
    hid = info.scanCode == 0x36 ? 0xE5u : 0xE1u;
  } else if (info.vkCode == VK_CONTROL) {
    hid = (info.flags & LLKHF_EXTENDED) != 0 ? 0xE4u : 0xE0u;
  } else if (info.vkCode == VK_MENU) {
    hid = (info.flags & LLKHF_EXTENDED) != 0 ? 0xE6u : 0xE2u;
  } else if (info.vkCode == VK_RETURN && (info.flags & LLKHF_EXTENDED) != 0) {
    hid = 0x58u;
  } else {
    hid = windows_vk_to_hid(info.vkCode);
  }
  if (!hid.has_value()) {
    std::lock_guard lock(mutex_);
    status_.degraded = true;
    status_.last_error = "unsupported Windows virtual key";
    return;
  }
  InputEvent event{};
  event.payload = Key{*hid, message == WM_KEYDOWN || message == WM_SYSKEYDOWN};
  event.origin = InputOrigin::kPhysical;
  dispatch(std::move(event));
}

void WindowsInputBackend::handle_mouse(const MSLLHOOKSTRUCT& info, WPARAM message) {
  InputEvent event{};
  event.origin = InputOrigin::kPhysical;
  if (message == WM_MOUSEMOVE) {
    if (consume_markerless_injected_point(info.pt, kInjectedEchoFromHook)) {
      debug::log("windows ignored markerless injected hook echo point=", info.pt.x,
                 ",", info.pt.y);
      return;
    }
    std::lock_guard lock(mutex_);
    if (!have_last_point_) {
      last_point_ = info.pt;
      have_last_point_ = true;
      return;
    }
    if (raw_input_ready_) {
      // The low-level hook exposes only the clamped screen point. Raw Input
      // below supplies relative deltas when the pointer is held at an edge.
      last_point_ = info.pt;
      return;
    }
    event.payload = MouseDelta{static_cast<double>(info.pt.x - last_point_.x),
                               static_cast<double>(info.pt.y - last_point_.y)};
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

bool WindowsInputBackend::consume_markerless_injected_point(POINT point,
                                                            std::uint8_t source,
                                                            LONG delta_x,
                                                            LONG delta_y,
                                                            bool has_delta,
                                                            bool has_point) {
  std::lock_guard lock(mutex_);
  const auto now = std::chrono::steady_clock::now();
  while (!pending_injected_motions_.empty() &&
         pending_injected_motions_.front().deadline <= now)
    pending_injected_motions_.pop_front();
  for (auto it = pending_injected_motions_.rbegin();
       it != pending_injected_motions_.rend(); ++it) {
    const bool point_match = has_point && std::abs(point.x - it->point.x) <= 4 &&
                             std::abs(point.y - it->point.y) <= 4;
    const bool delta_match = has_delta && it->has_delta &&
                             std::abs(delta_x - it->delta_x) <= 4 &&
                             std::abs(delta_y - it->delta_y) <= 4;
    if (!point_match && !delta_match) {
      if (it->deadline <= now) break;
      continue;
    }
    if ((it->seen_sources & source) != 0) continue;
    it->seen_sources = static_cast<std::uint8_t>(it->seen_sources | source);
    const bool seen_all_sources =
        (it->seen_sources & kInjectedEchoSources) == kInjectedEchoSources;
    if (seen_all_sources) pending_injected_motions_.erase(std::next(it).base());
    return true;
  }
  return false;
}

void WindowsInputBackend::handle_raw_input(HRAWINPUT input) {
  UINT size = 0;
  if (GetRawInputData(input, RID_INPUT, nullptr, &size, sizeof(RAWINPUTHEADER)) ==
      static_cast<UINT>(-1))
    return;
  if (size < sizeof(RAWINPUT)) return;
  std::vector<std::uint8_t> buffer(size);
  if (GetRawInputData(input, RID_INPUT, buffer.data(), &size, sizeof(RAWINPUTHEADER)) ==
      static_cast<UINT>(-1))
    return;
  const auto* raw = reinterpret_cast<const RAWINPUT*>(buffer.data());
  if (raw->header.dwType != RIM_TYPEMOUSE) return;
  const auto& mouse = raw->data.mouse;
  // Some Windows versions mirror SendInput motion through WM_INPUT. Preserve
  // the backend marker so injected remote movement never re-enters the local
  // physical-input path and preempts the active lease.
  {
    std::lock_guard lock(mutex_);
    const auto now = std::chrono::steady_clock::now();
    while (!pending_injected_motions_.empty() &&
           pending_injected_motions_.front().deadline <= now)
      pending_injected_motions_.pop_front();
    if (mouse.ulExtraInformation == static_cast<ULONG>(own_extra_info())) {
      if (!pending_injected_motions_.empty()) pending_injected_motions_.pop_front();
      return;
    }
  }
  if ((mouse.usFlags & MOUSE_MOVE_ABSOLUTE) != 0) return;
  if (mouse.lLastX == 0 && mouse.lLastY == 0) return;
  // A few Windows versions drop dwExtraInfo when mirroring an absolute
  // SendInput through WM_INPUT and report it as a relative move. Correlate
  // that echo with the expected absolute screen point for a very short window.
  // The low-level hook uses the same ledger, so a mirrored event delivered to
  // both paths is consumed once by each path instead of becoming physical
  // input after the first callback removes the record.
  POINT current{};
  const bool have_current = GetCursorPos(&current);
  if (consume_markerless_injected_point(current, kInjectedEchoFromRawInput, mouse.lLastX,
                                        mouse.lLastY, true, have_current)) {
    debug::log("windows ignored markerless injected mouse echo point=", current.x,
               ",", current.y, " delta=", mouse.lLastX, ",", mouse.lLastY,
               " point_valid=", have_current);
    return;
  }
  InputEvent event{};
  event.origin = InputOrigin::kPhysical;
  event.payload = MouseDelta{static_cast<double>(mouse.lLastX),
                             static_cast<double>(mouse.lLastY)};
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

UINT WindowsInputBackend::send_inputs(INPUT* inputs, UINT count) {
  debug::log("windows SendInput begin count=", count);
  const UINT sent = SendInput(count, inputs, sizeof(INPUT));
  debug::log("windows SendInput end count=", count, " sent=", sent);
  return sent;
}

bool WindowsInputBackend::request_cursor_visibility(bool hidden) {
  // set_suppression and stop can run on a different thread from the Win32
  // message loop. Marshal the operation to the hook thread instead of calling
  // ShowCursor from the caller's input queue.
  DWORD thread_id = 0;
  std::uint32_t request_id = 0;
  {
    std::lock_guard lock(mutex_);
    if (cursor_hidden_ == hidden) return true;
    if (!status_.running || hook_thread_id_ == 0) return false;
    thread_id = hook_thread_id_;
    request_id = ++cursor_visibility_request_id_;
  }
  if (thread_id == GetCurrentThreadId()) {
    handle_cursor_visibility_message(static_cast<WPARAM>(hidden),
                                     static_cast<LPARAM>(request_id));
    std::lock_guard lock(mutex_);
    return cursor_visibility_completed_id_ >= request_id && cursor_hidden_ == hidden;
  }
  if (!PostThreadMessageW(thread_id, kCursorVisibilityMessage,
                          static_cast<WPARAM>(hidden),
                          static_cast<LPARAM>(request_id))) {
    return false;
  }
  std::unique_lock lock(mutex_);
  const bool completed = cursor_visibility_cv_.wait_for(
      lock, std::chrono::seconds(1), [this, request_id] {
        return cursor_visibility_completed_id_ >= request_id || !status_.running;
      });
  return completed && cursor_visibility_completed_id_ >= request_id &&
         cursor_hidden_ == hidden;
}

void WindowsInputBackend::handle_cursor_visibility_message(WPARAM wparam, LPARAM lparam) {
  const bool hidden = wparam != 0;
  bool was_hidden = false;
  {
    std::lock_guard lock(mutex_);
    was_hidden = cursor_hidden_;
  }
  bool applied = true;
  if (hidden && !was_hidden)
    applied = hide_cursor_on_current_thread(cursor_visibility_adjustment_);
  else if (!hidden && was_hidden)
    applied = show_cursor_on_current_thread(cursor_visibility_adjustment_);
  if (!applied) show_cursor_on_current_thread(cursor_visibility_adjustment_);
  {
    std::lock_guard lock(mutex_);
    cursor_hidden_ = applied && hidden;
    cursor_visibility_completed_id_ =
        std::max(cursor_visibility_completed_id_, static_cast<std::uint32_t>(lparam));
  }
  cursor_visibility_cv_.notify_all();
}

void WindowsInputBackend::release_all_locked() {
  std::vector<SessionId> sessions;
  sessions.reserve(held_.size());
  for (const auto& entry : held_) sessions.push_back(entry.first);
  for (const auto& session : sessions) release_session_locked(session);
}

void WindowsInputBackend::message_thread() {
  bool raw_class_registered = false;
  WNDCLASSEXW window_class{};
  window_class.cbSize = sizeof(window_class);
  window_class.lpfnWndProc = &WindowsInputBackend::raw_input_proc;
  window_class.hInstance = GetModuleHandleW(nullptr);
  window_class.lpszClassName = kRawInputWindowClass;
  const auto class_atom = RegisterClassExW(&window_class);
  if (class_atom != 0) {
    raw_class_registered = true;
  } else if (GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
    debug::log("windows raw input class registration failed");
  }
  raw_input_window_ = CreateWindowExW(0, kRawInputWindowClass, L"LANLink raw input", 0, 0, 0,
                                      0, 0, HWND_MESSAGE, nullptr, GetModuleHandleW(nullptr),
                                      this);
  if (raw_input_window_ != nullptr) {
    RAWINPUTDEVICE device{};
    device.usUsagePage = 0x01;
    device.usUsage = 0x02;
    device.dwFlags = RIDEV_INPUTSINK;
    device.hwndTarget = raw_input_window_;
    raw_input_ready_ = RegisterRawInputDevices(&device, 1, sizeof(device)) != FALSE;
    if (raw_input_ready_)
      debug::log("windows raw input ready");
    else
      debug::log("windows raw input registration failed; using hook deltas");
  } else {
    raw_input_ready_ = false;
    debug::log("windows raw input window creation failed; using hook deltas");
  }
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
    raw_input_ready_ = false;
    if (raw_input_window_ != nullptr) {
      DestroyWindow(raw_input_window_);
      raw_input_window_ = nullptr;
    }
    if (raw_class_registered)
      UnregisterClassW(kRawInputWindowClass, GetModuleHandleW(nullptr));
    return;
  }
  MSG message;
  while (GetMessageW(&message, nullptr, 0, 0) > 0) {
    if (message.message == kCursorVisibilityMessage)
      handle_cursor_visibility_message(message.wParam, message.lParam);
    else
      DispatchMessageW(&message);
  }
  bool unexpected_exit = false;
  bool restore_cursor = false;
  {
    std::lock_guard lock(mutex_);
    restore_cursor = cursor_hidden_;
    cursor_hidden_ = false;
    if (keyboard_hook_) UnhookWindowsHookEx(keyboard_hook_);
    if (mouse_hook_) UnhookWindowsHookEx(mouse_hook_);
    unexpected_exit = !stop_requested_.load(std::memory_order_acquire);
    keyboard_hook_ = nullptr;
    mouse_hook_ = nullptr;
    status_.capture = false;
    if (active_instance_.load(std::memory_order_acquire) == this)
      active_instance_.store(nullptr, std::memory_order_release);
    status_.running = false;
    status_.suppression = false;
    pending_injected_motions_.clear();
    raw_input_ready_ = false;
    if (unexpected_exit) {
      status_.injection = false;
      status_.degraded = true;
      status_.last_error = "low-level hook thread exited unexpectedly";
      debug::log("windows hook thread exited unexpectedly");
    }
  }
  if (restore_cursor) show_cursor_on_current_thread(cursor_visibility_adjustment_);
  cursor_visibility_cv_.notify_all();
  if (raw_input_window_ != nullptr) {
    DestroyWindow(raw_input_window_);
    raw_input_window_ = nullptr;
  }
  if (raw_class_registered)
    UnregisterClassW(kRawInputWindowClass, GetModuleHandleW(nullptr));
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

Result<void> WindowsInputBackend::warp_pointer(Point) {
  status_.last_error = kUnsupported;
  status_.degraded = true;
  return Result<void>::failure(status_.last_error);
}

Result<void> WindowsInputBackend::inject(const InputEvent&) {
  status_.last_error = kUnsupported;
  status_.degraded = true;
  return Result<void>::failure(status_.last_error);
}

std::optional<Point> WindowsInputBackend::pointer_position() const noexcept {
  return std::nullopt;
}

std::optional<Rect> WindowsInputBackend::display_bounds() const noexcept {
  return std::nullopt;
}
double WindowsInputBackend::display_scale() const noexcept { return 1.0; }

void WindowsInputBackend::release_injected_state(const SessionId&) {}

Result<void> WindowsInputBackend::recover() {
  ++status_.recovery_count;
  status_.last_error = kUnsupported;
  status_.degraded = true;
  return Result<void>::failure(status_.last_error);
}

}  // namespace lanlink::platform

#endif

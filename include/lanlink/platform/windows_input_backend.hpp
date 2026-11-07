#pragma once

#include "lanlink/platform/input_backend.hpp"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <unordered_set>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#ifdef max
#undef max
#endif
#ifdef min
#undef min
#endif
#endif

namespace lanlink::platform {

// Win32 low-level hook and SendInput implementation. On non-Windows platforms
// this type is an explicit unsupported backend so callers can select a native
// backend without conditional construction code.
class WindowsInputBackend final : public IInputBackend {
 public:
  WindowsInputBackend();
  ~WindowsInputBackend() override;

  Result<void> start(EventCallback callback) override;
  void stop() override;
  Result<void> set_suppression(bool enabled) override;
  Result<void> inject(const InputEvent& event) override;
  std::optional<Rect> display_bounds() const noexcept override;
  Result<void> warp_pointer(Point point) override;
  std::optional<Point> pointer_position() const noexcept override;
  void release_injected_state(const SessionId& session) override;
  Result<void> recover() override;
  const BackendStatus& status() const noexcept override { return status_; }

 private:
#if defined(_WIN32)
  struct HeldState {
    std::unordered_set<std::uint32_t> keys;
    std::unordered_set<std::uint32_t> buttons;
  };
  struct PendingInjectedMove {
    LONG dx{};
    LONG dy{};
    std::chrono::steady_clock::time_point deadline{};
  };

  static LRESULT CALLBACK keyboard_proc(int code, WPARAM wparam, LPARAM lparam);
  static LRESULT CALLBACK mouse_proc(int code, WPARAM wparam, LPARAM lparam);
  static LRESULT CALLBACK raw_input_proc(HWND window, UINT message, WPARAM wparam,
                                         LPARAM lparam);
  void message_thread();
  void handle_keyboard(const KBDLLHOOKSTRUCT& info, WPARAM message);
  void handle_mouse(const MSLLHOOKSTRUCT& info, WPARAM message);
  void handle_raw_input(HRAWINPUT input);
  void dispatch(InputEvent event);
  bool send_inputs(INPUT* inputs, UINT count);
  bool release_session_locked(const SessionId& session);
  void release_all_locked();
  void set_hook_error(const char* operation);
  void join_thread();

  ULONG_PTR own_extra_info() const noexcept { return reinterpret_cast<ULONG_PTR>(this); }
  static std::atomic<WindowsInputBackend*> active_instance_;
  HHOOK keyboard_hook_{nullptr};
  HHOOK mouse_hook_{nullptr};
  HWND raw_input_window_{nullptr};
  std::thread hook_thread_;
  DWORD hook_thread_id_{0};
  mutable std::mutex mutex_;
  std::recursive_mutex operation_mutex_;
  std::condition_variable hook_ready_cv_;
  bool hook_ready_{false};
  std::atomic<bool> stop_requested_{false};
  bool raw_input_ready_{false};
  bool cursor_hidden_{false};
  std::deque<PendingInjectedMove> pending_injected_moves_;
  EventCallback callback_;
  std::unordered_map<SessionId, HeldState> held_;
  POINT last_point_{};
  bool have_last_point_{false};
  double pending_move_dx_{0.0};
  double pending_move_dy_{0.0};
#else
  EventCallback callback_;
#endif

  BackendStatus status_{};
};

using NativeInputBackend = WindowsInputBackend;
using Win32InputBackend = WindowsInputBackend;

}  // namespace lanlink::platform

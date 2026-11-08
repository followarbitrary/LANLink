#include "lanlink/platform/macos_input_backend.hpp"

#include "lanlink/common/debug.hpp"
#include "lanlink/platform/key_mapping.hpp"

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cmath>
#include <deque>
#include <mutex>
#include <optional>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#if defined(__APPLE__)

#include <ApplicationServices/ApplicationServices.h>
#include <CoreFoundation/CoreFoundation.h>

namespace lanlink::platform {
namespace {
constexpr std::int64_t kOwnEventUserData = 0x4c4c4e4bLL;

struct HeldState {
  std::unordered_set<std::uint32_t> keys;
  std::unordered_set<std::uint32_t> buttons;
};

CGEventType button_type(std::uint32_t button, bool pressed) {
  if (button == 1) return pressed ? kCGEventLeftMouseDown : kCGEventLeftMouseUp;
  if (button == 2) return pressed ? kCGEventRightMouseDown : kCGEventRightMouseUp;
  return pressed ? kCGEventOtherMouseDown : kCGEventOtherMouseUp;
}

std::optional<InputEvent> convert_event(CGEventType type, CGEventRef event, CGPoint& last,
                                         bool& have_last) {
  InputEvent converted{};
  converted.origin = InputOrigin::kPhysical;
  switch (type) {
    case kCGEventKeyDown:
    case kCGEventKeyUp: {
      const auto keycode = static_cast<std::uint32_t>(
          CGEventGetIntegerValueField(event, kCGKeyboardEventKeycode));
      const auto hid = mac_keycode_to_hid(keycode);
      if (!hid.has_value()) return std::nullopt;
      converted.payload = Key{*hid, type == kCGEventKeyDown};
      return converted;
    }
    case kCGEventMouseMoved:
    case kCGEventLeftMouseDragged:
    case kCGEventRightMouseDragged:
    case kCGEventOtherMouseDragged: {
      const CGPoint point = CGEventGetLocation(event);
      if (!have_last) {
        last = point;
        have_last = true;
        return std::nullopt;
      }
      // At a desktop edge Quartz keeps the cursor location clamped, while
      // the event still carries the raw device delta. Prefer those fields so
      // the runtime can accumulate outward motion and hand off after the
      // edge threshold instead of seeing an endless stream of zero deltas.
      double dx = static_cast<double>(
          CGEventGetIntegerValueField(event, kCGMouseEventDeltaX));
      double dy = static_cast<double>(
          CGEventGetIntegerValueField(event, kCGMouseEventDeltaY));
      if (dx == 0.0 && dy == 0.0) {
        dx = point.x - last.x;
        dy = point.y - last.y;
      }
      converted.payload = MouseDelta{dx, dy};
      last = point;
      return converted;
    }
    case kCGEventLeftMouseDown:
    case kCGEventLeftMouseUp:
      converted.payload = MouseButton{1, type == kCGEventLeftMouseDown};
      return converted;
    case kCGEventRightMouseDown:
    case kCGEventRightMouseUp:
      converted.payload = MouseButton{2, type == kCGEventRightMouseDown};
      return converted;
    case kCGEventOtherMouseDown:
    case kCGEventOtherMouseUp:
      converted.payload = MouseButton{3, type == kCGEventOtherMouseDown};
      return converted;
    case kCGEventScrollWheel:
      converted.payload = MouseWheel{static_cast<double>(CGEventGetIntegerValueField(
          event, kCGScrollWheelEventDeltaAxis1))};
      return converted;
    default:
      return std::nullopt;
  }
}

// Quartz pointer APIs use the global desktop coordinate space in points. This
// is the same logical space returned by CGEventGetLocation, including Retina
// scaling, so callers must not multiply these dimensions by a backing scale.
std::optional<Rect> query_display_bounds() noexcept {
  try {
    CGDisplayCount display_count = 0;
    if (CGGetActiveDisplayList(0, nullptr, &display_count) != CGDisplayNoErr || display_count == 0)
      return std::nullopt;

    std::vector<CGDirectDisplayID> displays(display_count);
    if (CGGetActiveDisplayList(display_count, displays.data(), &display_count) != CGDisplayNoErr)
      return std::nullopt;

    CGRect total_bounds = CGRectZero;
    bool have_bounds = false;
    for (CGDisplayCount index = 0; index < display_count; ++index) {
      const CGRect bounds = CGDisplayBounds(displays[index]);
      total_bounds = have_bounds ? CGRectUnion(total_bounds, bounds) : bounds;
      have_bounds = true;
    }

    if (!(total_bounds.size.width > 0.0) || !(total_bounds.size.height > 0.0) ||
        !std::isfinite(total_bounds.origin.x) || !std::isfinite(total_bounds.origin.y) ||
        !std::isfinite(total_bounds.size.width) || !std::isfinite(total_bounds.size.height))
      return std::nullopt;
    return Rect{total_bounds.origin.x, total_bounds.origin.y,
                total_bounds.origin.x + total_bounds.size.width,
                total_bounds.origin.y + total_bounds.size.height};
  } catch (...) {
    return std::nullopt;
  }
}

double query_display_scale() noexcept {
  try {
    const auto display = CGMainDisplayID();
    const auto bounds = CGDisplayBounds(display);
    const auto pixel_width = CGDisplayPixelsWide(display);
    if (bounds.size.width > 0.0 && pixel_width > 0) {
      const double scale = static_cast<double>(pixel_width) / bounds.size.width;
      if (std::isfinite(scale) && scale > 0.0) return scale;
    }
  } catch (...) {
  }
  return 1.0;
}

}  // namespace

struct MacOSInputBackend::Impl {
  explicit Impl(MacOSInputBackend* owner_value) : owner(owner_value) {}
  MacOSInputBackend* owner;
  std::mutex mutex;
  std::recursive_mutex operation_mutex;
  std::condition_variable ready_cv;
  IInputBackend::EventCallback callback;
  std::thread thread;
  CFRunLoopRef run_loop{nullptr};
  CFMachPortRef tap{nullptr};
  CFRunLoopSourceRef source{nullptr};
  bool ready{false};
  bool stop_requested{false};
  bool tap_disabled{false};
  bool suppression{false};
  bool cursor_hidden{false};
  CGPoint last_point{};
  bool have_last_point{false};
  // CGWarpMouseCursorPosition emits a normal mouse-moved event without the
  // user-data marker used by CGEventPost. Consume that event at the exact
  // warp point so a remote handoff is not mistaken for local input and
  // immediately revoked. Keep the point separately because remote deltas may
  // be posted before the asynchronous warp event reaches the tap.
  struct PendingWarp {
    CGPoint point{};
    std::chrono::steady_clock::time_point deadline{};
  };
  std::deque<PendingWarp> ignored_warps;
  std::unordered_map<SessionId, HeldState> held;
};

namespace {
CGEventRef tap_callback(CGEventTapProxy, CGEventType type, CGEventRef event, void* user_info) {
  auto* impl = static_cast<MacOSInputBackend::Impl*>(user_info);
  if (impl == nullptr || event == nullptr) return event;
  if (type == kCGEventTapDisabledByTimeout || type == kCGEventTapDisabledByUserInput) {
    std::lock_guard lock(impl->mutex);
    impl->tap_disabled = true;
    debug::log("mac event tap disabled type=", static_cast<int>(type), "; re-enabling");
    if (impl->tap != nullptr) CGEventTapEnable(impl->tap, true);
    return event;
  }
  const bool own_event =
      CGEventGetIntegerValueField(event, kCGEventSourceUserData) == kOwnEventUserData;

  std::optional<InputEvent> converted;
  IInputBackend::EventCallback callback;
  bool suppress = false;
  {
    std::lock_guard lock(impl->mutex);
    if (type == kCGEventMouseMoved || type == kCGEventLeftMouseDragged ||
        type == kCGEventRightMouseDragged || type == kCGEventOtherMouseDragged) {
      const CGPoint point = CGEventGetLocation(event);
      const auto now = std::chrono::steady_clock::now();
      // CGWarpMouseCursorPosition is asynchronous. Keep all recent warp
      // points because an older source-edge warp can arrive after a newer
      // target-entry warp. A single slot would misclassify that stale event
      // as physical input and revoke the new lease.
      for (auto it = impl->ignored_warps.begin(); it != impl->ignored_warps.end();) {
        if (now >= it->deadline)
          it = impl->ignored_warps.erase(it);
        else
          ++it;
      }
      constexpr double kWarpTolerance = 1.0;
      for (auto it = impl->ignored_warps.begin(); it != impl->ignored_warps.end(); ++it) {
        const bool is_warp = std::abs(point.x - it->point.x) <= kWarpTolerance &&
                             std::abs(point.y - it->point.y) <= kWarpTolerance;
        if (is_warp) {
          impl->last_point = point;
          impl->have_last_point = true;
          impl->ignored_warps.erase(it);
          debug::log("mac ignored asynchronous warp event point=", point.x, ",", point.y);
          return impl->suppression ? nullptr : event;
        }
      }
    }
    if (own_event) return event;
    converted = convert_event(type, event, impl->last_point, impl->have_last_point);
    callback = impl->callback;
    suppress = impl->suppression;
  }
  if (converted && callback) callback(*converted);
  return suppress ? nullptr : event;
}
}  // namespace

MacOSInputBackend::MacOSInputBackend() : impl_(std::make_unique<Impl>(this)) {
  status_.permission = true;
  status_.capture = false;
  status_.injection = false;
}

MacOSInputBackend::~MacOSInputBackend() { stop(); }

Result<void> MacOSInputBackend::start(EventCallback callback) {
  std::lock_guard operation_lock(impl_->operation_mutex);
  if (impl_->thread.joinable() && impl_->thread.get_id() != std::this_thread::get_id())
    impl_->thread.join();
  if (status_.running) {
    std::lock_guard lock(impl_->mutex);
    impl_->callback = std::move(callback);
    return Result<void>::success();
  }
  if (!AXIsProcessTrusted()) {
    debug::log("mac backend start failed: Accessibility permission denied");
    status_.permission = false;
    status_.capture = false;
    status_.injection = false;
    status_.degraded = true;
    status_.last_error = "Accessibility permission denied";
    return Result<void>::failure(status_.last_error);
  }
  {
    std::lock_guard lock(impl_->mutex);
    impl_->callback = std::move(callback);
    impl_->ready = false;
    impl_->stop_requested = false;
    impl_->tap_disabled = false;
    impl_->suppression = false;
    impl_->cursor_hidden = false;
    impl_->have_last_point = false;
    impl_->ignored_warps.clear();
    impl_->thread = std::thread([this] {
      CFMachPortRef tap = CGEventTapCreate(
          kCGHIDEventTap, kCGHeadInsertEventTap, kCGEventTapOptionDefault,
          CGEventMaskBit(kCGEventKeyDown) | CGEventMaskBit(kCGEventKeyUp) |
              CGEventMaskBit(kCGEventMouseMoved) | CGEventMaskBit(kCGEventLeftMouseDown) |
              CGEventMaskBit(kCGEventLeftMouseUp) | CGEventMaskBit(kCGEventRightMouseDown) |
              CGEventMaskBit(kCGEventRightMouseUp) | CGEventMaskBit(kCGEventOtherMouseDown) |
              CGEventMaskBit(kCGEventOtherMouseUp) | CGEventMaskBit(kCGEventLeftMouseDragged) |
              CGEventMaskBit(kCGEventRightMouseDragged) | CGEventMaskBit(kCGEventOtherMouseDragged) |
              CGEventMaskBit(kCGEventScrollWheel),
          &tap_callback, impl_.get());
      {
        std::lock_guard lock(impl_->mutex);
        impl_->tap = tap;
        if (tap != nullptr) {
          impl_->source = CFMachPortCreateRunLoopSource(kCFAllocatorDefault, tap, 0);
          impl_->run_loop = CFRunLoopGetCurrent();
          if (impl_->source != nullptr) {
            CFRunLoopAddSource(impl_->run_loop, impl_->source, kCFRunLoopCommonModes);
            CGEventTapEnable(tap, true);
            impl_->ready = true;
          }
        }
        if (!impl_->ready) {
          if (impl_->source != nullptr) CFRelease(impl_->source);
          if (impl_->tap != nullptr) CFRelease(impl_->tap);
          impl_->source = nullptr;
          impl_->tap = nullptr;
          impl_->run_loop = nullptr;
        }
      }
      impl_->ready_cv.notify_all();
      {
        std::lock_guard lock(impl_->mutex);
        if (!impl_->ready || impl_->stop_requested) {
          if (impl_->source != nullptr) CFRelease(impl_->source);
          if (impl_->tap != nullptr) CFRelease(impl_->tap);
          impl_->source = nullptr;
          impl_->tap = nullptr;
          impl_->run_loop = nullptr;
          impl_->ready = false;
          return;
        }
      }
      CFRunLoopRun();
      std::lock_guard lock(impl_->mutex);
      const bool unexpected_exit = !impl_->stop_requested;
      if (impl_->source != nullptr && impl_->run_loop != nullptr) {
        CFRunLoopRemoveSource(impl_->run_loop, impl_->source, kCFRunLoopCommonModes);
        CFRelease(impl_->source);
      }
      if (impl_->tap != nullptr) CFRelease(impl_->tap);
      impl_->source = nullptr;
      impl_->tap = nullptr;
      impl_->run_loop = nullptr;
      impl_->ready = false;
      if (unexpected_exit) {
        impl_->owner->status_.running = false;
        impl_->owner->status_.capture = false;
        impl_->owner->status_.injection = false;
        impl_->owner->status_.suppression = false;
        impl_->owner->status_.degraded = true;
        impl_->owner->status_.last_error = "CGEventTap stopped unexpectedly";
        debug::log("mac event tap stopped unexpectedly");
      }
    });
  }

  std::unique_lock lock(impl_->mutex);
  if (!impl_->ready_cv.wait_for(lock, std::chrono::seconds(2), [this] {
        return impl_->ready || !impl_->thread.joinable();
      })) {
    lock.unlock();
    stop();
    status_.capture = false;
    status_.injection = false;
    status_.degraded = true;
    status_.last_error = "CGEventTap startup timed out";
    debug::log("mac backend start failed: event tap startup timed out");
    return Result<void>::failure(status_.last_error);
  }
  if (!impl_->ready) {
    lock.unlock();
    if (impl_->thread.joinable()) impl_->thread.join();
    status_.capture = false;
    status_.injection = false;
    status_.degraded = true;
    status_.last_error = "CGEventTapCreate failed";
    debug::log("mac backend start failed: CGEventTapCreate failed");
    return Result<void>::failure(status_.last_error);
  }
  status_.permission = true;
  status_.capture = true;
  status_.injection = true;
  status_.running = true;
  status_.degraded = false;
  status_.last_error.clear();
  debug::log("mac backend started capture=1 injection=1");
  return Result<void>::success();
}

void MacOSInputBackend::stop() {
  std::lock_guard operation_lock(impl_->operation_mutex);
  CFRunLoopRef loop = nullptr;
  std::vector<SessionId> sessions;
  bool restore_cursor = false;
  {
    std::lock_guard lock(impl_->mutex);
    restore_cursor = impl_->suppression || status_.suppression || impl_->cursor_hidden;
    status_.running = false;
    status_.suppression = false;
    impl_->suppression = false;
    impl_->stop_requested = true;
    loop = impl_->run_loop;
    sessions.reserve(impl_->held.size());
    for (const auto& entry : impl_->held) sessions.push_back(entry.first);
  }
  if (restore_cursor) {
    const auto association_error = CGAssociateMouseAndMouseCursorPosition(true);
    if (association_error != kCGErrorSuccess)
      debug::log("mac cursor association restore failed error=",
                 static_cast<int>(association_error));
    const auto cursor_error = CGDisplayShowCursor(kCGNullDirectDisplay);
    if (cursor_error != kCGErrorSuccess)
      debug::log("mac cursor show failed error=", static_cast<int>(cursor_error));
  }
  for (const auto& session : sessions) release_injected_state(session);
  if (loop != nullptr) {
    CFRunLoopStop(loop);
    CFRunLoopWakeUp(loop);
  }
  if (impl_->thread.joinable() && impl_->thread.get_id() != std::this_thread::get_id())
    impl_->thread.join();
  std::lock_guard lock(impl_->mutex);
  impl_->callback = {};
  impl_->have_last_point = false;
  impl_->ignored_warps.clear();
  status_.capture = false;
  status_.injection = status_.permission && impl_->held.empty();
  status_.degraded = !status_.permission || !status_.injection;
}

Result<void> MacOSInputBackend::set_suppression(bool enabled) {
  std::lock_guard operation_lock(impl_->operation_mutex);
  std::lock_guard lock(impl_->mutex);
  if (enabled && (!status_.running || !status_.capture)) {
    status_.last_error = "capture unavailable";
    status_.degraded = true;
    debug::log("mac suppression failed enabled=", enabled, " reason=capture unavailable");
    return Result<void>::failure(status_.last_error);
  }
  if (status_.suppression != enabled) {
    const auto association_error =
        CGAssociateMouseAndMouseCursorPosition(enabled ? false : true);
    if (association_error != kCGErrorSuccess)
      debug::log("mac cursor association failed enabled=", enabled,
                 " error=", static_cast<int>(association_error));
    else
      debug::log("mac cursor association=", enabled ? "disconnected" : "connected");
    if (enabled) {
      const auto cursor_error = CGDisplayHideCursor(kCGNullDirectDisplay);
      if (cursor_error != kCGErrorSuccess)
        debug::log("mac cursor hide failed error=", static_cast<int>(cursor_error));
      else {
        impl_->cursor_hidden = true;
        debug::log("mac cursor hidden");
      }
    } else if (impl_->cursor_hidden) {
      const auto cursor_error = CGDisplayShowCursor(kCGNullDirectDisplay);
      if (cursor_error != kCGErrorSuccess)
        debug::log("mac cursor show failed error=", static_cast<int>(cursor_error));
      else {
        impl_->cursor_hidden = false;
        debug::log("mac cursor shown");
      }
    }
    debug::log("mac suppression=", enabled ? "on" : "off");
  }
  status_.suppression = enabled;
  impl_->suppression = enabled;
  return Result<void>::success();
}

std::optional<Rect> MacOSInputBackend::display_bounds() const noexcept {
  const auto bounds = query_display_bounds();
  if (bounds.has_value()) {
    debug::log("mac display_bounds=", bounds->left, ",", bounds->top, " ", bounds->right,
               "x", bounds->bottom);
  } else {
    debug::log("mac display_bounds unavailable");
  }
  return bounds;
}

double MacOSInputBackend::display_scale() const noexcept {
  const double scale = query_display_scale();
  debug::log("mac display_scale=", scale);
  return scale;
}

Result<void> MacOSInputBackend::warp_pointer(Point point) {
  return inject(InputEvent{{}, {}, 0, 0, PointerWarp{point.x, point.y},
                           InputOrigin::kSynthetic});
}

std::optional<Point> MacOSInputBackend::pointer_position() const noexcept {
  std::lock_guard lock(impl_->mutex);
  if (!impl_->have_last_point) return std::nullopt;
  return Point{impl_->last_point.x, impl_->last_point.y};
}

Result<void> MacOSInputBackend::inject(const InputEvent& event) {
  std::lock_guard operation_lock(impl_->operation_mutex);
  {
    std::lock_guard lock(impl_->mutex);
    if (!status_.running || !status_.injection || !AXIsProcessTrusted()) {
      if (!AXIsProcessTrusted()) status_.permission = false;
      status_.injection = false;
      status_.degraded = true;
      status_.last_error = status_.permission ? "injection unavailable"
                                               : "Accessibility permission denied";
      return Result<void>::failure(status_.last_error);
    }
  }
  CGEventRef created = nullptr;
  std::optional<CGPoint> pointer_update;
  if (const auto* key = std::get_if<Key>(&event.payload)) {
    const auto keycode = hid_to_mac_keycode(key->code);
    if (!keycode.has_value()) {
      std::lock_guard lock(impl_->mutex);
      status_.degraded = true;
      status_.last_error = "unsupported HID key usage";
      return Result<void>::failure(status_.last_error);
    }
    created = CGEventCreateKeyboardEvent(nullptr, static_cast<CGKeyCode>(*keycode), key->pressed);
  } else if (const auto* button = std::get_if<MouseButton>(&event.payload)) {
    CGEventRef current = CGEventCreate(nullptr);
    const CGPoint point = current == nullptr ? CGPointZero : CGEventGetLocation(current);
    if (current != nullptr) CFRelease(current);
    if (button->button >= 1 && button->button <= 3)
      created = CGEventCreateMouseEvent(nullptr, button_type(button->button, button->pressed), point,
                                        static_cast<CGMouseButton>(button->button - 1));
  } else if (const auto* delta = std::get_if<MouseDelta>(&event.payload)) {
    // CGEventPost is asynchronous. Reading CGEventGetLocation for every
    // delta can therefore reuse a stale position while several remote moves
    // are queued, making the target cursor advance only a fraction of the
    // source movement. Keep the backend's serialized logical position as the
    // base and consult the system only for the first event.
    CGPoint point{};
    bool have_tracked_point = false;
    {
      std::lock_guard lock(impl_->mutex);
      if (impl_->have_last_point) {
        point = impl_->last_point;
        have_tracked_point = true;
      }
    }
    if (!have_tracked_point) {
      CGEventRef current = CGEventCreate(nullptr);
      point = current == nullptr ? CGPointZero : CGEventGetLocation(current);
      if (current != nullptr) CFRelease(current);
    }
    point.x += delta->dx;
    point.y += delta->dy;
    pointer_update = point;
    created = CGEventCreateMouseEvent(nullptr, kCGEventMouseMoved, point, kCGMouseButtonLeft);
  } else if (const auto* move = std::get_if<PointerMove>(&event.payload)) {
    const CGPoint point = CGPointMake(move->x, move->y);
    pointer_update = point;
    created = CGEventCreateMouseEvent(nullptr, kCGEventMouseMoved, point, kCGMouseButtonLeft);
    debug::log("mac pointer move point=", move->x, ",", move->y);
  } else if (const auto* wheel = std::get_if<MouseWheel>(&event.payload)) {
    created = CGEventCreateScrollWheelEvent(nullptr, kCGScrollEventUnitLine, 1,
                                            static_cast<int32_t>(std::llround(wheel->delta)));
  } else if (const auto* warp = std::get_if<PointerWarp>(&event.payload)) {
    {
      std::lock_guard lock(impl_->mutex);
      impl_->last_point = CGPointMake(warp->x, warp->y);
      impl_->have_last_point = true;
      // The Quartz callback is asynchronous. Keep this point pending long
      // enough for the warp event to arrive, while preserving older pending
      // warps that may be delivered out of order.
      impl_->ignored_warps.push_back(
          Impl::PendingWarp{CGPointMake(warp->x, warp->y),
                            std::chrono::steady_clock::now() + std::chrono::milliseconds{250}});
      while (impl_->ignored_warps.size() > 16) impl_->ignored_warps.pop_front();
    }
    const CGError error = CGWarpMouseCursorPosition(CGPointMake(warp->x, warp->y));
    if (error != kCGErrorSuccess) {
      std::lock_guard lock(impl_->mutex);
      if (!impl_->ignored_warps.empty()) impl_->ignored_warps.pop_back();
      status_.injection = false;
      status_.degraded = true;
      status_.last_error = "CGWarpMouseCursorPosition failed";
      debug::log("mac pointer warp failed point=", warp->x, ",", warp->y);
      return Result<void>::failure(status_.last_error);
    }
    debug::log("mac pointer warp point=", warp->x, ",", warp->y);
    return Result<void>::success();
  }

  {
    std::lock_guard lock(impl_->mutex);
    if (!status_.running || !status_.injection) {
      status_.injection = false;
      status_.degraded = true;
      status_.last_error = "injection unavailable";
      if (created != nullptr) CFRelease(created);
      return Result<void>::failure(status_.last_error);
    }
  }
  if (created == nullptr) {
    status_.degraded = true;
    status_.last_error = "unsupported or invalid event";
    return Result<void>::failure(status_.last_error);
  }
  CGEventSetIntegerValueField(created, kCGEventSourceUserData, kOwnEventUserData);
  if (const auto* delta = std::get_if<MouseDelta>(&event.payload)) {
    // Preserve the relative movement fields used by Quartz clients such as
    // Deskflow in addition to the tracked absolute point. This avoids event
    // coalescing treating a stream of absolute points as repeated warps.
    CGEventSetIntegerValueField(created, kCGMouseEventDeltaX,
                                static_cast<int64_t>(std::llround(delta->dx)));
    CGEventSetIntegerValueField(created, kCGMouseEventDeltaY,
                                static_cast<int64_t>(std::llround(delta->dy)));
  } else if (std::holds_alternative<PointerMove>(event.payload)) {
    // Keep the relative fields deterministic for an absolute move. The event
    // tap uses kCGEventSourceUserData as the authoritative injection marker;
    // matching a pending point and delta could swallow a real move.
    CGEventSetIntegerValueField(created, kCGMouseEventDeltaX, 0);
    CGEventSetIntegerValueField(created, kCGMouseEventDeltaY, 0);
  }
  CGEventPost(kCGHIDEventTap, created);
  CFRelease(created);

  std::lock_guard lock(impl_->mutex);
  if (const auto* key = std::get_if<Key>(&event.payload)) {
    auto& ledger = impl_->held[event.session].keys;
    if (key->pressed) ledger.insert(key->code); else ledger.erase(key->code);
  } else if (const auto* button = std::get_if<MouseButton>(&event.payload)) {
    auto& ledger = impl_->held[event.session].buttons;
    if (button->pressed) ledger.insert(button->button); else ledger.erase(button->button);
  }
  if (impl_->held[event.session].keys.empty() && impl_->held[event.session].buttons.empty())
    impl_->held.erase(event.session);
  if (pointer_update.has_value()) {
    impl_->last_point = *pointer_update;
    impl_->have_last_point = true;
  }
  return Result<void>::success();
}

void MacOSInputBackend::release_injected_state(const SessionId& session) {
  std::lock_guard operation_lock(impl_->operation_mutex);
  if (!AXIsProcessTrusted()) {
    status_.permission = false;
    status_.injection = false;
    status_.degraded = true;
    status_.last_error = "Accessibility permission denied";
    return;
  }
  std::unordered_set<std::uint32_t> keys;
  std::unordered_set<std::uint32_t> buttons;
  {
    std::lock_guard lock(impl_->mutex);
    const auto it = impl_->held.find(session);
    if (it == impl_->held.end()) return;
    keys = it->second.keys;
    buttons = it->second.buttons;
  }

  std::unordered_set<std::uint32_t> released_keys;
  std::unordered_set<std::uint32_t> released_buttons;
  bool failed = false;
  for (const auto key : keys) {
    const auto keycode = mac_release_keycode_for_hid(key);
    if (!keycode.has_value()) {
      failed = true;
      continue;
    }
    CGEventRef event = CGEventCreateKeyboardEvent(nullptr, static_cast<CGKeyCode>(*keycode), false);
    if (event == nullptr) {
      failed = true;
      continue;
    }
    CGEventSetIntegerValueField(event, kCGEventSourceUserData, kOwnEventUserData);
    CGEventPost(kCGHIDEventTap, event);
    CFRelease(event);
    released_keys.insert(key);
  }
  for (const auto button : buttons) {
    if (button < 1 || button > 3) {
      failed = true;
      continue;
    }
    CGEventRef event = CGEventCreateMouseEvent(nullptr, button_type(button, false), CGPointZero,
                                               static_cast<CGMouseButton>(button - 1));
    if (event == nullptr) {
      failed = true;
      continue;
    }
    CGEventSetIntegerValueField(event, kCGEventSourceUserData, kOwnEventUserData);
    CGEventPost(kCGHIDEventTap, event);
    CFRelease(event);
    released_buttons.insert(button);
  }

  std::lock_guard lock(impl_->mutex);
  const auto it = impl_->held.find(session);
  if (it != impl_->held.end()) {
    for (const auto key : released_keys) it->second.keys.erase(key);
    for (const auto button : released_buttons) it->second.buttons.erase(button);
    if (it->second.keys.empty() && it->second.buttons.empty()) impl_->held.erase(it);
  }
  if (failed) {
    status_.injection = false;
    status_.degraded = true;
    status_.last_error = "CGEventCreate release failed";
  }
}

Result<void> MacOSInputBackend::recover() {
  std::lock_guard operation_lock(impl_->operation_mutex);
  ++status_.recovery_count;
  status_.suppression = false;
  const auto association_error = CGAssociateMouseAndMouseCursorPosition(true);
  if (association_error != kCGErrorSuccess)
    debug::log("mac cursor association restore failed error=",
               static_cast<int>(association_error));
  if (impl_->cursor_hidden) {
    const auto cursor_error = CGDisplayShowCursor(kCGNullDirectDisplay);
    if (cursor_error != kCGErrorSuccess)
      debug::log("mac cursor show failed error=", static_cast<int>(cursor_error));
    else
      impl_->cursor_hidden = false;
  }
  std::vector<SessionId> sessions;
  {
    std::lock_guard lock(impl_->mutex);
    sessions.reserve(impl_->held.size());
    for (const auto& entry : impl_->held) sessions.push_back(entry.first);
    impl_->suppression = false;
    if (impl_->tap != nullptr) CGEventTapEnable(impl_->tap, true);
    impl_->tap_disabled = false;
  }
  for (const auto& session : sessions) release_injected_state(session);
  {
    std::lock_guard lock(impl_->mutex);
    if (!impl_->held.empty()) {
      status_.injection = false;
      status_.degraded = true;
      if (status_.last_error.empty()) status_.last_error = "injected state release failed";
      return Result<void>::failure(status_.last_error);
    }
  }
  if (!status_.permission) {
    status_.degraded = true;
    status_.last_error = "Accessibility permission denied";
    return Result<void>::failure(status_.last_error);
  }
  if (!status_.running && status_.last_error == "CGEventTap stopped unexpectedly") {
    IInputBackend::EventCallback callback;
    {
      std::lock_guard lock(impl_->mutex);
      callback = impl_->callback;
    }
    status_.last_error.clear();
    return start(std::move(callback));
  }
  status_.injection = status_.permission && status_.running;
  status_.degraded = !status_.permission || !status_.running || !status_.injection;
  if (status_.running) status_.last_error.clear();
  return Result<void>::success();
}

}  // namespace lanlink::platform

#else

namespace lanlink::platform {
namespace {
constexpr const char* kUnsupported = "macOS input backend unsupported on this platform";
}
struct MacOSInputBackend::Impl {};
MacOSInputBackend::MacOSInputBackend() : impl_(std::make_unique<Impl>()) {
  status_.permission = false;
  status_.capture = false;
  status_.injection = false;
  status_.degraded = true;
  status_.last_error = kUnsupported;
}
MacOSInputBackend::~MacOSInputBackend() = default;
Result<void> MacOSInputBackend::start(EventCallback) {
  status_.last_error = kUnsupported;
  status_.degraded = true;
  return Result<void>::failure(status_.last_error);
}
void MacOSInputBackend::stop() { status_.running = false; status_.suppression = false; }
Result<void> MacOSInputBackend::set_suppression(bool) {
  status_.last_error = kUnsupported;
  status_.degraded = true;
  return Result<void>::failure(status_.last_error);
}

Result<void> MacOSInputBackend::warp_pointer(Point) {
  status_.last_error = kUnsupported;
  status_.degraded = true;
  return Result<void>::failure(status_.last_error);
}
Result<void> MacOSInputBackend::inject(const InputEvent&) {
  status_.last_error = kUnsupported;
  status_.degraded = true;
  return Result<void>::failure(status_.last_error);
}

std::optional<Point> MacOSInputBackend::pointer_position() const noexcept {
  return std::nullopt;
}

std::optional<Rect> MacOSInputBackend::display_bounds() const noexcept {
  return std::nullopt;
}
double MacOSInputBackend::display_scale() const noexcept { return 1.0; }
void MacOSInputBackend::release_injected_state(const SessionId&) {}
Result<void> MacOSInputBackend::recover() {
  ++status_.recovery_count;
  status_.last_error = kUnsupported;
  status_.degraded = true;
  return Result<void>::failure(status_.last_error);
}
}  // namespace lanlink::platform
#endif

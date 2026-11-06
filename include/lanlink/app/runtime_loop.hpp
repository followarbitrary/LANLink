#pragma once

#include <chrono>
#include <string_view>

namespace lanlink::app {

// A signal can interrupt the listener/read wait. That condition is a normal
// shutdown only when the process has already received a stop request.
bool is_shutdown_interrupt(bool stop_requested, std::string_view error) noexcept;

// A listener-only node has no blocking transport operation, so pace its loop.
std::chrono::milliseconds idle_wait(bool connected) noexcept;

}  // namespace lanlink::app

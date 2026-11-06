#include "lanlink/app/runtime_loop.hpp"

namespace lanlink::app {

bool is_shutdown_interrupt(bool stop_requested, std::string_view error) noexcept {
  return stop_requested && error == "read wait interrupted";
}

std::chrono::milliseconds idle_wait(bool connected) noexcept {
  return connected ? std::chrono::milliseconds{0} : std::chrono::milliseconds{10};
}

}  // namespace lanlink::app

#pragma once

#include <sstream>
#include <string>
#include <string_view>

namespace lanlink::debug {

// Debug logging is opt-in so normal operation remains quiet. Set
// LANLINK_DEBUG=1 before starting the node to enable the control-plane and
// handoff messages.
bool enabled() noexcept;
void write(std::string_view message) noexcept;
void warning(std::string_view message) noexcept;

template <typename... Values>
void log(const Values&... values) noexcept {
  if (!enabled()) return;
  try {
    std::ostringstream stream;
    (stream << ... << values);
    write(stream.str());
  } catch (...) {
    // Logging must never change input routing or take the node down.
  }
}

template <typename... Values>
void warn(const Values&... values) noexcept {
  try {
    std::ostringstream stream;
    (stream << ... << values);
    warning(stream.str());
  } catch (...) {
  }
}

}  // namespace lanlink::debug

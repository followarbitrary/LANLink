#include "lanlink/common/debug.hpp"

#include <cstdlib>
#include <iostream>
#include <mutex>

namespace lanlink::debug {
namespace {
std::mutex& output_mutex() {
  static std::mutex mutex;
  return mutex;
}
}  // namespace

bool enabled() noexcept {
  const char* value = std::getenv("LANLINK_DEBUG");
  return value != nullptr && std::string_view(value) == "1";
}

void write(std::string_view message) noexcept {
  try {
    std::lock_guard lock(output_mutex());
    std::cerr << "[LANLink debug] " << message << '\n';
  } catch (...) {
  }
}

void warning(std::string_view message) noexcept {
  try {
    std::lock_guard lock(output_mutex());
    std::cerr << "[LANLink warning] " << message << '\n';
  } catch (...) {
  }
}

}  // namespace lanlink::debug

#pragma once
#include <optional>
#include "lanlink/common/clock.hpp"

namespace lanlink {
class ConnectionLiveness {
 public:
  void mark_alive(IClock::time_point timestamp);
  bool expired(IClock::time_point timestamp) const;
 private:
  std::optional<IClock::time_point> last_alive_;
};
}

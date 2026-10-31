#include "lanlink/common/liveness.hpp"
#include "lanlink/common/config.hpp"
namespace lanlink {
void ConnectionLiveness::mark_alive(IClock::time_point timestamp) { last_alive_ = timestamp; }
bool ConnectionLiveness::expired(IClock::time_point timestamp) const {
  return !last_alive_.has_value() || timestamp - *last_alive_ >= m0::kConnectionTimeout;
}
}

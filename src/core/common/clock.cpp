#include "lanlink/common/clock.hpp"
namespace lanlink {
IClock::time_point MockClock::now() const { return current_; }
void MockClock::advance(Duration amount) { current_ += amount; }
void MockClock::set(time_point value) { current_ = value; }
}

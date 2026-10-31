#pragma once
#include <chrono>

namespace lanlink {
class IClock {
 public:
  using Clock = std::chrono::steady_clock;
  using time_point = Clock::time_point;
  using Duration = Clock::duration;
  virtual ~IClock() = default;
  virtual time_point now() const = 0;
};

class MockClock final : public IClock {
 public:
  MockClock() = default;
  explicit MockClock(time_point initial) : current_(initial) {}
  time_point now() const override;
  void advance(Duration amount);
  void set(time_point value);
 private:
  time_point current_{};
};
}

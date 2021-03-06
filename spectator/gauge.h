#pragma once

#include "meter.h"
#include <atomic>

namespace spectator {

class Gauge : public Meter {
 public:
  Gauge(Id id, absl::Duration ttl) noexcept;
  void Measure(Measurements* results,
               int64_t now = absl::GetCurrentTimeNanos()) const noexcept;

  void Set(double value) noexcept;
  double Get() const noexcept;
  void SetTtl(absl::Duration ttl) noexcept;
  absl::Duration GetTtl() const noexcept {
    return absl::Nanoseconds(ttl_nanos_.load());
  }
  bool HasExpired(int64_t now) const noexcept;

 private:
  std::atomic<int64_t> ttl_nanos_;
  mutable std::unique_ptr<Id> gauge_id_;
  mutable std::atomic<double> value_;
};

}  // namespace spectator

#pragma once
// The monotonic clock the daemon runs on. Pure code never reads it: callers pass `TimePoint now`.

#include <chrono>

namespace omenkbd {

using Clock = std::chrono::steady_clock;
using TimePoint = Clock::time_point;
using Duration = Clock::duration;

} // namespace omenkbd

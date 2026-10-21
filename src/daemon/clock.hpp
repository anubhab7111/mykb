#pragma once

#include <chrono>

namespace omenkbd {

using Clock = std::chrono::steady_clock;
using TimePoint = Clock::time_point;

} // namespace omenkbd

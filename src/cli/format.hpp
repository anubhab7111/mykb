#pragma once
// Human rendering of a daemon status.

#include "ipc/protocol.hpp"

#include <string>

namespace omenkbd {

/// One `label  value` line each: profile, zones, brightness, rules, backend; then error,
/// budget and warnings only when present.
std::string format_status(const Status &s);

} // namespace omenkbd

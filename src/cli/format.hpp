#pragma once
// Human and Waybar renderings of a daemon status.

#include "ipc/protocol.hpp"

#include <nlohmann/json.hpp>

#include <string>

namespace omenkbd {

/// One `label  value` line each: profile, zones, brightness, hint, rules, backend; then error,
/// budget and warnings only when present.
std::string format_status(const Status &s);

/// `{"text", "class", "percentage", "tooltip"}` for a Waybar custom module. class is `error`
/// (last error or budget exhausted), else `off`, `hint`, `effect` (animated) or `on`.
nlohmann::json waybar_json(const Status &s);

/// What `omen-kbd waybar` prints while the daemon is unreachable.
inline constexpr std::string_view kWaybarNotRunning =
    R"({"text":"󰌐 ?","class":"error","tooltip":"omen-kbd daemon not running"})";

} // namespace omenkbd

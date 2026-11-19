#pragma once
// Things that happen outside the daemon (system D-Bus, Hyprland, …), delivered to
// Daemon::handle_event on the loop thread.

#include "core/signals.hpp"
#include "daemon/model.hpp"

#include <optional>
#include <string>
#include <variant>

namespace omenkbd {

namespace event {
/// logind `PrepareForSleep`: `entering` before suspend, `!entering` after resume.
struct Sleep {
    bool entering;
    friend bool operator==(const Sleep &, const Sleep &) = default;
};
/// What the firmware reports for the backlight.
struct HwBacklight {
    bool on;
    friend bool operator==(const HwBacklight &, const HwBacklight &) = default;
};
} // namespace event

/// A `Signal` from the Hyprland side goes to `Daemon::deliver_signal`, which reads the backlight
/// first when a reaction could start.
using Event = std::variant<FactUpdate, event::Sleep, event::HwBacklight, Signal>;

} // namespace omenkbd

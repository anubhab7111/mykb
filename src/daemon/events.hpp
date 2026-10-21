#pragma once
// Things that happen outside the daemon (system D-Bus, Hyprland, theme switch, …), delivered to
// Daemon::handle_event on the loop thread.

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
/// Hyprland submap; nullopt = the default (no) submap.
struct Submap {
    std::optional<std::string> name;
    friend bool operator==(const Submap &, const Submap &) = default;
};
struct ThemeChanged {
    friend bool operator==(const ThemeChanged &, const ThemeChanged &) = default;
};
/// What the firmware reports for the backlight.
struct HwBacklight {
    bool on;
    friend bool operator==(const HwBacklight &, const HwBacklight &) = default;
};
} // namespace event

using Event =
    std::variant<FactUpdate, event::Sleep, event::Submap, event::ThemeChanged, event::HwBacklight>;

} // namespace omenkbd

#pragma once
// The `[reactions]` settings as plain data: how the keyboard reacts to workspace switches, windows,
// notifications, urgent windows and Claude Code sessions, and the limits on it. Parsing and the
// spec defaults live in reactions_config.hpp; this header only holds the shape.

#include "core/clip.hpp"
#include "core/palette.hpp"
#include "core/signals.hpp"
#include "core/zones.hpp"

#include <cstdint>
#include <functional>
#include <map>
#include <string>
#include <vector>

namespace omenkbd {

/// Every ColorRef member starts black only because a ColorRef cannot be default-constructed; the
/// real defaults come from `default_reactions()`.
struct ReactionsCfg {
    /// True when the `[reactions]` table is present (and `enabled = false` is not set).
    bool enabled = false;
    /// One step of a sweep or pulse (60 to 1000) and one on or off phase of a blink (60 to 1000).
    std::uint32_t step_ms = 120, blink_ms = 150;
    /// Short reactions started per rolling minute (1 to 600).
    std::uint32_t max_per_minute = 30;

    struct Workspace {
        ColorRef color = ColorRef::hex(kBlack);
        WasdWith wasd_with = WasdWith::Left;
        friend bool operator==(const Workspace &, const Workspace &) = default;
    } workspace;

    struct Window {
        /// How far a new window lifts the zones toward white, 0 to 1 (0 turns it off).
        double open = 0.5;
        /// The fraction a closing window dips the zones to, 0 to 1 (1 turns it off).
        double close = 0.35;
        /// 0 to 10000.
        std::uint32_t cooldown_ms = 500;
        friend bool operator==(const Window &, const Window &) = default;
    } window;

    struct Notify {
        ColorRef color = ColorRef::hex(kBlack);
        ColorRef critical = ColorRef::hex(kBlack);
        Urgency min_urgency = Urgency::Normal;
        bool skip_if_focused = true;
        /// Per-app colors. Keys are lowercase: an app name, a desktop entry or its last segment.
        std::map<std::string, ColorRef, std::less<>> apps;
        friend bool operator==(const Notify &, const Notify &) = default;
    } notify;

    struct Urgent {
        /// Empty turns the lamp off.
        std::vector<Zone> zones{Zone::Right};
        ColorRef color = ColorRef::hex(kBlack);
        /// Lit phases of the intro before the light stays steady, 0 to 5.
        int blinks = 2;
        friend bool operator==(const Urgent &, const Urgent &) = default;
    } urgent;

    struct Agent {
        /// Empty turns the lamp off.
        std::vector<Zone> zones{Zone::Wasd};
        ColorRef waiting = ColorRef::hex(kBlack);
        ColorRef done = ColorRef::hex(kBlack);
        /// 0 to 5, as for `Urgent`.
        int blinks = 2;
        /// How long a light stays without a new event, 60 to 86400.
        std::uint32_t ttl_s = 3600;
        friend bool operator==(const Agent &, const Agent &) = default;
    } agent;

    friend bool operator==(const ReactionsCfg &, const ReactionsCfg &) = default;
};

} // namespace omenkbd

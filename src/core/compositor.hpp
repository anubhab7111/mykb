#pragma once
// Layer compositor: base effect → rule overlay → mode hint → brightness → backlight gate.
// Pure: the output is a function of the inputs and the time since start.

#include "core/effects.hpp"
#include "core/zones.hpp"

#include <cstdint>
#include <optional>
#include <variant>
#include <vector>

namespace omenkbd {

/// Rule overlays with their colors already resolved against the palette.
struct ResolvedZones {
    std::vector<Zone> zones;
    Rgb color;
    bool pulse;
    friend bool operator==(const ResolvedZones &, const ResolvedZones &) = default;
};

struct ResolvedTint {
    Rgb color;
    double strength;
    friend bool operator==(const ResolvedTint &, const ResolvedTint &) = default;
};

using ResolvedOverlay = std::variant<ResolvedZones, ResolvedTint>;

/// Everything the compositor needs for one frame (logical zone order).
struct Inputs {
    Frame base;
    Effect effect;
    std::uint8_t speed;
    /// Animation allowed right now (effects enabled, not paused by a gate).
    bool animate;
    std::optional<ResolvedOverlay> overlay;
    std::optional<Rgb> hint;
    std::uint8_t brightness;
    /// Backlight should be on (user on, not gated off).
    bool backlight;
    friend bool operator==(const Inputs &, const Inputs &) = default;
};

/// The frame to show, in logical zone order.
struct Output {
    bool backlight;
    Frame zones;
    /// True while the frame changes over time (the daemon must keep ticking).
    bool animated;
    friend bool operator==(const Output &, const Output &) = default;
};

Output compose(const Inputs &i, double t);

} // namespace omenkbd

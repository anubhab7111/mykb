#pragma once
// Zone-level software effects. All functions are pure: a frame is a function of the base colors,
// the time since the effect started, and the speed.

#include "core/zones.hpp"

#include <cstdint>
#include <optional>
#include <string_view>

namespace omenkbd {

/// A lighting effect. Only `Static` costs no hardware writes over time.
///
/// The others are slow ambient glows. They take their stops from the active colors and change the
/// frame only twice a second. Drift and Dusk use all four colors as stops, Glow the first two, Tide
/// the first three for the wave and the fourth (WASD) as a steady accent.
enum class Effect { Static, Drift, Glow, Tide, Dusk };

std::string_view name(Effect e);

/// Case-insensitive.
std::optional<Effect> effect_from_name(std::string_view s);

constexpr bool is_animated(Effect e) { return e != Effect::Static; }

/// Period of one loop in seconds: 32 (Drift), 14 (Glow), 20 (Tide) or 36 (Dusk) at speed 50, four
/// times shorter at speed 100 and longer at speed 1 (speed clamped to 1..100). 0 for Static.
double period_secs(Effect effect, std::uint8_t speed);

/// Renders `effect` at time `t` seconds. `Static` returns `base` unchanged.
Frame render(Effect effect, const Frame &base, double t, std::uint8_t speed);

/// Brightness factor for pulsing alert overlays: 1 s period, range [0.2, 1], 1 at t = 0.
double pulse_factor(double t);

/// Multiplies every channel by `f` in [0, 1], rounding half away from zero.
Rgb multiply(Rgb c, double f);

} // namespace omenkbd

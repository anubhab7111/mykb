#pragma once
// Zone-level software effects. All functions are pure: a frame is a function of the base colors,
// the time since the effect started, and the speed.

#include "core/zones.hpp"

#include <cstdint>
#include <optional>
#include <string_view>

namespace omenkbd {

/// A profile's lighting effect. Only `Static` costs no hardware writes over time.
enum class Effect { Static, Breathe, Wave, Cycle, Gradient };

std::string_view name(Effect e);

/// Case-insensitive.
std::optional<Effect> effect_from_name(std::string_view s);

constexpr bool is_animated(Effect e) { return e != Effect::Static; }

/// Effect period in seconds: speed 100 ⇒ 0.5 s, speed 1 ⇒ 10 s (speed clamped to 1..100).
double period_secs(std::uint8_t speed);

/// Renders `effect` at time `t` seconds. `Static` returns `base` unchanged.
Frame render(Effect effect, const Frame &base, double t, std::uint8_t speed);

/// Brightness factor for pulsing alert overlays: 1 s period, range [0.2, 1], 1 at t = 0.
double pulse_factor(double t);

/// Multiplies every channel by `f` in [0, 1], rounding half away from zero.
Rgb multiply(Rgb c, double f);

} // namespace omenkbd

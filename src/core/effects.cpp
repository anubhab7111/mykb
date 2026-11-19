#include "core/effects.hpp"

#include "util/strings.hpp"

#include <algorithm>
#include <cmath>
#include <numbers>

namespace omenkbd {

namespace {

constexpr double kTau = 2.0 * std::numbers::pi;

/// Position of each logical zone along the keyboard: left, middle, right, and WASD (on the left).
constexpr std::array<double, 4> kPos{0.0, 1.0, 2.0, 0.0};

/// Effects change the frame this often. Each change is one hardware write, so this is the
/// effect's whole write cost: 2 per second is 7200 an hour of the 40000 budget.
constexpr double kUpdatesPerSecond = 2.0;

/// Blends along `c[0..n)` for `u` in [0, n-1].
Rgb along(const Frame &c, std::size_t n, double u) {
  u = std::clamp(u, 0.0, static_cast<double>(n - 1));
  const auto i = std::min(n - 2, static_cast<std::size_t>(std::floor(u)));
  return Rgb::lerp(c[i], c[i + 1], u - static_cast<double>(i));
}

/// Blends around the loop `c[0..4)` back to `c[0]`.
Rgb around(const Frame &c, double u) {
  u = std::fmod(u, 4.0);
  if (u < 0.0) {
    u += 4.0;
  }
  const auto i = static_cast<std::size_t>(std::floor(u)) % 4;
  return Rgb::lerp(c[i], c[(i + 1) % 4], u - std::floor(u));
}

} // namespace

std::string_view name(Effect e) {
  switch (e) {
  case Effect::Static:
    return "static";
  case Effect::Drift:
    return "drift";
  case Effect::Glow:
    return "glow";
  case Effect::Tide:
    return "tide";
  case Effect::Dusk:
    return "dusk";
  }
  return "?";
}

std::optional<Effect> effect_from_name(std::string_view s) {
  const std::string lower = to_lower(s);
  for (Effect e : {Effect::Static, Effect::Drift, Effect::Glow, Effect::Tide, Effect::Dusk}) {
    if (name(e) == lower) {
      return e;
    }
  }
  return std::nullopt;
}

double period_secs(Effect effect, std::uint8_t speed) {
  double natural = 0.0;
  switch (effect) {
  case Effect::Drift:
    natural = 32.0;
    break;
  case Effect::Glow:
    natural = 14.0;
    break;
  case Effect::Tide:
    natural = 20.0;
    break;
  case Effect::Dusk:
    natural = 36.0;
    break;
  case Effect::Static:
    return 0.0;
  }
  const double s = std::clamp<double>(speed, 1.0, 100.0);
  return natural * std::exp2((50.0 - s) / 25.0);
}

double pulse_factor(double t) { return 0.6 + 0.4 * std::cos(kTau * t); }

Rgb multiply(Rgb c, double f) {
  f = std::clamp(f, 0.0, 1.0);
  const auto m = [f](std::uint8_t x) {
    return static_cast<std::uint8_t>(std::round(static_cast<double>(x) * f));
  };
  return {m(c.r), m(c.g), m(c.b)};
}

Frame render(Effect effect, const Frame &base, double t, std::uint8_t speed) {
  if (effect == Effect::Static) {
    return base;
  }
  const double p = period_secs(effect, speed);
  const double q = std::floor(t * kUpdatesPerSecond) / kUpdatesPerSecond;
  Frame out = base;
  for (std::size_t i = 0; i < 4; ++i) {
    switch (effect) {
    case Effect::Drift:
      out[i] = around(base, q / p * 4.0 - kPos[i] * 0.6);
      break;
    case Effect::Glow: {
      const double k = 0.5 - 0.5 * std::cos(kTau * (q / p - 0.06 * kPos[i]));
      out[i] = multiply(Rgb::lerp(base[0], base[1], k), 0.62 + 0.38 * k);
      break;
    }
    case Effect::Tide: {
      if (i == 3) {
        break; // WASD keeps its own color
      }
      const double w = 0.5 + 0.5 * std::sin(kTau * (q / p - kPos[i] / 3.0));
      out[i] = along(base, 3, w * 2.0);
      break;
    }
    case Effect::Dusk:
      out[i] = along(base, 4, 1.5 + 1.5 * std::sin(kTau * q / p - 0.35 * kPos[i]));
      break;
    case Effect::Static:
      break;
    }
  }
  return out;
}

} // namespace omenkbd

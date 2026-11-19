#include "core/effects.hpp"

#include "util/strings.hpp"

#include <algorithm>
#include <cmath>
#include <numbers>

namespace omenkbd {

namespace {

constexpr double kTau = 2.0 * std::numbers::pi;

/// Wave/cycle position of each logical zone: left, middle, right, and WASD (on
/// the left).
constexpr std::array<double, 4> kPos{0.0, 1.0, 2.0, 0.0};

/// The slow glows change the frame this often. Each change is one hardware write, so this is the
/// glow's whole write cost: 2 per second is 7200 an hour of the 40000 budget.
constexpr double kSlowUpdatesPerSecond = 2.0;

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

Frame render_slow(Effect effect, const Frame &base, double t, std::uint8_t speed) {
  const double p = slow_period_secs(effect, speed);
  const double q = std::floor(t * kSlowUpdatesPerSecond) / kSlowUpdatesPerSecond;
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
    default:
      break;
    }
  }
  return out;
}

} // namespace

std::string_view name(Effect e) {
  switch (e) {
  case Effect::Static:
    return "static";
  case Effect::Breathe:
    return "breathe";
  case Effect::Wave:
    return "wave";
  case Effect::Cycle:
    return "cycle";
  case Effect::Gradient:
    return "gradient";
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
  for (Effect e : {Effect::Static, Effect::Breathe, Effect::Wave, Effect::Cycle, Effect::Gradient,
                   Effect::Drift, Effect::Glow, Effect::Tide, Effect::Dusk}) {
    if (name(e) == lower) {
      return e;
    }
  }
  return std::nullopt;
}

double period_secs(std::uint8_t speed) {
  const double s = std::clamp<double>(speed, 1.0, 100.0);
  return 0.5 + 9.5 * (100.0 - s) / 99.0;
}

double slow_period_secs(Effect effect, std::uint8_t speed) {
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
  default:
    return period_secs(speed);
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
  const double p = period_secs(speed);
  Frame out = base;
  switch (effect) {
  case Effect::Static:
    break;
  case Effect::Breathe: {
    const double f = 0.55 + 0.45 * std::cos(kTau * t / p);
    for (Rgb &c : out) {
      c = multiply(c, f);
    }
    break;
  }
  case Effect::Wave:
    for (std::size_t i = 0; i < 4; ++i) {
      const double k = 0.5 - 0.5 * std::cos(kTau * (t / p - kPos[i] / 3.0));
      out[i] = Rgb::lerp(base[0], base[1], k);
    }
    break;
  case Effect::Cycle:
    for (std::size_t i = 0; i < 4; ++i) {
      const double h = t / p + kPos[i] / 3.0;
      out[i] = Rgb::from_hsv(h - std::floor(h), 1.0, 1.0);
    }
    break;
  case Effect::Gradient:
    for (std::size_t i = 0; i < 4; ++i) {
      const double x = 4.0 * t / p + static_cast<double>(i);
      const double fl = std::floor(x);
      const auto k = static_cast<std::size_t>(std::fmod(fl, 4.0));
      out[i] = Rgb::lerp(base[k], base[(k + 1) % 4], x - fl);
    }
    break;
  case Effect::Drift:
  case Effect::Glow:
  case Effect::Tide:
  case Effect::Dusk:
    return render_slow(effect, base, t, speed);
  }
  return out;
}

} // namespace omenkbd

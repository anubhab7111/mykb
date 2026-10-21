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
  }
  return "?";
}

std::optional<Effect> effect_from_name(std::string_view s) {
  const std::string lower = to_lower(s);
  for (Effect e : {Effect::Static, Effect::Breathe, Effect::Wave, Effect::Cycle,
                   Effect::Gradient}) {
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
  }
  return out;
}

} // namespace omenkbd

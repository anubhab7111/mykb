#include "core/color.hpp"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <format>
#include <ostream>

namespace omenkbd {

namespace {

std::uint8_t to_channel(double x) {
    return static_cast<std::uint8_t>(std::clamp(std::round(x), 0.0, 255.0));
}

} // namespace

std::expected<Rgb, std::string> Rgb::parse(std::string_view s) {
    std::string_view hex = s.starts_with('#') ? s.substr(1) : s;
    const auto bad = [s] {
        return std::unexpected(std::format("invalid color \"{}\": expected #RRGGBB", s));
    };
    if (hex.size() != 6) {
        return bad();
    }
    std::uint8_t ch[3]{};
    for (std::size_t i = 0; i < 3; ++i) {
        const char *first = hex.data() + 2 * i;
        const auto [ptr, ec] = std::from_chars(first, first + 2, ch[i], 16);
        if (ec != std::errc{} || ptr != first + 2) {
            return bad();
        }
    }
    return Rgb{ch[0], ch[1], ch[2]};
}

std::string Rgb::to_hex() const { return std::format("{:02X}{:02X}{:02X}", r, g, b); }

Rgb Rgb::scale(std::uint8_t pct) const {
    const double f = std::min<double>(pct, 100) / 100.0;
    return {to_channel(r * f), to_channel(g * f), to_channel(b * f)};
}

Rgb Rgb::lerp(Rgb a, Rgb b, double t) {
    t = std::clamp(t, 0.0, 1.0);
    const auto mix = [t](std::uint8_t x, std::uint8_t y) { return to_channel(x + (y - x) * t); };
    return {mix(a.r, b.r), mix(a.g, b.g), mix(a.b, b.b)};
}

Hsv Rgb::to_hsv() const {
    const double rf = r / 255.0, gf = g / 255.0, bf = b / 255.0;
    const double max = std::max({rf, gf, bf});
    const double min = std::min({rf, gf, bf});
    const double d = max - min;
    double h = 0.0;
    if (d > 0.0) {
        if (max == rf) {
            h = std::fmod(std::fmod((gf - bf) / d, 6.0) + 6.0, 6.0) / 6.0;
        } else if (max == gf) {
            h = ((bf - rf) / d + 2.0) / 6.0;
        } else {
            h = ((rf - gf) / d + 4.0) / 6.0;
        }
    }
    return {h, max == 0.0 ? 0.0 : d / max, max};
}

Rgb Rgb::from_hsv(double h, double s, double v) {
    s = std::clamp(s, 0.0, 1.0);
    v = std::clamp(v, 0.0, 1.0);
    const double h6 = (h - std::floor(h)) * 6.0;
    const double sector = std::floor(h6);
    const double f = h6 - sector;
    const double p = v * (1.0 - s);
    const double q = v * (1.0 - s * f);
    const double t = v * (1.0 - s * (1.0 - f));
    double rf = v, gf = p, bf = q;
    switch (static_cast<int>(sector) % 6) {
    case 0:
        rf = v, gf = t, bf = p;
        break;
    case 1:
        rf = q, gf = v, bf = p;
        break;
    case 2:
        rf = p, gf = v, bf = t;
        break;
    case 3:
        rf = p, gf = q, bf = v;
        break;
    case 4:
        rf = t, gf = p, bf = v;
        break;
    default:
        break; // sector 5: (v, p, q)
    }
    return {to_channel(rf * 255.0), to_channel(gf * 255.0), to_channel(bf * 255.0)};
}

Rgb Rgb::vivid() const {
    const Hsv hsv = to_hsv();
    if (hsv.s < 0.1) {
        return *this;
    }
    return from_hsv(hsv.h, std::max(hsv.s, 0.85), 1.0);
}

std::ostream &operator<<(std::ostream &os, Rgb c) { return os << '#' << c.to_hex(); }

} // namespace omenkbd

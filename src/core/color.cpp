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
        if (s.starts_with("theme:")) {
            return std::unexpected(
                std::format("\"{}\": theme colors were removed; write the color as #RRGGBB", s));
        }
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

std::ostream &operator<<(std::ostream &os, Rgb c) { return os << '#' << c.to_hex(); }

} // namespace omenkbd

#pragma once
// 8-bit RGB colors: parsing, formatting, scaling, blending and HSV helpers.

#include <cstdint>
#include <expected>
#include <iosfwd>
#include <string>
#include <string_view>

namespace omenkbd {

/// Hue in [0, 1), saturation and value in [0, 1].
struct Hsv {
    double h, s, v;
};

struct Rgb {
    std::uint8_t r{}, g{}, b{};

    friend constexpr bool operator==(Rgb, Rgb) = default;

    /// Parses `#RRGGBB` or `RRGGBB` (case-insensitive).
    static std::expected<Rgb, std::string> parse(std::string_view s);

    /// Uppercase `RRGGBB`, without `#`.
    [[nodiscard]] std::string to_hex() const;

    /// Scales every channel by `pct` percent (clamped to 100), rounding half away from zero.
    [[nodiscard]] Rgb scale(std::uint8_t pct) const;

    /// Linear blend from `a` (t = 0) to `b` (t = 1); `t` is clamped to [0, 1].
    static Rgb lerp(Rgb a, Rgb b, double t);

    [[nodiscard]] Hsv to_hsv() const;

    /// Inverse of `to_hsv`; `h` wraps, `s` and `v` are clamped to [0, 1].
    static Rgb from_hsv(double h, double s, double v);

    /// Keeps the hue, boosts saturation to at least 0.85 and value to 1, so pastel theme colors
    /// stay distinguishable on RGB LEDs. Near-greys (saturation < 0.1) are returned unchanged.
    [[nodiscard]] Rgb vivid() const;
};

inline constexpr Rgb kBlack{0, 0, 0};
inline constexpr Rgb kWhite{255, 255, 255};

/// Prints `#RRGGBB` (used by GoogleTest failure messages).
std::ostream &operator<<(std::ostream &os, Rgb c);

} // namespace omenkbd

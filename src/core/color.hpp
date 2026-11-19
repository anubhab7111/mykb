#pragma once
// 8-bit RGB colors: parsing, formatting, scaling, and blending.

#include <cstdint>
#include <expected>
#include <iosfwd>
#include <string>
#include <string_view>

namespace omenkbd {

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
};

inline constexpr Rgb kBlack{0, 0, 0};
inline constexpr Rgb kWhite{255, 255, 255};

/// Prints `#RRGGBB` (used by GoogleTest failure messages).
std::ostream &operator<<(std::ostream &os, Rgb c);

} // namespace omenkbd

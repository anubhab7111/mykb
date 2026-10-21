#pragma once
// Keyboard zones. Software always uses the logical order [left, middle, right, wasd]; ZoneMap
// translates a logical Frame into the firmware's hardware zone order.

#include "core/color.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <optional>
#include <string>
#include <string_view>

namespace omenkbd {

enum class Zone : std::uint8_t { Left, Middle, Right, Wasd };

inline constexpr std::array<Zone, 4> kAllZones{Zone::Left, Zone::Middle, Zone::Right, Zone::Wasd};

/// Logical index 0..3.
constexpr std::size_t index(Zone z) { return static_cast<std::size_t>(z); }

std::string_view name(Zone z);

/// Case-insensitive; `center` is accepted as an alias of `middle`.
std::optional<Zone> zone_from_name(std::string_view s);

/// Four colors in logical zone order [left, middle, right, wasd].
using Frame = std::array<Rgb, 4>;

/// Logical zone → hardware zone index; a permutation of 0..3.
class ZoneMap {
  public:
    /// left→2, middle→1, right→0, wasd→3 (omen-space's classic mapping and the upstream order).
    ZoneMap() = default;

    static std::expected<ZoneMap, std::string> make(std::size_t left, std::size_t middle,
                                                    std::size_t right, std::size_t wasd);

    /// Reorders a logical frame into hardware order.
    [[nodiscard]] std::array<Rgb, 4> to_hw(const Frame &f) const;

    friend bool operator==(const ZoneMap &, const ZoneMap &) = default;

  private:
    explicit ZoneMap(std::array<std::size_t, 4> hw) : hw_(hw) {}
    std::array<std::size_t, 4> hw_{2, 1, 0, 3};
};

} // namespace omenkbd

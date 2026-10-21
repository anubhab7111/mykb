#include "core/zones.hpp"

#include "util/strings.hpp"

#include <format>

namespace omenkbd {

std::string_view name(Zone z) {
    switch (z) {
    case Zone::Left:
        return "left";
    case Zone::Middle:
        return "middle";
    case Zone::Right:
        return "right";
    case Zone::Wasd:
        return "wasd";
    }
    return "?";
}

std::optional<Zone> zone_from_name(std::string_view s) {
    const std::string lower = to_lower(s);
    if (lower == "left") {
        return Zone::Left;
    }
    if (lower == "middle" || lower == "center") {
        return Zone::Middle;
    }
    if (lower == "right") {
        return Zone::Right;
    }
    if (lower == "wasd") {
        return Zone::Wasd;
    }
    return std::nullopt;
}

std::expected<ZoneMap, std::string> ZoneMap::make(std::size_t left, std::size_t middle,
                                                  std::size_t right, std::size_t wasd) {
    const std::array<std::size_t, 4> hw{left, middle, right, wasd};
    std::array<bool, 4> seen{};
    for (std::size_t i : hw) {
        if (i > 3 || seen[i]) {
            return std::unexpected(
                std::format("zone_map must use each hardware index 0-3 exactly once, got [{}, {}, "
                            "{}, {}]",
                            left, middle, right, wasd));
        }
        seen[i] = true;
    }
    return ZoneMap(hw);
}

std::array<Rgb, 4> ZoneMap::to_hw(const Frame &f) const {
    std::array<Rgb, 4> out{};
    for (std::size_t logical = 0; logical < 4; ++logical) {
        out[hw_[logical]] = f[logical];
    }
    return out;
}

} // namespace omenkbd

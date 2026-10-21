#pragma once
// Declarative rules: conditions over system facts that switch the profile, cap brightness, or add
// an overlay. Evaluation is first-match per category.

#include "core/palette.hpp"
#include "core/zones.hpp"

#include <cstdint>
#include <expected>
#include <optional>
#include <span>
#include <string>
#include <toml++/toml.hpp>
#include <variant>
#include <vector>

namespace omenkbd {

/// What the daemon currently knows about the system. nullopt = not known (yet).
struct Facts {
    std::optional<std::string> power_profile;
    std::optional<bool> on_battery;
    std::optional<double> battery_pct;
    std::optional<double> cpu_temp_c;
    bool gamemode = false;
    bool locked = false;
    bool idle = false;

    friend bool operator==(const Facts &, const Facts &) = default;
};

/// All present fields must hold. A condition on an unknown (nullopt) fact does not hold.
struct Condition {
    std::optional<std::string> power_profile;
    std::optional<bool> on_battery;
    std::optional<double> battery_below;
    std::optional<double> cpu_temp_above;
    std::optional<bool> gamemode;
    std::optional<bool> locked;
    std::optional<bool> idle;

    [[nodiscard]] bool holds(const Facts &f) const;
};

struct OverlayCfg {
    std::vector<Zone> zones;
    ColorRef color = ColorRef::hex(kBlack);
    bool pulse = false;
};

struct TintCfg {
    ColorRef color = ColorRef::hex(kBlack);
    double strength = 0.0;
};

struct Rule {
    std::string name;
    Condition when;
    std::optional<std::string> profile;
    std::optional<std::uint8_t> brightness_cap;
    std::optional<OverlayCfg> overlay;
    std::optional<TintCfg> tint;

    /// Not both overlay and tint; at least one action; brightness_cap ≤ 100; strength in [0, 1];
    /// overlays name at least one zone.
    [[nodiscard]] std::expected<void, std::string> validate() const;
};

/// Parses one `[[rules]]` table. Unknown keys (here or in `when`/`overlay`/`tint`) are errors.
std::expected<Rule, std::string> parse_rule(const toml::table &t);

/// An overlay chosen by the rules, before its colors are resolved against the palette.
struct ZonesOverlay {
    std::vector<Zone> zones;
    ColorRef color;
    bool pulse;
    friend bool operator==(const ZonesOverlay &, const ZonesOverlay &) = default;
};

struct TintOverlay {
    ColorRef color;
    double strength;
    friend bool operator==(const TintOverlay &, const TintOverlay &) = default;
};

using Overlay = std::variant<ZonesOverlay, TintOverlay>;

struct RuleOutcome {
    std::optional<std::string> profile;
    std::optional<std::uint8_t> brightness_cap;
    std::optional<Overlay> overlay;
    /// Names of the rules that contributed to this outcome, in rule order.
    std::vector<std::string> active;
};

/// First matching rule wins per category: profile, brightness cap, overlay (overlay or tint).
RuleOutcome evaluate(std::span<const Rule> rules, const Facts &facts);

} // namespace omenkbd

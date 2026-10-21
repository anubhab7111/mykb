#pragma once
// User configuration, `~/.config/omen-kbd/config.toml`. The daemon only ever reads this file.

#include "core/effects.hpp"
#include "core/palette.hpp"
#include "core/rules.hpp"
#include "core/zones.hpp"

#include <cstdint>
#include <expected>
#include <filesystem>
#include <map>
#include <string>
#include <string_view>
#include <vector>

namespace omenkbd {

/// Profile name reserved for colors set with `omen-kbd set`.
inline constexpr std::string_view kCustomProfile = "custom";

struct DeviceCfg {
    ZoneMap zone_map;
};

struct EffectsCfg {
    bool enabled = false;
    std::uint8_t fps = 10;
};

struct ThemeCfg {
    ThemeColors colors = ThemeColors::Vivid;
    /// Directory holding the current theme's files (`~` already expanded).
    std::filesystem::path dir;
};

enum class GateAction { Off, Dim, Keep };

struct GatesCfg {
    GateAction on_lock = GateAction::Off;
    GateAction on_idle = GateAction::Dim;
    std::uint8_t dim_brightness = 20;
    bool pause_effects_on_battery = true;
};

struct HintsCfg {
    std::uint64_t timeout_s = 15;
    std::uint64_t delay_ms = 250;
    /// Keys are normalized modifier sets (see normalize_mods).
    std::map<std::string, ColorRef, std::less<>> modifiers;
    std::map<std::string, ColorRef, std::less<>> submaps;
};

struct ProfileCfg {
    std::string name;
    /// One color (all zones) or four (left, middle, right, wasd).
    std::vector<ColorRef> colors;
    Effect effect = Effect::Static;
    std::uint8_t speed = 50;
};

struct Config {
    DeviceCfg device;
    EffectsCfg effects;
    ThemeCfg theme;
    GatesCfg gates;
    HintsCfg hints;
    std::vector<ProfileCfg> profiles;
    std::vector<Rule> rules;

    [[nodiscard]] const ProfileCfg *profile(std::string_view name) const;
    /// True if any rule needs the CPU temperature (so the hwmon poller should run).
    [[nodiscard]] bool needs_cpu_temp() const;
};

/// Parses and validates a config. Error messages name the offending field.
std::expected<Config, std::string> parse_config(std::string_view text);

/// Loads `path`; a missing file yields embedded_default(). Errors are prefixed with the path.
std::expected<Config, std::string> load_config(const std::filesystem::path &path);

/// The shipped example config (packaging/config.example.toml).
Config embedded_default();

/// `$XDG_CONFIG_HOME/omen-kbd/config.toml` (default `~/.config/omen-kbd/config.toml`).
std::filesystem::path config_path();

/// Normalizes a modifier set like "Shift+SUPER" to "super+shift": case-insensitive,
/// `+`-separated, deduplicated, ordered super, ctrl, alt, shift.
std::expected<std::string, std::string> normalize_mods(std::string_view s);

/// Expands a leading `~` to $HOME.
std::filesystem::path expand_home(std::string_view p);

} // namespace omenkbd

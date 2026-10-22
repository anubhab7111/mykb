#include "core/rules.hpp"

#include "util/toml_read.hpp"

#include <format>

namespace omenkbd {

namespace {

using tomlr::check_keys;
using tomlr::opt;

/// Assigns `*out` from an `expected<optional<T>>`, or returns its error.
template <class T, class Out>
std::expected<void, std::string> take(std::expected<std::optional<T>, std::string> v, Out &out) {
    if (!v) {
        return std::unexpected(v.error());
    }
    if (*v) {
        out = **v; // NOLINT(bugprone-optional-value-conversion): Out is T or std::optional<T>
    }
    return {};
}

#define OMENKBD_TRY(expr)                                                                          \
    do {                                                                                           \
        if (auto _r = (expr); !_r) {                                                               \
            return std::unexpected(_r.error());                                                    \
        }                                                                                          \
    } while (false)

std::expected<Condition, std::string> parse_condition(const toml::table &t,
                                                      std::string_view where) {
    OMENKBD_TRY(check_keys(t,
                           {"power_profile", "on_battery", "battery_below", "cpu_temp_above",
                            "gamemode", "locked", "idle"},
                           where));
    Condition c;
    OMENKBD_TRY(take(opt<std::string>(t, "power_profile", where), c.power_profile));
    OMENKBD_TRY(take(opt<bool>(t, "on_battery", where), c.on_battery));
    OMENKBD_TRY(take(opt<double>(t, "battery_below", where), c.battery_below));
    OMENKBD_TRY(take(opt<double>(t, "cpu_temp_above", where), c.cpu_temp_above));
    OMENKBD_TRY(take(opt<bool>(t, "gamemode", where), c.gamemode));
    OMENKBD_TRY(take(opt<bool>(t, "locked", where), c.locked));
    OMENKBD_TRY(take(opt<bool>(t, "idle", where), c.idle));
    return c;
}

std::expected<ColorRef, std::string> parse_color(const toml::table &t, std::string_view where) {
    std::optional<std::string> s;
    OMENKBD_TRY(take(opt<std::string>(t, "color", where), s));
    if (!s) {
        return std::unexpected(std::format("{}: missing `color`", where));
    }
    auto c = ColorRef::parse(*s);
    if (!c) {
        return std::unexpected(std::format("{}: {}", where, c.error()));
    }
    return c;
}

std::expected<OverlayCfg, std::string> parse_overlay(const toml::table &t, std::string_view where) {
    OMENKBD_TRY(check_keys(t, {"zones", "color", "pulse"}, where));
    OverlayCfg o;
    if (const toml::array *zones = t["zones"].as_array()) {
        for (const toml::node &z : *zones) {
            const auto name_str = z.value<std::string>();
            const auto zone = name_str ? zone_from_name(*name_str) : std::nullopt;
            if (!zone) {
                return std::unexpected(
                    std::format("{}: unknown zone {} (expected left, middle, right or wasd)", where,
                                name_str ? "\"" + *name_str + "\"" : std::string("value")));
            }
            o.zones.push_back(*zone);
        }
    } else if (t.contains("zones")) {
        return std::unexpected(std::format("{}: `zones` must be a list", where));
    }
    auto color = parse_color(t, where);
    if (!color) {
        return std::unexpected(color.error());
    }
    o.color = *color;
    OMENKBD_TRY(take(opt<bool>(t, "pulse", where), o.pulse));
    return o;
}

std::expected<TintCfg, std::string> parse_tint(const toml::table &t, std::string_view where) {
    OMENKBD_TRY(check_keys(t, {"color", "strength"}, where));
    TintCfg tint;
    auto color = parse_color(t, where);
    if (!color) {
        return std::unexpected(color.error());
    }
    tint.color = *color;
    std::optional<double> strength;
    OMENKBD_TRY(take(opt<double>(t, "strength", where), strength));
    if (!strength) {
        return std::unexpected(std::format("{}: missing `strength`", where));
    }
    tint.strength = *strength;
    return tint;
}

} // namespace

bool Condition::holds(const Facts &f) const {
    if (power_profile && f.power_profile != *power_profile) {
        return false;
    }
    if (on_battery && f.on_battery != *on_battery) {
        return false;
    }
    if (battery_below && !(f.battery_pct && *f.battery_pct < *battery_below)) {
        return false;
    }
    if (cpu_temp_above && !(f.cpu_temp_c && *f.cpu_temp_c > *cpu_temp_above)) {
        return false;
    }
    const auto flag = [](const std::optional<bool> &want, bool have) {
        return !want || *want == have;
    };
    return flag(gamemode, f.gamemode) && flag(locked, f.locked) && flag(idle, f.idle);
}

std::expected<void, std::string> Rule::validate() const {
    if (overlay && tint) {
        return std::unexpected(
            std::format("rule \"{}\": use either overlay or tint, not both", name));
    }
    if (!profile && !brightness_cap && !overlay && !tint) {
        return std::unexpected(std::format(
            "rule \"{}\": needs an action (profile, brightness_cap, overlay or tint)", name));
    }
    if (brightness_cap && *brightness_cap > 100) {
        return std::unexpected(std::format("rule \"{}\": brightness_cap must be 0-100, got {}",
                                           name, *brightness_cap));
    }
    if (tint && (tint->strength < 0.0 || tint->strength > 1.0)) {
        return std::unexpected(std::format(
            "rule \"{}\": tint strength must be between 0 and 1, got {}", name, tint->strength));
    }
    if (overlay && overlay->zones.empty()) {
        return std::unexpected(std::format("rule \"{}\": overlay needs at least one zone", name));
    }
    return {};
}

std::expected<Rule, std::string> parse_rule(const toml::table &t) {
    Rule r;
    std::optional<std::string> name;
    OMENKBD_TRY(take(opt<std::string>(t, "name", "rules"), name));
    if (!name) {
        return std::unexpected(std::string("rules: every rule needs a `name`"));
    }
    r.name = *name;
    const std::string where = std::format("rule \"{}\"", r.name);
    OMENKBD_TRY(
        check_keys(t, {"name", "when", "profile", "brightness_cap", "overlay", "tint"}, where));
    if (const toml::table *when = t["when"].as_table()) {
        auto c = parse_condition(*when, where + " when");
        if (!c) {
            return std::unexpected(c.error());
        }
        r.when = *c;
    } else if (t.contains("when")) {
        return std::unexpected(where + ": `when` must be a table");
    }
    OMENKBD_TRY(take(opt<std::string>(t, "profile", where), r.profile));
    std::optional<std::int64_t> cap;
    OMENKBD_TRY(take(tomlr::opt_int_in(t, "brightness_cap", where, 0, 255), cap));
    if (cap) {
        r.brightness_cap = static_cast<std::uint8_t>(*cap);
    }
    if (const toml::table *o = t["overlay"].as_table()) {
        auto ov = parse_overlay(*o, where + " overlay");
        if (!ov) {
            return std::unexpected(ov.error());
        }
        r.overlay = *ov;
    } else if (t.contains("overlay")) {
        return std::unexpected(where + ": `overlay` must be a table");
    }
    if (const toml::table *ti = t["tint"].as_table()) {
        auto tn = parse_tint(*ti, where + " tint");
        if (!tn) {
            return std::unexpected(tn.error());
        }
        r.tint = *tn;
    } else if (t.contains("tint")) {
        return std::unexpected(where + ": `tint` must be a table");
    }
    return r;
}

RuleOutcome evaluate(std::span<const Rule> rules, const Facts &facts) {
    RuleOutcome out;
    for (const Rule &r : rules) {
        if (!r.when.holds(facts)) {
            continue;
        }
        bool contributed = false;
        if (!out.profile && r.profile) {
            out.profile = r.profile;
            contributed = true;
        }
        if (!out.brightness_cap && r.brightness_cap) {
            out.brightness_cap = r.brightness_cap;
            contributed = true;
        }
        if (!out.overlay) {
            if (r.overlay) {
                out.overlay = ZonesOverlay{r.overlay->zones, r.overlay->color, r.overlay->pulse};
                contributed = true;
            } else if (r.tint) {
                out.overlay = TintOverlay{r.tint->color, r.tint->strength};
                contributed = true;
            }
        }
        if (contributed) {
            out.active.push_back(r.name);
        }
    }
    return out;
}

} // namespace omenkbd

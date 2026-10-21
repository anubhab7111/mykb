#include "config.hpp"

#include "config_example.hpp"
#include "util/strings.hpp"
#include "util/toml_read.hpp"

#include <algorithm>
#include <array>
#include <cstdlib>
#include <format>
#include <fstream>
#include <iterator>

namespace omenkbd {

namespace {

using tomlr::check_keys;
using tomlr::opt;
using tomlr::opt_int_in;

constexpr std::array<std::string_view, 4> kModifierOrder{"super", "ctrl", "alt", "shift"};

using Err = std::unexpected<std::string>;

#define OMENKBD_TRY(expr)                                                                          \
    do {                                                                                           \
        if (auto _r = (expr); !_r) {                                                               \
            return Err(_r.error());                                                                \
        }                                                                                          \
    } while (false)

/// `t[key]` as a table; missing ⇒ nullptr; present but not a table ⇒ error.
std::expected<const toml::table *, std::string> sub_table(const toml::table &t,
                                                          std::string_view key) {
    const toml::node *n = t.get(key);
    if (n == nullptr) {
        return nullptr;
    }
    if (!n->is_table()) {
        return Err(std::format("`{}` must be a table", key));
    }
    return n->as_table();
}

std::expected<GateAction, std::string> gate_action(std::string_view s, std::string_view field) {
    const std::string v = to_lower(s);
    if (v == "off") {
        return GateAction::Off;
    }
    if (v == "dim") {
        return GateAction::Dim;
    }
    if (v == "keep") {
        return GateAction::Keep;
    }
    return Err(std::format("gates.{} must be \"off\", \"dim\" or \"keep\", got \"{}\"", field, s));
}

std::expected<void, std::string> parse_device(const toml::table &t, DeviceCfg &out) {
    OMENKBD_TRY(check_keys(t, {"zone_map"}, "device"));
    auto zm = sub_table(t, "zone_map");
    if (!zm) {
        return Err("device: " + zm.error());
    }
    if (*zm == nullptr) {
        return {};
    }
    const toml::table &m = **zm;
    OMENKBD_TRY(check_keys(m, {"left", "middle", "right", "wasd"}, "device.zone_map"));
    std::array<std::size_t, 4> idx{};
    std::size_t i = 0;
    for (std::string_view k : {"left", "middle", "right", "wasd"}) {
        auto v = opt_int_in(m, k, "device.zone_map", 0, 3);
        if (!v) {
            return Err(v.error());
        }
        if (!*v) {
            return Err(std::format("device.zone_map: missing `{}`", k));
        }
        idx[i++] = static_cast<std::size_t>(**v);
    }
    auto map = ZoneMap::make(idx[0], idx[1], idx[2], idx[3]);
    if (!map) {
        return Err("device: " + map.error());
    }
    out.zone_map = *map;
    return {};
}

std::expected<void, std::string> parse_effects(const toml::table &t, EffectsCfg &out) {
    OMENKBD_TRY(check_keys(t, {"enabled", "fps"}, "effects"));
    auto enabled = opt<bool>(t, "enabled", "effects");
    if (!enabled) {
        return Err(enabled.error());
    }
    out.enabled = enabled->value_or(out.enabled);
    auto fps = opt_int_in(t, "fps", "effects.fps", 1, 20);
    if (!fps) {
        return Err(fps.error());
    }
    if (*fps) {
        out.fps = static_cast<std::uint8_t>(**fps);
    }
    return {};
}

std::expected<void, std::string> parse_theme(const toml::table &t, ThemeCfg &out,
                                             std::string &dir) {
    OMENKBD_TRY(check_keys(t, {"colors", "dir"}, "theme"));
    auto colors = opt<std::string>(t, "colors", "theme");
    if (!colors) {
        return Err(colors.error());
    }
    if (*colors) {
        const auto mode = theme_colors_from_name(**colors);
        if (!mode) {
            return Err(
                std::format("theme.colors must be \"vivid\" or \"exact\", got \"{}\"", **colors));
        }
        out.colors = *mode;
    }
    auto d = opt<std::string>(t, "dir", "theme");
    if (!d) {
        return Err(d.error());
    }
    if (*d) {
        dir = **d;
    }
    return {};
}

std::expected<void, std::string> parse_gates(const toml::table &t, GatesCfg &out) {
    OMENKBD_TRY(check_keys(t, {"on_lock", "on_idle", "dim_brightness", "pause_effects_on_battery"},
                           "gates"));
    for (auto [key, target] : {std::pair{"on_lock", &out.on_lock}, {"on_idle", &out.on_idle}}) {
        auto s = opt<std::string>(t, key, "gates");
        if (!s) {
            return Err(s.error());
        }
        if (*s) {
            auto a = gate_action(**s, key);
            if (!a) {
                return Err(a.error());
            }
            *target = *a;
        }
    }
    auto dim = opt_int_in(t, "dim_brightness", "gates.dim_brightness", 0, 100);
    if (!dim) {
        return Err(dim.error());
    }
    if (*dim) {
        out.dim_brightness = static_cast<std::uint8_t>(**dim);
    }
    auto pause = opt<bool>(t, "pause_effects_on_battery", "gates");
    if (!pause) {
        return Err(pause.error());
    }
    out.pause_effects_on_battery = pause->value_or(out.pause_effects_on_battery);
    return {};
}

std::expected<std::map<std::string, ColorRef, std::less<>>, std::string>
parse_color_map(const toml::table &t, std::string_view where, bool modifier_keys) {
    std::map<std::string, ColorRef, std::less<>> out;
    for (const auto &[key, node] : t) {
        std::string k(key.str());
        if (modifier_keys) {
            auto n = normalize_mods(k);
            if (!n) {
                return Err(std::format("{}: {}", where, n.error()));
            }
            k = *n;
        }
        const auto s = node.value<std::string>();
        if (!s) {
            return Err(std::format("{}.{} must be a color string", where, key.str()));
        }
        auto c = ColorRef::parse(*s);
        if (!c) {
            return Err(std::format("{}.{}: {}", where, key.str(), c.error()));
        }
        out.insert_or_assign(k, *c);
    }
    return out;
}

std::expected<void, std::string> parse_hints(const toml::table &t, HintsCfg &out) {
    OMENKBD_TRY(check_keys(t, {"timeout_s", "delay_ms", "modifiers", "submaps"}, "hints"));
    auto timeout = opt_int_in(t, "timeout_s", "hints.timeout_s", 1, 3600);
    if (!timeout) {
        return Err(timeout.error());
    }
    if (*timeout) {
        out.timeout_s = static_cast<std::uint64_t>(**timeout);
    }
    auto delay = opt_int_in(t, "delay_ms", "hints.delay_ms", 0, 10000);
    if (!delay) {
        return Err(delay.error());
    }
    if (*delay) {
        out.delay_ms = static_cast<std::uint64_t>(**delay);
    }
    for (auto [key, target, mods] :
         {std::tuple{"modifiers", &out.modifiers, true}, {"submaps", &out.submaps, false}}) {
        auto sub = sub_table(t, key);
        if (!sub) {
            return Err("hints: " + sub.error());
        }
        if (*sub != nullptr) {
            auto m = parse_color_map(**sub, std::format("hints.{}", key), mods);
            if (!m) {
                return Err(m.error());
            }
            *target = std::move(*m);
        }
    }
    return {};
}

std::expected<ProfileCfg, std::string> parse_profile(const toml::table &t) {
    ProfileCfg p;
    auto name = opt<std::string>(t, "name", "profiles");
    if (!name) {
        return Err(name.error());
    }
    if (!*name || trim(**name).empty()) {
        return Err(std::string("profiles: name must not be empty"));
    }
    p.name = **name;
    const std::string where = std::format("profile \"{}\"", p.name);
    OMENKBD_TRY(check_keys(t, {"name", "colors", "effect", "speed"}, where));
    const toml::array *colors = t["colors"].as_array();
    if (colors == nullptr) {
        return Err(where + ": colors must be a list of 1 or 4 colors");
    }
    for (const toml::node &n : *colors) {
        const auto s = n.value<std::string>();
        auto c = s ? ColorRef::parse(*s) : Err(std::string("expected a color string"));
        if (!c) {
            return Err(std::format("{}: colors: {}", where, c.error()));
        }
        p.colors.push_back(*c);
    }
    if (p.colors.size() != 1 && p.colors.size() != 4) {
        return Err(
            std::format("{}: colors must list 1 or 4 colors (left, middle, right, wasd), got {}",
                        where, p.colors.size()));
    }
    auto effect = opt<std::string>(t, "effect", where);
    if (!effect) {
        return Err(effect.error());
    }
    if (*effect) {
        const auto e = effect_from_name(**effect);
        if (!e) {
            return Err(std::format("{}: unknown effect \"{}\" (expected static, breathe, wave, "
                                   "cycle or gradient)",
                                   where, **effect));
        }
        p.effect = *e;
    }
    auto speed = opt_int_in(t, "speed", where + " speed", 1, 100);
    if (!speed) {
        return Err(speed.error());
    }
    if (*speed) {
        p.speed = static_cast<std::uint8_t>(**speed);
    }
    return p;
}

std::optional<std::string> read_text(const std::filesystem::path &p) {
    std::ifstream f(p, std::ios::binary);
    if (!f) {
        return std::nullopt;
    }
    return std::string(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
}

} // namespace

std::expected<Config, std::string> parse_config(std::string_view text) {
    toml::table root;
    try {
        root = toml::parse(text);
    } catch (const toml::parse_error &e) {
        return Err(
            std::format("TOML error at line {}: {}", e.source().begin.line, e.description()));
    }
    OMENKBD_TRY(check_keys(
        root, {"device", "effects", "theme", "gates", "hints", "profiles", "rules"}, ""));

    Config c;
    std::string theme_dir = "~/.config/theming/current";
    const auto section = [&](std::string_view key) { return sub_table(root, key); };

    if (auto s = section("device"); !s) {
        return Err(s.error());
    } else if (*s != nullptr) {
        OMENKBD_TRY(parse_device(**s, c.device));
    }
    if (auto s = section("effects"); !s) {
        return Err(s.error());
    } else if (*s != nullptr) {
        OMENKBD_TRY(parse_effects(**s, c.effects));
    }
    if (auto s = section("theme"); !s) {
        return Err(s.error());
    } else if (*s != nullptr) {
        OMENKBD_TRY(parse_theme(**s, c.theme, theme_dir));
    }
    c.theme.dir = expand_home(theme_dir);
    if (auto s = section("gates"); !s) {
        return Err(s.error());
    } else if (*s != nullptr) {
        OMENKBD_TRY(parse_gates(**s, c.gates));
    }
    if (auto s = section("hints"); !s) {
        return Err(s.error());
    } else if (*s != nullptr) {
        OMENKBD_TRY(parse_hints(**s, c.hints));
    }

    if (const toml::array *profiles = root["profiles"].as_array()) {
        for (const toml::node &n : *profiles) {
            if (!n.is_table()) {
                return Err(std::string("profiles: each entry must be a table"));
            }
            auto p = parse_profile(*n.as_table());
            if (!p) {
                return Err(p.error());
            }
            if (p->name == kCustomProfile) {
                return Err(std::format("profile name \"{}\" is reserved for `omen-kbd set`",
                                       kCustomProfile));
            }
            if (c.profile(p->name) != nullptr) {
                return Err(std::format("duplicate profile name \"{}\"", p->name));
            }
            c.profiles.push_back(std::move(*p));
        }
    }
    if (c.profiles.empty()) {
        return Err(std::string("config needs at least one [[profiles]] entry"));
    }

    if (const toml::array *rules = root["rules"].as_array()) {
        for (const toml::node &n : *rules) {
            if (!n.is_table()) {
                return Err(std::string("rules: each entry must be a table"));
            }
            auto r = parse_rule(*n.as_table());
            if (!r) {
                return Err(r.error());
            }
            OMENKBD_TRY(r->validate());
            if (r->profile && c.profile(*r->profile) == nullptr) {
                return Err(
                    std::format("rule \"{}\": unknown profile \"{}\"", r->name, *r->profile));
            }
            c.rules.push_back(std::move(*r));
        }
    }
    return c;
}

std::expected<Config, std::string> load_config(const std::filesystem::path &path) {
    std::error_code ec;
    if (!std::filesystem::exists(path, ec)) {
        return embedded_default();
    }
    const auto text = read_text(path);
    if (!text) {
        return Err(std::format("{}: cannot read", path.string()));
    }
    auto c = parse_config(*text);
    if (!c) {
        return Err(std::format("{}: {}", path.string(), c.error()));
    }
    return c;
}

Config embedded_default() { return parse_config(kConfigExample).value(); }

std::filesystem::path config_path() {
    const char *xdg = std::getenv("XDG_CONFIG_HOME");
    const std::filesystem::path base =
        (xdg != nullptr && *xdg != '\0') ? std::filesystem::path(xdg) : expand_home("~/.config");
    return base / "omen-kbd/config.toml";
}

std::expected<std::string, std::string> normalize_mods(std::string_view s) {
    std::array<bool, 4> present{};
    std::size_t start = 0;
    while (start <= s.size()) {
        const std::size_t plus = s.find('+', start);
        const std::string part =
            to_lower(trim(s.substr(start, plus == std::string_view::npos ? s.npos : plus - start)));
        const auto it = std::ranges::find(kModifierOrder, part);
        if (it == kModifierOrder.end()) {
            return Err(std::format(
                "unknown modifier \"{}\" in \"{}\" (use super, ctrl, alt, shift joined by +)", part,
                s));
        }
        present[static_cast<std::size_t>(it - kModifierOrder.begin())] = true;
        if (plus == std::string_view::npos) {
            break;
        }
        start = plus + 1;
    }
    std::string out;
    for (std::size_t i = 0; i < kModifierOrder.size(); ++i) {
        if (present[i]) {
            if (!out.empty()) {
                out += '+';
            }
            out += kModifierOrder[i];
        }
    }
    return out;
}

std::filesystem::path expand_home(std::string_view p) {
    const char *home_env = std::getenv("HOME");
    const std::filesystem::path home = home_env != nullptr ? home_env : "";
    if (p == "~") {
        return home;
    }
    if (p.starts_with("~/")) {
        return home / p.substr(2);
    }
    return std::filesystem::path(p);
}

const ProfileCfg *Config::profile(std::string_view name) const {
    const auto it = std::ranges::find(profiles, name, &ProfileCfg::name);
    return it == profiles.end() ? nullptr : &*it;
}

bool Config::needs_cpu_temp() const {
    return std::ranges::any_of(rules,
                               [](const Rule &r) { return r.when.cpu_temp_above.has_value(); });
}

} // namespace omenkbd

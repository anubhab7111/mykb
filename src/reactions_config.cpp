#include "reactions_config.hpp"

#include "util/strings.hpp"
#include "util/toml_read.hpp"

#include <cstdint>
#include <format>
#include <optional>
#include <string_view>
#include <utility>

namespace omenkbd {

namespace {

using tomlr::check_keys;
using tomlr::opt;
using tomlr::opt_int_in;

using Err = std::unexpected<std::string>;

#define OMENKBD_TRY(expr)                                                                          \
    do {                                                                                           \
        if (auto _r = (expr); !_r) {                                                               \
            return Err(_r.error());                                                                \
        }                                                                                          \
    } while (false)

/// `t[key]` as a table; missing => nullptr; present but not a table => error.
std::expected<const toml::table *, std::string>
sub_table(const toml::table &t, std::string_view key, std::string_view where) {
    const toml::node *n = t.get(key);
    if (n == nullptr) {
        return nullptr;
    }
    if (!n->is_table()) {
        return Err(std::format("{}: `{}` must be a table", where, key));
    }
    return n->as_table();
}

/// Reads the integer `key` into `out` when present; it must lie in [lo, hi], so a value that would
/// wrap or overflow `T` is rejected before it is narrowed.
template <class T>
std::expected<void, std::string> read_int(const toml::table &t, std::string_view key,
                                          std::string_view where, std::int64_t lo, std::int64_t hi,
                                          T &out) {
    const auto v = opt_int_in(t, key, where, lo, hi);
    if (!v) {
        return Err(v.error());
    }
    if (*v) {
        out = static_cast<T>(**v);
    }
    return {};
}

/// Reads the number `key` into `out` when present; it must lie in [0, 1]. NaN is rejected too.
std::expected<void, std::string> read_unit(const toml::table &t, std::string_view key,
                                           std::string_view where, double &out) {
    const auto v = opt<double>(t, key, where);
    if (!v) {
        return Err(v.error());
    }
    if (*v) {
        if (!(**v >= 0.0 && **v <= 1.0)) {
            return Err(std::format("{}: `{}` must be 0-1, got {}", where, key, **v));
        }
        out = **v;
    }
    return {};
}

std::expected<void, std::string> read_bool(const toml::table &t, std::string_view key,
                                           std::string_view where, bool &out) {
    const auto v = opt<bool>(t, key, where);
    if (!v) {
        return Err(v.error());
    }
    out = v->value_or(out);
    return {};
}

/// Reads the color `key` into `out` when present.
std::expected<void, std::string> read_color(const toml::table &t, std::string_view key,
                                            std::string_view where, ColorRef &out) {
    const auto s = opt<std::string>(t, key, where);
    if (!s) {
        return Err(s.error());
    }
    if (*s) {
        auto c = ColorRef::parse(**s);
        if (!c) {
            return Err(std::format("{}: `{}`: {}", where, key, c.error()));
        }
        out = std::move(*c);
    }
    return {};
}

/// Reads the list of zone names `key` into `out` when present; `[]` is allowed and turns it off.
std::expected<void, std::string> read_zones(const toml::table &t, std::string_view key,
                                            std::string_view where, std::vector<Zone> &out) {
    const toml::node *n = t.get(key);
    if (n == nullptr) {
        return {};
    }
    const toml::array *list = n->as_array();
    if (list == nullptr) {
        return Err(std::format("{}: `{}` must be a list", where, key));
    }
    std::vector<Zone> zones;
    for (const toml::node &z : *list) {
        const auto text = z.value<std::string>();
        const auto zone = text ? zone_from_name(*text) : std::nullopt;
        if (!zone) {
            return Err(
                std::format("{}: `{}`: unknown zone {} (expected left, middle, right or wasd)",
                            where, key, text ? "\"" + *text + "\"" : std::string("value")));
        }
        zones.push_back(*zone);
    }
    out = std::move(zones);
    return {};
}

std::expected<void, std::string> parse_workspace(const toml::table &t,
                                                 ReactionsCfg::Workspace &out) {
    constexpr std::string_view where = "reactions.workspace";
    OMENKBD_TRY(check_keys(t, {"color", "wasd_with"}, where));
    OMENKBD_TRY(read_color(t, "color", where, out.color));
    const auto with = opt<std::string>(t, "wasd_with", where);
    if (!with) {
        return Err(with.error());
    }
    if (*with) {
        const std::string v = to_lower(**with);
        if (v == "left") {
            out.wasd_with = WasdWith::Left;
        } else if (v == "middle") {
            out.wasd_with = WasdWith::Middle;
        } else if (v == "none") {
            out.wasd_with = WasdWith::None;
        } else {
            return Err(
                std::format("{}: `wasd_with` must be \"left\", \"middle\" or \"none\", got \"{}\"",
                            where, **with));
        }
    }
    return {};
}

std::expected<void, std::string> parse_window(const toml::table &t, ReactionsCfg::Window &out) {
    constexpr std::string_view where = "reactions.window";
    OMENKBD_TRY(check_keys(t, {"open", "close", "cooldown_ms"}, where));
    OMENKBD_TRY(read_unit(t, "open", where, out.open));
    OMENKBD_TRY(read_unit(t, "close", where, out.close));
    OMENKBD_TRY(read_int(t, "cooldown_ms", where, 0, 10000, out.cooldown_ms));
    return {};
}

/// The whole dotted key when `node` is the table an unquoted `com.example.app = ...` was split
/// into (TOML reads that line as the tables com -> example -> app; with several keys under one
/// prefix, the first in key order), otherwise "". Such a table starts exactly where its key does,
/// which an inline table and a `[apps.zapzap]` header, also tables, do not.
std::string unquoted_dotted_key(const toml::key &key, const toml::node &node) {
    const toml::table *tbl = node.as_table();
    if (tbl == nullptr || tbl->is_inline() || tbl->source().begin != key.source().begin) {
        return {};
    }
    std::string dotted(key.str());
    while (tbl != nullptr && !tbl->empty()) {
        const auto first = tbl->begin(); // `*tbl->begin()` would refer into a dead iterator
        const auto &[part, child] = *first;
        dotted += '.';
        dotted += part.str();
        tbl = child.as_table();
    }
    return dotted;
}

/// `[reactions.notify.apps]`: app name => color. Keys are lowercased, and two keys that collide
/// after that are an error rather than a silent last-one-wins.
std::expected<std::map<std::string, ColorRef, std::less<>>, std::string>
parse_apps(const toml::table &t) {
    constexpr std::string_view where = "reactions.notify.apps";
    std::map<std::string, ColorRef, std::less<>> apps;
    for (const auto &[key, node] : t) {
        if (const std::string dotted = unquoted_dotted_key(key, node); !dotted.empty()) {
            return Err(std::format("{}: key `{}` contains dots and must be quoted: \"{}\" = ...",
                                   where, dotted, dotted));
        }
        const auto s = node.value<std::string>();
        if (!s) {
            return Err(std::format("{}.{} must be a color string", where, key.str()));
        }
        auto c = ColorRef::parse(*s);
        if (!c) {
            return Err(std::format("{}.{}: {}", where, key.str(), c.error()));
        }
        const auto [it, inserted] = apps.emplace(to_lower(key.str()), std::move(*c));
        if (!inserted) {
            return Err(std::format("{}: duplicate app `{}`", where, it->first));
        }
    }
    return apps;
}

std::expected<void, std::string> parse_notify(const toml::table &t, ReactionsCfg::Notify &out) {
    constexpr std::string_view where = "reactions.notify";
    OMENKBD_TRY(
        check_keys(t, {"color", "critical", "min_urgency", "skip_if_focused", "apps"}, where));
    OMENKBD_TRY(read_color(t, "color", where, out.color));
    OMENKBD_TRY(read_color(t, "critical", where, out.critical));
    const auto urgency = opt<std::string>(t, "min_urgency", where);
    if (!urgency) {
        return Err(urgency.error());
    }
    if (*urgency) {
        const auto u = urgency_from_name(**urgency);
        if (!u) {
            return Err(std::format(
                "{}: `min_urgency` must be \"low\", \"normal\" or \"critical\", got \"{}\"", where,
                **urgency));
        }
        out.min_urgency = *u;
    }
    OMENKBD_TRY(read_bool(t, "skip_if_focused", where, out.skip_if_focused));
    const auto apps = sub_table(t, "apps", where);
    if (!apps) {
        return Err(apps.error());
    }
    if (*apps != nullptr) {
        auto parsed = parse_apps(**apps);
        if (!parsed) {
            return Err(parsed.error());
        }
        out.apps = std::move(*parsed);
    }
    return {};
}

std::expected<void, std::string> parse_urgent(const toml::table &t, ReactionsCfg::Urgent &out) {
    constexpr std::string_view where = "reactions.urgent";
    OMENKBD_TRY(check_keys(t, {"zones", "color", "blinks"}, where));
    OMENKBD_TRY(read_zones(t, "zones", where, out.zones));
    OMENKBD_TRY(read_color(t, "color", where, out.color));
    OMENKBD_TRY(read_int(t, "blinks", where, 0, 5, out.blinks));
    return {};
}

std::expected<void, std::string> parse_agent(const toml::table &t, ReactionsCfg::Agent &out) {
    constexpr std::string_view where = "reactions.agent";
    OMENKBD_TRY(check_keys(t, {"zones", "waiting", "done", "blinks", "ttl_s"}, where));
    OMENKBD_TRY(read_zones(t, "zones", where, out.zones));
    OMENKBD_TRY(read_color(t, "waiting", where, out.waiting));
    OMENKBD_TRY(read_color(t, "done", where, out.done));
    OMENKBD_TRY(read_int(t, "blinks", where, 0, 5, out.blinks));
    OMENKBD_TRY(read_int(t, "ttl_s", where, 60, 86400, out.ttl_s));
    return {};
}

/// Runs `parse` on the sub-table `key` of `t` when there is one.
template <class Section, class Parse>
std::expected<void, std::string> with_section(const toml::table &t, std::string_view key,
                                              Section &out, Parse parse) {
    const auto s = sub_table(t, key, "reactions");
    if (!s) {
        return Err(s.error());
    }
    if (*s != nullptr) {
        return parse(**s, out);
    }
    return {};
}

} // namespace

ReactionsCfg default_reactions() {
    ReactionsCfg c;
    c.workspace.color = ColorRef::theme("lavender");
    c.notify.color = ColorRef::theme("blue");
    c.notify.critical = ColorRef::theme("red");
    c.notify.apps.emplace("zapzap", ColorRef::theme("green"));
    c.notify.apps.emplace("discord", ColorRef::hex(Rgb{0x58, 0x65, 0xf2}));
    c.urgent.color = ColorRef::theme("yellow");
    c.agent.waiting = ColorRef::theme("peach");
    c.agent.done = ColorRef::theme("green");
    return c;
}

std::expected<ReactionsCfg, std::string> parse_reactions(const toml::table &t) {
    constexpr std::string_view where = "reactions";
    OMENKBD_TRY(check_keys(t,
                           {"enabled", "step_ms", "blink_ms", "max_per_minute", "workspace",
                            "window", "notify", "urgent", "agent"},
                           where));
    ReactionsCfg c = default_reactions();
    c.enabled = true; // the table is present
    OMENKBD_TRY(read_bool(t, "enabled", where, c.enabled));
    OMENKBD_TRY(read_int(t, "step_ms", where, 60, 1000, c.step_ms));
    OMENKBD_TRY(read_int(t, "blink_ms", where, 60, 1000, c.blink_ms));
    OMENKBD_TRY(read_int(t, "max_per_minute", where, 1, 600, c.max_per_minute));
    OMENKBD_TRY(with_section(t, "workspace", c.workspace, parse_workspace));
    OMENKBD_TRY(with_section(t, "window", c.window, parse_window));
    OMENKBD_TRY(with_section(t, "notify", c.notify, parse_notify));
    OMENKBD_TRY(with_section(t, "urgent", c.urgent, parse_urgent));
    OMENKBD_TRY(with_section(t, "agent", c.agent, parse_agent));
    return c;
}

} // namespace omenkbd

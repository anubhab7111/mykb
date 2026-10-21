#include "ipc/protocol.hpp"

#include <format>
#include <nlohmann/json.hpp>

namespace omenkbd {

namespace {

using ojson = nlohmann::ordered_json;

template <class T> ojson opt_json(const std::optional<T> &v) {
    return v ? ojson(*v) : ojson(nullptr);
}

template <class T> std::optional<T> opt_get(const ojson &j, const char *key) {
    if (!j.contains(key) || j.at(key).is_null()) {
        return std::nullopt;
    }
    return j.at(key).get<T>();
}

std::string_view power_name(req::PowerAction a) {
    switch (a) {
    case req::PowerAction::On:
        return "on";
    case req::PowerAction::Off:
        return "off";
    case req::PowerAction::Toggle:
        return "toggle";
    }
    return "?";
}

ojson facts_json(const Facts &f) {
    return ojson{{"power_profile", opt_json(f.power_profile)},
                 {"on_battery", opt_json(f.on_battery)},
                 {"battery_pct", opt_json(f.battery_pct)},
                 {"cpu_temp_c", opt_json(f.cpu_temp_c)},
                 {"gamemode", f.gamemode},
                 {"locked", f.locked},
                 {"idle", f.idle}};
}

Facts facts_from(const ojson &j) {
    Facts f;
    f.power_profile = opt_get<std::string>(j, "power_profile");
    f.on_battery = opt_get<bool>(j, "on_battery");
    f.battery_pct = opt_get<double>(j, "battery_pct");
    f.cpu_temp_c = opt_get<double>(j, "cpu_temp_c");
    f.gamemode = j.value("gamemode", false);
    f.locked = j.value("locked", false);
    f.idle = j.value("idle", false);
    return f;
}

ojson status_json(const Status &s) {
    return ojson{{"profile", s.profile},
                 {"effect", s.effect},
                 {"effects_enabled", s.effects_enabled},
                 {"animated", s.animated},
                 {"brightness", s.brightness},
                 {"on", s.on},
                 {"backlight", s.backlight},
                 {"zones", s.zones},
                 {"hint", opt_json(s.hint)},
                 {"active_rules", s.active_rules},
                 {"facts", facts_json(s.facts)},
                 {"backend", s.backend},
                 {"last_error", opt_json(s.last_error)},
                 {"budget_exhausted", s.budget_exhausted},
                 {"lifetime_writes", s.lifetime_writes},
                 {"warnings", s.warnings}};
}

Status status_from(const ojson &j) {
    Status s;
    s.profile = j.at("profile").get<std::string>();
    s.effect = j.at("effect").get<std::string>();
    s.effects_enabled = j.at("effects_enabled").get<bool>();
    s.animated = j.value("animated", false);
    s.brightness = j.at("brightness").get<std::uint8_t>();
    s.on = j.at("on").get<bool>();
    s.backlight = j.at("backlight").get<bool>();
    s.zones = j.at("zones").get<std::array<std::string, 4>>();
    s.hint = opt_get<std::string>(j, "hint");
    s.active_rules = j.at("active_rules").get<std::vector<std::string>>();
    s.facts = facts_from(j.at("facts"));
    s.backend = j.at("backend").get<std::string>();
    s.last_error = opt_get<std::string>(j, "last_error");
    s.budget_exhausted = j.at("budget_exhausted").get<bool>();
    s.lifetime_writes = j.at("lifetime_writes").get<std::uint64_t>();
    s.warnings = j.at("warnings").get<std::vector<std::string>>();
    return s;
}

Request request_from(const ojson &j) {
    const std::string cmd = j.at("cmd").get<std::string>();
    if (cmd == "status") {
        return req::Status{};
    }
    if (cmd == "set") {
        return req::Set{j.at("colors").get<std::vector<std::string>>()};
    }
    if (cmd == "profile") {
        return req::Profile{j.at("select").get<std::string>()};
    }
    if (cmd == "brightness") {
        return req::Brightness{j.at("value").get<std::string>()};
    }
    if (cmd == "power") {
        const std::string a = j.at("action").get<std::string>();
        if (a == "on") {
            return req::Power{req::PowerAction::On};
        }
        if (a == "off") {
            return req::Power{req::PowerAction::Off};
        }
        if (a == "toggle") {
            return req::Power{req::PowerAction::Toggle};
        }
        throw std::invalid_argument(std::format("unknown power action \"{}\"", a));
    }
    if (cmd == "effect") {
        return req::Effect{j.at("name").get<std::string>(), opt_get<std::uint8_t>(j, "speed")};
    }
    if (cmd == "hint") {
        return req::Hint{j.at("mods").get<std::string>()};
    }
    if (cmd == "idle") {
        return req::Idle{j.at("on").get<bool>()};
    }
    if (cmd == "flag") {
        return req::Flag{j.at("name").get<std::string>(), j.at("on").get<bool>()};
    }
    if (cmd == "reload") {
        return req::Reload{};
    }
    if (cmd == "subscribe") {
        return req::Subscribe{};
    }
    throw std::invalid_argument(std::format("unknown command \"{}\"", cmd));
}

} // namespace

std::string encode(const Request &r) {
    struct Visitor {
        ojson operator()(const req::Status &) const { return {{"cmd", "status"}}; }
        ojson operator()(const req::Set &v) const { return {{"cmd", "set"}, {"colors", v.colors}}; }
        ojson operator()(const req::Profile &v) const {
            return {{"cmd", "profile"}, {"select", v.select}};
        }
        ojson operator()(const req::Brightness &v) const {
            return {{"cmd", "brightness"}, {"value", v.value}};
        }
        ojson operator()(const req::Power &v) const {
            return {{"cmd", "power"}, {"action", power_name(v.action)}};
        }
        ojson operator()(const req::Effect &v) const {
            return {{"cmd", "effect"}, {"name", v.name}, {"speed", opt_json(v.speed)}};
        }
        ojson operator()(const req::Hint &v) const { return {{"cmd", "hint"}, {"mods", v.mods}}; }
        ojson operator()(const req::Idle &v) const { return {{"cmd", "idle"}, {"on", v.on}}; }
        ojson operator()(const req::Flag &v) const {
            return {{"cmd", "flag"}, {"name", v.name}, {"on", v.on}};
        }
        ojson operator()(const req::Reload &) const { return {{"cmd", "reload"}}; }
        ojson operator()(const req::Subscribe &) const { return {{"cmd", "subscribe"}}; }
    };
    return std::visit(Visitor{}, r).dump();
}

std::expected<Request, std::string> decode_request(std::string_view line) {
    try {
        return request_from(ojson::parse(line));
    } catch (const std::exception &e) {
        return std::unexpected(std::format("bad request: {}", e.what()));
    }
}

std::string encode(const Response &r) {
    return ojson{{"ok", r.ok},
                 {"error", opt_json(r.error)},
                 {"status", r.status ? status_json(*r.status) : ojson(nullptr)}}
        .dump();
}

std::expected<Response, std::string> decode_response(std::string_view line) {
    try {
        const ojson j = ojson::parse(line);
        Response r;
        r.ok = j.at("ok").get<bool>();
        r.error = opt_get<std::string>(j, "error");
        if (j.contains("status") && !j.at("status").is_null()) {
            r.status = status_from(j.at("status"));
        }
        return r;
    } catch (const std::exception &e) {
        return std::unexpected(std::format("bad response: {}", e.what()));
    }
}

} // namespace omenkbd

#include "cli/format.hpp"

#include <format>

namespace omenkbd {

namespace {

std::string join(const std::vector<std::string> &items, std::string_view sep) {
    std::string out;
    for (const auto &item : items) {
        if (!out.empty()) {
            out += sep;
        }
        out += item;
    }
    return out;
}

const char *on_off(bool b) { return b ? "on" : "off"; }

} // namespace

std::string format_status(const Status &s) {
    std::string out;
    const auto line = [&out](std::string_view label, std::string_view value) {
        out += std::format("{:<12}{}\n", label, value);
    };

    std::string profile = s.profile;
    if (s.effect != "static") {
        profile += "  effect " + s.effect;
        if (!s.effects_enabled) {
            profile += " (animations off)";
        } else if (!s.animated) {
            profile += " (paused)";
        }
    }
    line("profile", profile);

    std::string zones;
    constexpr std::string_view kNames = "LMRW";
    for (std::size_t i = 0; i < 4; ++i) {
        zones += std::format("{}{} {}", i == 0 ? "" : "  ", kNames[i],
                             s.zones[i].empty() ? "-" : "#" + s.zones[i]);
    }
    line("zones", zones);
    line("brightness",
         std::format("{}%  {}  backlight {}", s.brightness, on_off(s.on), on_off(s.backlight)));
    line("hint", s.hint.value_or("-"));
    line("rules", s.active_rules.empty() ? "-" : join(s.active_rules, ", "));
    line("backend", std::format("{}  writes {}", s.backend, s.lifetime_writes));
    if (s.last_error) {
        line("error", *s.last_error);
    }
    if (s.budget_exhausted) {
        line("budget", "hourly write budget exhausted; effects paused until it refills");
    }
    if (!s.warnings.empty()) {
        line("warnings", join(s.warnings, "; "));
    }
    return out;
}

nlohmann::json waybar_json(const Status &s) {
    const char *cls = (s.last_error || s.budget_exhausted) ? "error"
                      : !s.on                              ? "off"
                      : s.hint                             ? "hint"
                      : s.animated                         ? "effect"
                                                           : "on";
    return {{"text", s.on ? "󰌌 " + s.profile : std::string("󰌐 off")},
            {"class", cls},
            {"percentage", s.brightness},
            {"tooltip", format_status(s)}};
}

} // namespace omenkbd

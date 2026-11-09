#include "core/lookbook.hpp"

#include "util/strings.hpp"

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <utility>
#include <vector>

namespace omenkbd {

namespace {

Duration millis(std::uint32_t ms) { return std::chrono::milliseconds(ms); }

std::map<std::string, Rgb, std::less<>>
resolve_apps(const std::map<std::string, ColorRef, std::less<>> &apps, const Resolver &resolve) {
    std::map<std::string, Rgb, std::less<>> out;
    for (const auto &[key, color] : apps) {
        out.emplace(key, resolve(color));
    }
    return out;
}

/// The lowercase names a notification's app goes by, in the order they are matched: the app name,
/// the desktop entry, and the entry's last dot-separated segment. An empty name is left out, so an
/// absent app, an entry without a dot (no last segment) or one ending in a dot matches nothing.
std::vector<std::string> identities(const signal::Notified &n) {
    std::vector<std::string> ids;
    const auto add = [&ids](std::string id) {
        if (!id.empty()) {
            ids.push_back(std::move(id));
        }
    };
    std::string entry = to_lower(n.desktop_entry);
    const std::size_t dot = entry.rfind('.');
    std::string last = dot == std::string::npos ? std::string{} : entry.substr(dot + 1);
    add(to_lower(n.app));
    add(std::move(entry));
    add(std::move(last));
    return ids;
}

} // namespace

LookBook::LookBook(const ReactionsCfg &cfg, const Resolver &resolve)
    : step_(millis(cfg.step_ms)), blink_(millis(cfg.blink_ms)), max_per_minute_(cfg.max_per_minute),
      window_cooldown_(millis(cfg.window.cooldown_ms)),
      workspace_color_(resolve(cfg.workspace.color)), wasd_with_(cfg.workspace.wasd_with),
      window_open_(cfg.window.open), window_close_(cfg.window.close),
      notify_{resolve(cfg.notify.color), resolve(cfg.notify.critical), cfg.notify.min_urgency,
              cfg.notify.skip_if_focused, resolve_apps(cfg.notify.apps, resolve)},
      urgent_{cfg.urgent.zones, resolve(cfg.urgent.color), cfg.urgent.blinks, blink_, std::nullopt},
      agent_{cfg.agent.zones, resolve(cfg.agent.waiting), cfg.agent.blinks, blink_,
             std::chrono::seconds(cfg.agent.ttl_s)},
      agent_done_(resolve(cfg.agent.done)) {}

std::optional<Clip> LookBook::workspace(int from, int to) const {
    if (from == to) {
        return std::nullopt;
    }
    return sweep(to > from ? Direction::Up : Direction::Down, workspace_color_, wasd_with_, step_);
}

std::optional<Clip> LookBook::window_opened() const {
    if (window_open_ <= 0.0) {
        return std::nullopt;
    }
    return lift(window_open_, step_);
}

std::optional<Clip> LookBook::window_closed() const {
    if (window_close_ >= 1.0) {
        return std::nullopt;
    }
    return dip(window_close_, step_);
}

std::optional<Clip> LookBook::notified(const signal::Notified &n,
                                       std::string_view focused_class) const {
    if (n.urgency < notify_.min_urgency) {
        return std::nullopt;
    }
    const std::vector<std::string> ids = identities(n);
    if (notify_.skip_if_focused && !focused_class.empty() &&
        std::ranges::find(ids, to_lower(focused_class)) != ids.end()) {
        return std::nullopt;
    }
    Rgb color = notify_.color;
    if (n.urgency == Urgency::Critical) {
        color = notify_.critical;
    } else {
        for (const std::string &id : ids) {
            if (const auto app = notify_.apps.find(id); app != notify_.apps.end()) {
                color = app->second;
                break;
            }
        }
    }
    const std::vector<Zone> all(kAllZones.begin(), kAllZones.end());
    return flash(color, all, 1, blink_, blink_);
}

std::optional<Clip> LookBook::agent_done() const {
    if (agent_.zones.empty()) {
        return std::nullopt;
    }
    return flash(agent_done_, agent_.zones, 1, blink_, blink_);
}

LampStyle LookBook::urgent_style() const { return urgent_; }

LampStyle LookBook::agent_style() const { return agent_; }

PlayerLimits LookBook::limits() const {
    return PlayerLimits{max_per_minute_, window_cooldown_, blink_};
}

} // namespace omenkbd

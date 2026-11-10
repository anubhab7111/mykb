#include "sources/hyprland_bridge.hpp"

#include "util/strings.hpp"

#include <utility>
#include <variant>

namespace omenkbd {

HyprlandBridge::HyprlandBridge(EventLoop &loop, HyprlandSource::PathFn path_fn, Emit emit)
    : emit_(std::move(emit)),
      query_(loop, [this](const std::optional<ActiveWindowInfo> &info) { on_query_result(info); }),
      source_(loop, std::move(path_fn), [this](const HyprEvent &ev) { on_hypr(ev); }) {}

void HyprlandBridge::configure(bool reactions_on, bool fullscreen_wanted) {
    const bool switched_on = reactions_on && !reactions_on_;
    reactions_on_ = reactions_on; // before the replay: `deliver` drops signals while it is off
    if (switched_on) {
        // P25: the tracker followed Hyprland while the signals were dropped. Say what it holds, or
        // the focus stays unknown (and an urgent window dark) until another window gets the focus:
        // the tracker ignores a repeat of the focused window (P15).
        for (const Signal &s : tracker_.replay()) {
            deliver(s);
        }
    }
    if (fullscreen_wanted == fullscreen_wanted_) {
        return;
    }
    fullscreen_wanted_ = fullscreen_wanted;
    if (fullscreen_wanted_) {
        check_fullscreen(); // P21: the check that came with `Connected` was ignored
    } else {
        query_.cancel(); // a reply that is still on its way must not bring the fact back
        set_fullscreen(std::nullopt);
    }
}

void HyprlandBridge::on_hypr(const HyprEvent &ev) {
    if (std::holds_alternative<hypr::Disconnected>(ev)) {
        query_.cancel(); // what a Hyprland that is gone says no longer counts
    }
    for (const TrackerOut &out : tracker_.on(ev)) {
        std::visit([this](const auto &o) { deliver(o); }, out);
    }
}

void HyprlandBridge::deliver(const Signal &s) {
    if (reactions_on_) {
        emit_(Event{s});
    }
}

void HyprlandBridge::deliver(const FactUpdate &f) {
    if (const auto *fullscreen = std::get_if<fact::FullscreenApp>(&f)) {
        fullscreen_ = fullscreen->v; // the tracker clears it on a disconnect; remember that
    }
    emit_(Event{f});
}

void HyprlandBridge::deliver(const event::Submap &m) { emit_(Event{m}); }

void HyprlandBridge::deliver(const CheckFullscreen & /*check*/) { check_fullscreen(); }

void HyprlandBridge::check_fullscreen() {
    // Not wanted comes first: `Connected` arrives while `source_` is still being constructed.
    if (!fullscreen_wanted_) {
        return;
    }
    if (const auto &socket2 = source_.connected_path()) {
        query_.request(socket2->parent_path() / ".socket.sock");
    }
}

void HyprlandBridge::on_query_result(const std::optional<ActiveWindowInfo> &info) {
    if (!info) {
        return; // the query failed: keep what we know
    }
    if (is_true_fullscreen(info->fullscreen)) {
        set_fullscreen(to_lower(info->app_class)); // rules compare lowercased classes
    } else {
        set_fullscreen(std::nullopt); // also what "no window is focused" comes to
    }
}

void HyprlandBridge::set_fullscreen(std::optional<std::string> app) {
    if (app == fullscreen_) {
        return;
    }
    fullscreen_ = std::move(app);
    emit_(Event{FactUpdate{fact::FullscreenApp{fullscreen_}}});
}

} // namespace omenkbd

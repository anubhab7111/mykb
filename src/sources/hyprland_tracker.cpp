#include "sources/hyprland_tracker.hpp"

#include <algorithm>
#include <string>
#include <utility>

namespace omenkbd {

namespace {

TrackerOut check() { return CheckFullscreen{}; }

template <class S> TrackerOut sig(S s) { return Signal{std::move(s)}; }

} // namespace

std::vector<TrackerOut> HyprlandTracker::on(const HyprEvent &ev) {
    return std::visit([this](const auto &e) { return handle(e); }, ev);
}

std::vector<Signal> HyprlandTracker::replay() const {
    std::vector<Signal> out;
    if (focused_) {
        out.emplace_back(signal::FocusChanged{*focused_, app_class_});
    }
    std::vector<std::string> urgent(urgent_.begin(), urgent_.end()); // sorted next: a stable order
    std::ranges::sort(urgent);
    for (std::string &window : urgent) {
        out.emplace_back(signal::UrgentRaised{std::move(window)});
    }
    return out;
}

void HyprlandTracker::reset() {
    workspace_.reset();
    focused_.reset();
    app_class_.clear();
    urgent_.clear();
}

HyprlandTracker::Out HyprlandTracker::handle(const hypr::Submap &ev) {
    Out out;
    out.emplace_back(event::Submap{ev.name});
    return out;
}

HyprlandTracker::Out HyprlandTracker::handle(const hypr::WorkspaceV2 &ev) {
    Out out;
    if (ev.id <= 0) {
        return out; // a special workspace: the one underneath is still the workspace
    }
    const std::optional<int> previous = std::exchange(workspace_, ev.id);
    if (previous && *previous != ev.id) {
        out.push_back(sig(signal::WorkspaceSwitched{*previous, ev.id}));
    }
    out.push_back(check());
    return out;
}

HyprlandTracker::Out HyprlandTracker::handle(const hypr::OpenWindow & /*ev*/) {
    Out out;
    out.push_back(sig(signal::WindowOpened{}));
    return out;
}

HyprlandTracker::Out HyprlandTracker::handle(const hypr::CloseWindow &ev) {
    Out out;
    out.push_back(sig(signal::WindowClosed{}));
    if (urgent_.erase(ev.address) > 0) {
        out.push_back(sig(signal::UrgentCleared{ev.address}));
    }
    out.push_back(check());
    return out;
}

HyprlandTracker::Out HyprlandTracker::handle(const hypr::Urgent &ev) {
    Out out;
    if (focused_ == ev.address) {
        return out; // the user is looking at it
    }
    urgent_.insert(ev.address);
    out.push_back(sig(signal::UrgentRaised{ev.address}));
    return out;
}

HyprlandTracker::Out HyprlandTracker::handle(const hypr::ActiveWindow &ev) {
    app_class_ = ev.app_class;
    return {};
}

HyprlandTracker::Out HyprlandTracker::handle(const hypr::ActiveWindowV2 &ev) {
    Out out;
    if (focused_ == ev.address) {
        return out; // a title change of the focused window re-sends both focus events (P15)
    }
    focused_ = ev.address;
    urgent_.erase(ev.address);
    out.push_back(sig(signal::FocusChanged{ev.address, app_class_}));
    out.push_back(check());
    return out;
}

HyprlandTracker::Out HyprlandTracker::handle(const hypr::Fullscreen & /*ev*/) {
    Out out;
    out.push_back(check());
    return out;
}

HyprlandTracker::Out HyprlandTracker::handle(const hypr::Connected & /*ev*/) {
    reset();
    Out out;
    out.push_back(check());
    return out;
}

HyprlandTracker::Out HyprlandTracker::handle(const hypr::Disconnected & /*ev*/) {
    reset();
    Out out;
    out.push_back(sig(signal::UrgentReset{}));
    out.emplace_back(FactUpdate{fact::FullscreenApp{std::nullopt}});
    out.emplace_back(event::Submap{std::nullopt});
    return out;
}

} // namespace omenkbd

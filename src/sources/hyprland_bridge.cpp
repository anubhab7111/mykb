#include "sources/hyprland_bridge.hpp"

#include <utility>
#include <variant>

namespace omenkbd {

namespace {

/// Delivers a tracker output; `signals_on` gates only the signals.
struct Deliver {
    const HyprlandBridge::Emit &emit;
    bool signals_on;

    void operator()(const Signal &s) const {
        if (signals_on) {
            emit(Event{s});
        }
    }
    void operator()(const FactUpdate &f) const { emit(Event{f}); }
    void operator()(const event::Submap &m) const { emit(Event{m}); }
    void operator()(const CheckFullscreen & /*check*/) const {} // nothing queries fullscreen yet
};

} // namespace

HyprlandBridge::HyprlandBridge(EventLoop &loop, HyprlandSource::PathFn path_fn, Emit emit)
    : emit_(std::move(emit)),
      source_(loop, std::move(path_fn), [this](const HyprEvent &ev) { on_hypr(ev); }) {}

void HyprlandBridge::configure(bool reactions_on) { reactions_on_ = reactions_on; }

void HyprlandBridge::on_hypr(const HyprEvent &ev) {
    const Deliver deliver{emit_, reactions_on_};
    for (const TrackerOut &out : tracker_.on(ev)) {
        std::visit(deliver, out);
    }
}

} // namespace omenkbd

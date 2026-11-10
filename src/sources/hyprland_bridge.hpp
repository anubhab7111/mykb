#pragma once
// Hyprland on the loop, as the daemon sees it: the socket2 source feeds the tracker, and what the
// tracker says is delivered as events. The one thing `run_daemon` wires for Hyprland.

#include "daemon/event_loop.hpp"
#include "daemon/events.hpp"
#include "sources/hyprland.hpp"
#include "sources/hyprland_tracker.hpp"

#include <functional>

namespace omenkbd {

class HyprlandBridge {
  public:
    using Emit = std::function<void(const Event &)>;

    /// Connects at once (see HyprlandSource), with reactions off until `configure` says otherwise.
    HyprlandBridge(EventLoop &loop, HyprlandSource::PathFn path_fn, Emit emit);
    HyprlandBridge(const HyprlandBridge &) = delete;
    HyprlandBridge &operator=(const HyprlandBridge &) = delete;

    /// Signals (workspace, windows, urgency, focus) are forwarded only while `reactions_on`. The
    /// fullscreen fact and the submap always are. The tracker follows Hyprland either way, so a
    /// switch takes effect at once. Loop thread only.
    void configure(bool reactions_on);

  private:
    void on_hypr(const HyprEvent &ev);

    // `source_` comes last: it connects in its constructor and may call `on_hypr` before the
    // constructor returns, so everything `on_hypr` uses must already exist.
    Emit emit_;
    HyprlandTracker tracker_;
    bool reactions_on_ = false;
    HyprlandSource source_;
};

} // namespace omenkbd

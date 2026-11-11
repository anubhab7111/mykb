#pragma once
// Hyprland on the loop, as the daemon sees it: the socket2 source feeds the tracker, what the
// tracker says is delivered as events, and a request to look at fullscreen again is answered by an
// async query to the request socket. The one thing `run_daemon` wires for Hyprland.

#include "daemon/event_loop.hpp"
#include "daemon/events.hpp"
#include "sources/hyprctl_query.hpp"
#include "sources/hyprland.hpp"
#include "sources/hyprland_tracker.hpp"

#include <functional>
#include <optional>
#include <string>

namespace omenkbd {

class HyprlandBridge {
  public:
    using Emit = std::function<void(const Event &)>;

    /// Connects at once (see HyprlandSource), with reactions and the fullscreen fact off until
    /// `configure` says otherwise.
    HyprlandBridge(EventLoop &loop, HyprlandSource::PathFn path_fn, Emit emit);
    ~HyprlandBridge();
    HyprlandBridge(const HyprlandBridge &) = delete;
    HyprlandBridge &operator=(const HyprlandBridge &) = delete;

    /// Signals (workspace, windows, urgency, focus) are forwarded only while `reactions_on`. The
    /// fullscreen fact and the submap always are. The tracker follows Hyprland either way, so a
    /// switch takes effect at once: on the change from off to on, the focus and urgent windows it
    /// holds are delivered as signals, once (P25).
    ///
    /// While `fullscreen_wanted`, each request to look at fullscreen starts a query to the request
    /// socket next to the connected socket2, and the fact follows the answer. A failed query keeps
    /// the last value; if that value is a fullscreen app (the keyboard is dimmed), the bridge asks
    /// once more about 500 ms later, and a retry that fails is not retried (F3). A new request, a
    /// disconnect or turning the fact off drops a pending retry. Turning it on while connected
    /// asks once at once (the check that came with the connection found nobody wanting it);
    /// turning it off clears the fact (once) and drops any query. Loop thread only.
    void configure(bool reactions_on, bool fullscreen_wanted);

  private:
    void on_hypr(const HyprEvent &ev);
    void deliver(const Signal &s);
    void deliver(const FactUpdate &f);
    void deliver(const event::Submap &m);
    void deliver(const CheckFullscreen &check);
    /// A new question about fullscreen: drops a pending retry, then asks (if wanted).
    void check_fullscreen();
    /// Starts a query to the request socket next to the connected socket2, if connected.
    void ask();
    void cancel_retry();
    void on_query_result(const std::optional<ActiveWindowInfo> &info);
    /// Sends the fact if it differs from the last one sent.
    void set_fullscreen(std::optional<std::string> app);

    // `source_` comes last: it connects in its constructor and may call `on_hypr` before the
    // constructor returns, so everything `on_hypr` uses must already exist.
    EventLoop &loop_;
    Emit emit_;
    HyprlandTracker tracker_;
    HyprctlQuery query_;
    bool reactions_on_ = false;
    bool fullscreen_wanted_ = false;
    /// The last `FullscreenApp` sent, whoever it came from. Starts as the model starts: nothing.
    std::optional<std::string> fullscreen_;
    /// The one retry after a failed query, while it waits (F3).
    std::optional<EventLoop::TimerId> retry_;
    /// The query in flight is that retry: if it fails too, nothing more is asked.
    bool retrying_ = false;
    HyprlandSource source_;
};

} // namespace omenkbd

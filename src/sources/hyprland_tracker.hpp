#pragma once
// Turns Hyprland's parsed events into what the daemon understands: signals for reactions, the
// fullscreen fact, the submap, and a request to look at fullscreen again. Pure: no clock, no I/O,
// no window title (the parser already dropped them).

#include "core/signals.hpp"
#include "daemon/events.hpp"
#include "daemon/model.hpp"
#include "sources/hyprland.hpp"

#include <optional>
#include <string>
#include <unordered_set>
#include <variant>
#include <vector>

namespace omenkbd {

/// Something about the focused window may have changed fullscreen state: ask Hyprland.
struct CheckFullscreen {
    friend bool operator==(const CheckFullscreen &, const CheckFullscreen &) = default;
};

using TrackerOut = std::variant<Signal, FactUpdate, event::Submap, CheckFullscreen>;

/// Remembers what the events so far imply (current workspace, focused window, urgent windows) and
/// says what each new event means. `Connected` and `Disconnected` forget all of it: events from a
/// previous Hyprland say nothing about the next one.
class HyprlandTracker {
  public:
    /// The outputs for `ev`, in the order they should be delivered.
    std::vector<TrackerOut> on(const HyprEvent &ev);

  private:
    using Out = std::vector<TrackerOut>;

    Out handle(const hypr::Submap &ev);
    Out handle(const hypr::WorkspaceV2 &ev);
    Out handle(const hypr::OpenWindow &ev);
    Out handle(const hypr::CloseWindow &ev);
    Out handle(const hypr::Urgent &ev);
    Out handle(const hypr::ActiveWindow &ev);
    Out handle(const hypr::ActiveWindowV2 &ev);
    Out handle(const hypr::Fullscreen &ev);
    Out handle(const hypr::Connected &ev);
    Out handle(const hypr::Disconnected &ev);

    void reset();

    /// The workspace shown last; nullopt until the first event after a (re)connect.
    std::optional<int> workspace_;
    /// The focused window's address; empty means no window is focused, nullopt that no focus event
    /// has arrived since the (re)connect (P20).
    std::optional<std::string> focused_;
    /// The class from the latest `activewindow` line; the `activewindowv2` that follows uses it.
    std::string app_class_;
    std::unordered_set<std::string> urgent_;
};

} // namespace omenkbd

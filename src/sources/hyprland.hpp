#pragma once
// Hyprland's event socket (socket2): submap changes. Only `submap>>` lines matter; the rest of
// the stream is ignored.

#include "daemon/event_loop.hpp"
#include "daemon/events.hpp"
#include "ipc/socket.hpp"
#include "util/fd.hpp"

#include <filesystem>
#include <functional>
#include <optional>
#include <string_view>

namespace omenkbd {

/// `submap>>resize` ⇒ Submap{"resize"}; `submap>>` ⇒ Submap{nullopt}; anything else ⇒ nullopt.
std::optional<Event> parse_socket2_line(std::string_view line);

/// `$XDG_RUNTIME_DIR/hypr/$HYPRLAND_INSTANCE_SIGNATURE/.socket2.sock` if it exists, else the
/// newest `.socket2.sock` under `$XDG_RUNTIME_DIR/hypr/*/` (systemd user units often lack the
/// variable, and it goes stale when Hyprland restarts).
std::optional<std::filesystem::path> socket2_path();

/// Follows socket2 on the loop. When Hyprland goes away it emits Submap{nullopt} (so no submap
/// color sticks) and reconnects after 1 s, 2 s, then every 5 s, asking `path_fn` each time.
class HyprlandSource {
  public:
    using PathFn = std::function<std::optional<std::filesystem::path>()>;
    using Emit = std::function<void(const Event &)>;

    HyprlandSource(EventLoop &loop, PathFn path_fn, Emit emit);
    ~HyprlandSource();
    HyprlandSource(const HyprlandSource &) = delete;
    HyprlandSource &operator=(const HyprlandSource &) = delete;

  private:
    void connect();
    void on_readable();
    void disconnected();
    void retry_later();

    EventLoop &loop_;
    PathFn path_fn_;
    Emit emit_;
    UniqueFd fd_;
    LineBuffer lines_;
    int failures_ = 0;
    std::optional<EventLoop::TimerId> retry_;
};

} // namespace omenkbd

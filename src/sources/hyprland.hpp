#pragma once
// Hyprland's event socket (socket2): the lines we use, parsed into typed events with every window
// title dropped on the way, and the source that follows the socket on the loop.

#include "daemon/event_loop.hpp"
#include "ipc/socket.hpp"
#include "util/fd.hpp"

#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace omenkbd {

/// What a socket2 line says, and no more: no event carries a window title. Window addresses are
/// lowercase hex without `0x` (R8).
namespace hypr {
/// `nullopt` = the default (no) submap.
struct Submap {
    std::optional<std::string> name;
    friend bool operator==(const Submap &, const Submap &) = default;
};
/// `id` > 0 for a workspace, < 0 for a special one.
struct WorkspaceV2 {
    int id;
    friend bool operator==(const WorkspaceV2 &, const WorkspaceV2 &) = default;
};
struct OpenWindow {
    std::string address;
    friend bool operator==(const OpenWindow &, const OpenWindow &) = default;
};
struct CloseWindow {
    std::string address;
    friend bool operator==(const CloseWindow &, const CloseWindow &) = default;
};
struct Urgent {
    std::string address;
    friend bool operator==(const Urgent &, const Urgent &) = default;
};
/// The class of the window that got the focus. Empty when no window has it.
struct ActiveWindow {
    std::string app_class;
    friend bool operator==(const ActiveWindow &, const ActiveWindow &) = default;
};
/// The address of the window that got the focus; empty = nothing is focused.
struct ActiveWindowV2 {
    std::string address;
    friend bool operator==(const ActiveWindowV2 &, const ActiveWindowV2 &) = default;
};
/// The focused window entered (`on`) or left fullscreen.
struct Fullscreen {
    bool on;
    friend bool operator==(const Fullscreen &, const Fullscreen &) = default;
};
/// Not from the socket: the source connected to socket2 / lost it.
struct Connected {
    friend bool operator==(const Connected &, const Connected &) = default;
};
struct Disconnected {
    friend bool operator==(const Disconnected &, const Disconnected &) = default;
};
} // namespace hypr

using HyprEvent = std::variant<hypr::Submap, hypr::WorkspaceV2, hypr::OpenWindow, hypr::CloseWindow,
                               hypr::Urgent, hypr::ActiveWindow, hypr::ActiveWindowV2,
                               hypr::Fullscreen, hypr::Connected, hypr::Disconnected>;

/// One socket2 line (`EVENT>>DATA`, without the newline) ⇒ the event, or nullopt for an event we
/// do not use, a malformed line or one longer than 64 KiB. Pure. Fields are split only as far as
/// needed (the first `>>`, the first `,`), so a title with commas or `>>` cannot shift anything,
/// and the title is never kept.
std::optional<HyprEvent> parse_socket2_line(std::string_view line);

/// The socket2 paths to try, in order: `$XDG_RUNTIME_DIR/hypr/$HYPRLAND_INSTANCE_SIGNATURE/
/// .socket2.sock` if it exists, then every other `$XDG_RUNTIME_DIR/hypr/*/.socket2.sock`, newest
/// first. systemd user units often lack the variable, it goes stale when Hyprland restarts, and a
/// crashed instance leaves its socket file behind, so no single path can be trusted.
std::vector<std::filesystem::path> socket2_paths();

/// Follows socket2 on the loop and emits what it parses, plus `Connected` once a connection is
/// made and `Disconnected` when it ends. After a disconnect it reconnects after 1 s, 2 s, then
/// every 5 s, asking `path_fn` each time and using the first path that accepts.
class HyprlandSource {
  public:
    using PathFn = std::function<std::vector<std::filesystem::path>()>;
    using Emit = std::function<void(const HyprEvent &)>;

    /// Connects at once, so `emit` may be called before the constructor returns.
    HyprlandSource(EventLoop &loop, PathFn path_fn, Emit emit);
    ~HyprlandSource();
    HyprlandSource(const HyprlandSource &) = delete;
    HyprlandSource &operator=(const HyprlandSource &) = delete;

    /// The socket2 path of the live connection; nullopt while not connected.
    [[nodiscard]] const std::optional<std::filesystem::path> &connected_path() const {
        return connected_path_;
    }

  private:
    void connect();
    void on_readable();
    void disconnected();
    void retry_later();

    EventLoop &loop_;
    PathFn path_fn_;
    Emit emit_;
    UniqueFd fd_;
    std::optional<std::filesystem::path> connected_path_;
    LineBuffer lines_;
    int failures_ = 0;
    std::optional<EventLoop::TimerId> retry_;
};

} // namespace omenkbd

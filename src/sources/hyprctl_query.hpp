#pragma once
// One question to Hyprland's request socket (`.socket.sock`, the one `hyprctl` talks to): which
// window is focused, and is it fullscreen. Asked on the loop without ever blocking it, debounced,
// and bounded in time and size. Only the window's address and class are read from the reply; the
// title is dropped where the JSON is parsed.

#include "daemon/event_loop.hpp"
#include "util/fd.hpp"

#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <string_view>

namespace omenkbd {

/// The focused window, from `hyprctl activewindow -j`. A default-constructed value (empty address)
/// is the answer "no window is focused".
struct ActiveWindowInfo {
    std::string address; ///< lowercase hex without `0x` (R8)
    std::string app_class;
    int fullscreen = 0; ///< Hyprland's mode bits: 1 maximized, 2 fullscreen
    friend bool operator==(const ActiveWindowInfo &, const ActiveWindowInfo &) = default;
};

/// R9 (verified): true fullscreen is the 2 bit, so maximized (1) is not and maximized plus
/// fullscreen (3) is.
constexpr bool is_true_fullscreen(int mode) { return (static_cast<unsigned>(mode) & 2U) != 0; }

/// The JSON that `j/activewindow` returns for a window ⇒ its address, class and fullscreen mode.
/// Pure. nullopt for `{}` (nothing focused), for anything that is not a JSON object with a string
/// `address` (not empty after normalizing), a string `class` and a non-negative integer
/// `fullscreen`, and for garbage; never throws. Other fields, the title among them, are ignored.
std::optional<ActiveWindowInfo> parse_active_window(std::string_view json);

/// Asks `j/activewindow` on a request socket and reports what came back. Writes the command and
/// reads until EOF, as Hyprland expects.
///
/// - `request` is debounced: the query starts 100 ms after the last `request`, and a newer
///   `request` replaces one that is waiting or in flight (the older reply is never delivered).
/// - At most one query is in flight. It is cut off 300 ms after it connects, and a reply over
///   64 KiB is refused.
/// - The result is the window; a default `ActiveWindowInfo` when Hyprland says nothing is focused
///   (it answers `{}`); nullopt when the query failed (no socket, error, timeout, too big, not an
///   answer, an empty reply: Hyprland never sends one, so it is a hiccup, not "nothing focused").
///   Delivered once per started query, on the loop thread, after the query has let go of its
///   socket and timers, so the callback may call `request` again.
/// - The destructor and `cancel` close the socket and drop the query without a callback.
///
/// Loop thread only.
class HyprctlQuery {
  public:
    using Result = std::function<void(std::optional<ActiveWindowInfo>)>;

    HyprctlQuery(EventLoop &loop, Result on_result);
    ~HyprctlQuery();
    HyprctlQuery(const HyprctlQuery &) = delete;
    HyprctlQuery &operator=(const HyprctlQuery &) = delete;

    void request(const std::filesystem::path &request_socket);

    /// Drops what is waiting or in flight; no result is delivered for it.
    void cancel();

  private:
    void start();
    void on_readable();
    void finish(std::optional<ActiveWindowInfo> result);

    EventLoop &loop_;
    Result on_result_;
    std::filesystem::path socket_; ///< the latest `request`
    std::optional<EventLoop::TimerId> debounce_;
    std::optional<EventLoop::TimerId> timeout_;
    UniqueFd fd_;
    std::string reply_;
};

} // namespace omenkbd

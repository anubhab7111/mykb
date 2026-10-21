#pragma once
// Theme switches. The rice points `~/.config/theming/current` at the active theme through a
// chain of symlinks; any link in that chain being replaced means a new theme.

#include "daemon/event_loop.hpp"
#include "daemon/events.hpp"
#include "util/fd.hpp"

#include <filesystem>
#include <functional>
#include <map>
#include <optional>
#include <vector>

namespace omenkbd {

/// `p` and every symlink target after it, until a non-symlink (relative targets resolved against
/// the link's directory). Stops after 40 hops (a loop).
std::vector<std::filesystem::path> link_chain(const std::filesystem::path &p);

/// Watches the parent directory of every element of `dir`'s link chain (create, move-in, delete,
/// close-after-write, attribute change). An event naming a chain element emits ThemeChanged,
/// 200 ms after the last one; then the chain and the watches are rebuilt.
class ThemeWatcher {
  public:
    using Emit = std::function<void(const Event &)>;

    ThemeWatcher(EventLoop &loop, std::filesystem::path dir, Emit emit);
    ~ThemeWatcher();
    ThemeWatcher(const ThemeWatcher &) = delete;
    ThemeWatcher &operator=(const ThemeWatcher &) = delete;

  private:
    void rebuild();
    void on_readable();

    EventLoop &loop_;
    std::filesystem::path dir_;
    Emit emit_;
    UniqueFd inotify_;
    std::map<int, std::filesystem::path> watched_; // watch descriptor → directory
    std::vector<std::filesystem::path> chain_;
    std::optional<EventLoop::TimerId> debounce_;
};

} // namespace omenkbd

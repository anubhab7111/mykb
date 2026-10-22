// Event sources in real time: Hyprland socket2 (fake listener), theme symlink watcher (temp
// tree), hwmon (fake sysfs).

#include "helpers.hpp"

#include "ipc/socket.hpp"
#include "sources/hwmon.hpp"
#include "sources/hyprland.hpp"
#include "sources/theme.hpp"

#include <gtest/gtest.h>

#include <mutex>
#include <poll.h>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>

using namespace omenkbd;
using namespace omenkbd::test;
using namespace std::chrono_literals;
namespace fs = std::filesystem;

namespace {

/// Collects events emitted on the loop thread.
class Events {
  public:
    std::function<void(const Event &)> sink() {
        return [this](const Event &e) {
            const std::lock_guard lock(mu_);
            events_.push_back(e);
        };
    }
    std::vector<Event> get() const {
        const std::lock_guard lock(mu_);
        return events_;
    }
    std::size_t size() const { return get().size(); }

  private:
    mutable std::mutex mu_;
    std::vector<Event> events_;
};

/// Runs a loop on its own thread until destroyed.
class LoopThread {
  public:
    explicit LoopThread(EventLoop &loop) : loop_(loop), t_([&loop] { loop.run(); }) {}
    ~LoopThread() {
        loop_.stop();
        t_.join();
    }
    LoopThread(const LoopThread &) = delete;
    LoopThread &operator=(const LoopThread &) = delete;

  private:
    EventLoop &loop_;
    std::thread t_;
};

/// Sets (or, with nullopt, unsets) an environment variable until destroyed.
class ScopedEnv {
  public:
    ScopedEnv(const char *name, const std::optional<std::string> &value) : name_(name) {
        if (const char *old = std::getenv(name)) {
            old_ = old;
        }
        set(value);
    }
    ~ScopedEnv() { set(old_); }
    ScopedEnv(const ScopedEnv &) = delete;
    ScopedEnv &operator=(const ScopedEnv &) = delete;

  private:
    void set(const std::optional<std::string> &v) const {
        if (v) {
            ::setenv(name_, v->c_str(), 1);
        } else {
            ::unsetenv(name_);
        }
    }
    const char *name_;
    std::optional<std::string> old_;
};

/// A socket file nobody listens on, as a crashed Hyprland leaves behind.
void make_dead_socket(const fs::path &p) {
    fs::create_directories(p.parent_path());
    auto s = bind_socket(p);
    ASSERT_TRUE(s.has_value()) << s.error().message();
} // closed here; the file stays

UniqueFd accept_within(int listen_fd, std::chrono::milliseconds timeout) {
    pollfd p{listen_fd, POLLIN, 0};
    if (::poll(&p, 1, static_cast<int>(timeout.count())) != 1) {
        return UniqueFd();
    }
    return UniqueFd(::accept4(listen_fd, nullptr, nullptr, SOCK_CLOEXEC));
}

/// themes/a, themes/b, state/current → ../themes/a, cfg/current → ../state/current
void make_rice_layout(const fs::path &root) {
    fs::create_directories(root / "themes/a");
    fs::create_directories(root / "themes/b");
    fs::create_directories(root / "state");
    fs::create_directories(root / "cfg");
    fs::create_symlink("../themes/a", root / "state/current");
    fs::create_symlink("../state/current", root / "cfg/current");
}

/// Atomically points `link` at `target` (what `ln -sfn` + rename does).
void swap_link(const fs::path &link, const fs::path &target) {
    const fs::path tmp = link.string() + ".new";
    fs::create_symlink(target, tmp);
    fs::rename(tmp, link);
}

} // namespace

TEST(Hyprland, ParseSubmapLines) {
    EXPECT_EQ(parse_socket2_line("submap>>resize"), Event{event::Submap{"resize"}});
    EXPECT_EQ(parse_socket2_line("submap>>"), Event{event::Submap{std::nullopt}});
    EXPECT_EQ(parse_socket2_line("activewindow>>kitty,~"), std::nullopt);
    EXPECT_EQ(parse_socket2_line("submapx>>resize"), std::nullopt);
    EXPECT_EQ(parse_socket2_line(""), std::nullopt);
}

TEST(Hyprland, Socket2DisconnectClearsSubmapAndReconnects) {
    TempDir d;
    const fs::path path = d.path() / ".socket2.sock";
    auto listener = bind_socket(path);
    ASSERT_TRUE(listener.has_value()) << listener.error().message();
    EventLoop loop;
    Events events;
    HyprlandSource src(loop, [path] { return std::vector{path}; }, events.sink());
    LoopThread running(loop);

    UniqueFd first = accept_within(listener->get(), 2000ms);
    ASSERT_TRUE(first) << "connects at start";
    const std::string lines = "activewindow>>kitty,~\nsubmap>>resize\n";
    ASSERT_EQ(::write(first.get(), lines.data(), lines.size()), static_cast<ssize_t>(lines.size()));
    ASSERT_TRUE(wait_until([&] { return events.size() >= 1; }, 2000ms));
    first.reset(); // Hyprland goes away
    const auto dropped = Clock::now();

    UniqueFd second = accept_within(listener->get(), 3000ms);
    EXPECT_TRUE(second) << "reconnects within 3 s";
    EXPECT_GE(Clock::now() - dropped, 800ms) << "after the 1 s backoff, not in a loop";
    EXPECT_EQ(events.get(),
              (std::vector<Event>{event::Submap{"resize"}, event::Submap{std::nullopt}}));
}

// A socket file outlives a crashed Hyprland, so each candidate is tried until one accepts.
TEST(Hyprland, TriesEachSocket2CandidateUntilOneAccepts) {
    TempDir d;
    const fs::path dead = d.path() / "dead/.socket2.sock";
    make_dead_socket(dead);
    const fs::path live = d.path() / "live/.socket2.sock";
    fs::create_directories(live.parent_path());
    auto listener = bind_socket(live);
    ASSERT_TRUE(listener.has_value()) << listener.error().message();
    EventLoop loop;
    Events events;
    HyprlandSource src(loop, [dead, live] { return std::vector{dead, live}; }, events.sink());
    LoopThread running(loop);

    UniqueFd conn = accept_within(listener->get(), 2000ms);
    ASSERT_TRUE(conn) << "connects to the live candidate at start";
    const std::string line = "submap>>resize\n";
    ASSERT_EQ(::write(conn.get(), line.data(), line.size()), static_cast<ssize_t>(line.size()));
    ASSERT_TRUE(wait_until([&] { return events.size() >= 1; }, 2000ms));
    EXPECT_EQ(events.get(), std::vector<Event>{event::Submap{"resize"}});
}

TEST(Hyprland, Socket2CandidatesPutTheSignatureFirstThenTheNewest) {
    TempDir rt;
    const auto sock = [&rt](std::string_view sig) {
        return rt.path() / "hypr" / sig / ".socket2.sock";
    };
    const auto now = fs::file_time_type::clock::now();
    write_file(sock("OLD"), "");
    write_file(sock("MID"), "");
    write_file(sock("NEW"), "");
    fs::last_write_time(sock("OLD"), now - 3h);
    fs::last_write_time(sock("MID"), now - 2h);
    fs::last_write_time(sock("NEW"), now - 1h);
    fs::create_directories(rt.path() / "hypr/EMPTY"); // an instance directory without a socket
    const ScopedEnv runtime("XDG_RUNTIME_DIR", rt.path().string());
    {
        const ScopedEnv sig("HYPRLAND_INSTANCE_SIGNATURE", "OLD");
        EXPECT_EQ(socket2_paths(), (std::vector{sock("OLD"), sock("NEW"), sock("MID")}));
    }
    {
        const ScopedEnv sig("HYPRLAND_INSTANCE_SIGNATURE", std::nullopt);
        EXPECT_EQ(socket2_paths(), (std::vector{sock("NEW"), sock("MID"), sock("OLD")}));
    }
    {
        const ScopedEnv sig("HYPRLAND_INSTANCE_SIGNATURE", "GONE"); // its directory is gone
        EXPECT_EQ(socket2_paths(), (std::vector{sock("NEW"), sock("MID"), sock("OLD")}));
    }
}

// The review's case: a crashed Hyprland left OLD's socket file, the daemon's environment still
// names OLD, and NEW is the live instance.
TEST(Hyprland, StaleSignatureFallsBackToTheLiveInstance) {
    TempDir rt;
    make_dead_socket(rt.path() / "hypr/OLD/.socket2.sock");
    const fs::path live = rt.path() / "hypr/NEW/.socket2.sock";
    fs::create_directories(live.parent_path());
    auto listener = bind_socket(live);
    ASSERT_TRUE(listener.has_value()) << listener.error().message();
    const ScopedEnv runtime("XDG_RUNTIME_DIR", rt.path().string());
    const ScopedEnv sig("HYPRLAND_INSTANCE_SIGNATURE", "OLD");
    EventLoop loop;
    Events events;
    HyprlandSource src(loop, socket2_paths, events.sink());
    LoopThread running(loop);
    EXPECT_TRUE(accept_within(listener->get(), 2000ms)) << "the live instance is found";
}

TEST(Theme, LinkChainFollowsRiceLayout) {
    TempDir d;
    make_rice_layout(d.path());
    EXPECT_EQ(link_chain(d.path() / "cfg/current"),
              (std::vector<fs::path>{d.path() / "cfg/current", d.path() / "state/current",
                                     d.path() / "themes/a"}));
    EXPECT_EQ(link_chain(d.path() / "themes/b"), (std::vector<fs::path>{d.path() / "themes/b"}));
}

TEST(Theme, ThemeSwapEmitsEvent) {
    TempDir d;
    make_rice_layout(d.path());
    EventLoop loop;
    Events events;
    ThemeWatcher watcher(loop, d.path() / "cfg/current", events.sink());
    LoopThread running(loop);

    swap_link(d.path() / "state/current", "../themes/b");
    ASSERT_TRUE(wait_until([&] { return events.size() == 1; }, 1000ms)) << "theme swap seen";
    EXPECT_EQ(events.get().front(), Event{event::ThemeChanged{}});

    // The watches were rebuilt for the new chain: swapping back is seen too, once.
    swap_link(d.path() / "state/current", "../themes/a");
    EXPECT_TRUE(wait_until([&] { return events.size() == 2; }, 1000ms)) << "second swap seen";
    std::this_thread::sleep_for(400ms);
    EXPECT_EQ(events.size(), 2u) << "one event per swap (debounced)";
}

TEST(Hwmon, FindsPackageTemp) {
    TempDir d;
    const fs::path hw = d.path() / "class/hwmon";
    write_file(hw / "hwmon1/name", "acpitz\n");
    write_file(hw / "hwmon1/temp1_input", "30000\n");
    write_file(hw / "hwmon3/name", "coretemp\n");
    write_file(hw / "hwmon3/temp2_label", "Core 0\n");
    write_file(hw / "hwmon3/temp2_input", "41000\n");
    write_file(hw / "hwmon3/temp1_label", "Package id 0\n");
    write_file(hw / "hwmon3/temp1_input", "45000\n");
    const auto p = find_package_temp(d.path());
    ASSERT_EQ(p, hw / "hwmon3/temp1_input");
    EXPECT_EQ(read_temp_c(*p), 45.0);
    EXPECT_EQ(read_temp_c(hw / "hwmon3/nope"), std::nullopt);
    EXPECT_EQ(find_package_temp(d.path() / "nothing"), std::nullopt);
}

TEST(Hwmon, PollerEmitsCpuTemp) {
    TempDir d;
    const fs::path hw = d.path() / "class/hwmon/hwmon8";
    write_file(hw / "name", "coretemp\n");
    write_file(hw / "temp1_label", "Package id 0\n");
    write_file(hw / "temp1_input", "52500\n");
    EventLoop loop;
    Events events;
    HwmonPoller poller(loop, d.path(), events.sink());
    LoopThread running(loop);
    ASSERT_TRUE(wait_until([&] { return events.size() >= 1; }, 1000ms)) << "first reading at once";
    EXPECT_EQ(events.get().front(), Event{FactUpdate{fact::CpuTemp{52.5}}});
}

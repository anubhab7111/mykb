// Event sources in real time: Hyprland socket2 (fake listener), theme symlink watcher (temp
// tree), hwmon (fake sysfs), system D-Bus (a private dbus-daemon with a fake logind).

#include "helpers.hpp"

#include "ipc/socket.hpp"
#include "sources/hwmon.hpp"
#include "sources/hyprland.hpp"
#include "sources/hyprland_bridge.hpp"
#include "sources/system_dbus.hpp"
#include "sources/theme.hpp"

#include <sdbus-c++/sdbus-c++.h>

#include <gtest/gtest.h>

#include <atomic>
#include <csignal>
#include <expected>
#include <memory>
#include <mutex>
#include <poll.h>
#include <spawn.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>
#include <utility>

using namespace omenkbd;
using namespace omenkbd::test;
using namespace std::chrono_literals;
namespace fs = std::filesystem;

namespace {

/// Collects what a source emitted on the loop thread.
template <class T> class Collector {
  public:
    std::function<void(const T &)> sink() {
        return [this](const T &e) {
            const std::lock_guard lock(mu_);
            events_.push_back(e);
        };
    }
    std::vector<T> get() const {
        const std::lock_guard lock(mu_);
        return events_;
    }
    std::size_t size() const { return get().size(); }

  private:
    mutable std::mutex mu_;
    std::vector<T> events_;
};

using Events = Collector<Event>;
using HyprEvents = Collector<HyprEvent>;

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

/// A private dbus-daemon listening in `dir`, stopped by stop() or the destructor. Not ok() when
/// dbus-daemon is not installed or does not come up.
class PrivateBus {
  public:
    explicit PrivateBus(const fs::path &dir)
        : socket_(dir / "bus"), address_("unix:path=" + socket_.string()) {
        const std::string addr_arg = "--address=" + address_;
        std::array<const char *, 6> argv{"dbus-daemon", "--session",      "--nofork",
                                         "--nopidfile", addr_arg.c_str(), nullptr};
        if (::posix_spawnp(&pid_, "dbus-daemon", nullptr, nullptr,
                           const_cast<char *const *>(argv.data()), environ) != 0) {
            pid_ = -1;
            return;
        }
        ok_ = wait_until([this] { return fs::exists(socket_); }, 5000ms);
    }
    ~PrivateBus() { stop(); }
    PrivateBus(const PrivateBus &) = delete;
    PrivateBus &operator=(const PrivateBus &) = delete;

    [[nodiscard]] bool ok() const { return ok_; }
    [[nodiscard]] const std::string &address() const { return address_; }
    void stop() {
        if (pid_ > 0) {
            ::kill(pid_, SIGTERM);
            ::waitpid(pid_, nullptr, 0);
            pid_ = -1;
        }
    }

  private:
    fs::path socket_;
    std::string address_;
    pid_t pid_ = -1;
    bool ok_ = false;
};

/// Owns org.freedesktop.login1 on `bus` and sends PrepareForSleep, as logind does.
class FakeLogind {
  public:
    explicit FakeLogind(const std::string &address)
        : conn_(sdbus::createSessionBusConnectionWithAddress(address)) {
        conn_->requestName(sdbus::ServiceName{"org.freedesktop.login1"});
        obj_ = sdbus::createObject(*conn_, sdbus::ObjectPath{"/org/freedesktop/login1"});
    }
    void prepare_for_sleep(bool entering) {
        obj_->emitSignal("PrepareForSleep")
            .onInterface("org.freedesktop.login1.Manager")
            .withArguments(entering);
    }

  private:
    std::unique_ptr<sdbus::IConnection> conn_;
    std::unique_ptr<sdbus::IObject> obj_;
};

std::vector<event::Sleep> sleeps(const Events &events) {
    std::vector<event::Sleep> out;
    for (const Event &e : events.get()) {
        if (const auto *s = std::get_if<event::Sleep>(&e)) {
            out.push_back(*s);
        }
    }
    return out;
}

} // namespace

TEST(Hyprland, ParseSubmapLines) {
    EXPECT_EQ(parse_socket2_line("submap>>resize"), HyprEvent{hypr::Submap{"resize"}});
    EXPECT_EQ(parse_socket2_line("submap>>"), HyprEvent{hypr::Submap{std::nullopt}});
    EXPECT_EQ(parse_socket2_line("activewindow>>kitty,~"), HyprEvent{hypr::ActiveWindow{"kitty"}});
    EXPECT_EQ(parse_socket2_line("windowtitle>>kitty,~"), std::nullopt);
    EXPECT_EQ(parse_socket2_line("submapx>>resize"), std::nullopt);
    EXPECT_EQ(parse_socket2_line(""), std::nullopt);
}

TEST(Hyprland, Socket2ConnectAndDisconnectAreEventsAndItReconnects) {
    TempDir d;
    const fs::path path = d.path() / ".socket2.sock";
    auto listener = bind_socket(path);
    ASSERT_TRUE(listener.has_value()) << listener.error().message();
    EventLoop loop;
    HyprEvents events;
    HyprlandSource src(loop, [path] { return std::vector{path}; }, events.sink());
    EXPECT_EQ(src.connected_path(), path) << "the connect is synchronous";
    EXPECT_EQ(events.get(), (std::vector<HyprEvent>{hypr::Connected{}}));
    LoopThread running(loop);

    UniqueFd first = accept_within(listener->get(), 2000ms);
    ASSERT_TRUE(first) << "connects at start";
    const std::string lines = "activewindow>>kitty,~\nsubmap>>resize\n";
    ASSERT_EQ(::write(first.get(), lines.data(), lines.size()), static_cast<ssize_t>(lines.size()));
    ASSERT_TRUE(wait_until([&] { return events.size() >= 3; }, 2000ms));
    first.reset(); // Hyprland goes away
    const auto dropped = Clock::now();
    ASSERT_TRUE(wait_until([&] { return events.size() >= 4; }, 2000ms));
    EXPECT_EQ(src.connected_path(), std::nullopt) << "no socket is in use after Disconnected";

    UniqueFd second = accept_within(listener->get(), 3000ms);
    EXPECT_TRUE(second) << "reconnects within 3 s";
    EXPECT_GE(Clock::now() - dropped, 800ms) << "after the 1 s backoff, not in a loop";
    ASSERT_TRUE(wait_until([&] { return events.size() >= 5; }, 2000ms));
    EXPECT_EQ(events.get(), (std::vector<HyprEvent>{hypr::Connected{}, hypr::ActiveWindow{"kitty"},
                                                    hypr::Submap{"resize"}, hypr::Disconnected{},
                                                    hypr::Connected{}}));
    EXPECT_EQ(src.connected_path(), path);
}

TEST(Hyprland, NoSocketToConnectToMeansNoPathAndNoEvents) {
    TempDir d;
    const fs::path dead = d.path() / "dead/.socket2.sock";
    make_dead_socket(dead);
    EventLoop loop;
    HyprEvents events;
    const HyprlandSource src(loop, [dead] { return std::vector{dead}; }, events.sink());
    EXPECT_EQ(src.connected_path(), std::nullopt);
    EXPECT_EQ(events.size(), 0u) << "no Disconnected for a connection that never was";
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
    HyprEvents events;
    HyprlandSource src(loop, [dead, live] { return std::vector{dead, live}; }, events.sink());
    EXPECT_EQ(src.connected_path(), live) << "the path in use is the one that accepted";
    LoopThread running(loop);

    UniqueFd conn = accept_within(listener->get(), 2000ms);
    ASSERT_TRUE(conn) << "connects to the live candidate at start";
    const std::string line = "submap>>resize\n";
    ASSERT_EQ(::write(conn.get(), line.data(), line.size()), static_cast<ssize_t>(line.size()));
    ASSERT_TRUE(wait_until([&] { return events.size() >= 2; }, 2000ms));
    EXPECT_EQ(events.get(), (std::vector<HyprEvent>{hypr::Connected{}, hypr::Submap{"resize"}}));
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
    HyprEvents events;
    HyprlandSource src(loop, socket2_paths, events.sink());
    EXPECT_EQ(src.connected_path(), live);
    LoopThread running(loop);
    EXPECT_TRUE(accept_within(listener->get(), 2000ms)) << "the live instance is found";
}

namespace {

void send(const UniqueFd &conn, const std::string &lines) {
    ASSERT_EQ(::write(conn.get(), lines.data(), lines.size()), static_cast<ssize_t>(lines.size()));
}

/// Calls `bridge.configure(reactions_on)` on the loop thread and waits until it ran.
void configure_on_loop(EventLoop &loop, HyprlandBridge &bridge, bool reactions_on) {
    auto done = std::make_shared<std::atomic<bool>>(false);
    loop.post([&bridge, reactions_on, done] {
        bridge.configure(reactions_on);
        *done = true;
    });
    ASSERT_TRUE(wait_until([done] { return done->load(); }, 2000ms));
}

template <class S> Event sig(S s) { return Event{Signal{std::move(s)}}; }

Event fullscreen_cleared() { return Event{FactUpdate{fact::FullscreenApp{std::nullopt}}}; }

/// A fake Hyprland: a listener the bridge connects to.
struct FakeHyprland {
    TempDir dir;
    fs::path path = dir.path() / ".socket2.sock";
    std::expected<UniqueFd, IpcError> listener = bind_socket(path);

    [[nodiscard]] HyprlandSource::PathFn paths() const {
        return [p = path] { return std::vector{p}; };
    }
};

} // namespace

TEST(HyprlandBridge, ForwardsSignalsOnlyWhileReactionsAreOn) {
    FakeHyprland hypr_sock;
    ASSERT_TRUE(hypr_sock.listener.has_value()) << hypr_sock.listener.error().message();
    EventLoop loop;
    Events events;
    HyprlandBridge bridge(loop, hypr_sock.paths(), events.sink());
    LoopThread running(loop);
    UniqueFd conn = accept_within(hypr_sock.listener->get(), 2000ms);
    ASSERT_TRUE(conn);

    // Off until configured: the pulse is dropped, the submap passes.
    send(conn, "openwindow>>abc,1,Alacritty,t\nsubmap>>resize\n");
    ASSERT_TRUE(wait_until([&] { return events.size() >= 1; }, 2000ms));
    EXPECT_EQ(events.get(), (std::vector<Event>{event::Submap{"resize"}}));

    configure_on_loop(loop, bridge, true);
    send(conn, "openwindow>>abc,1,Alacritty,t\nurgent>>def\nclosewindow>>abc\n");
    ASSERT_TRUE(wait_until([&] { return events.size() >= 4; }, 2000ms));
    EXPECT_EQ(events.get(),
              (std::vector<Event>{event::Submap{"resize"}, sig(signal::WindowOpened{}),
                                  sig(signal::UrgentRaised{"def"}), sig(signal::WindowClosed{})}));

    configure_on_loop(loop, bridge, false);
    send(conn, "closewindow>>def\nsubmap>>\n");
    ASSERT_TRUE(wait_until([&] { return events.size() >= 5; }, 2000ms));
    EXPECT_EQ(events.get().back(), Event{event::Submap{std::nullopt}});
    EXPECT_EQ(events.size(), 5u) << "no WindowClosed or UrgentCleared while reactions are off";
}

// The real order of a focus change is activewindow, activewindowv2, windowtitle, windowtitlev2; a
// title change repeats all four for the same window. One FocusChanged results, and no title is
// in the stream the daemon sees.
TEST(HyprlandBridge, ARealFocusSequenceBecomesOneSignalPerChange) {
    FakeHyprland hypr_sock;
    ASSERT_TRUE(hypr_sock.listener.has_value()) << hypr_sock.listener.error().message();
    EventLoop loop;
    Events events;
    HyprlandBridge bridge(loop, hypr_sock.paths(), events.sink());
    bridge.configure(true); // the loop is not running yet
    LoopThread running(loop);
    UniqueFd conn = accept_within(hypr_sock.listener->get(), 2000ms);
    ASSERT_TRUE(conn);

    send(conn, "workspacev2>>1,1\nworkspacev2>>2,2\n"
               "activewindow>>kitty,Title, with >> and ,commas\nactivewindowv2>>0xAB\n"
               "windowtitle>>0xAB,Title\nwindowtitlev2>>0xAB,Title\n"
               "activewindow>>kitty,A new title\nactivewindowv2>>0xAB\n"
               "windowtitle>>0xAB,A new title\nwindowtitlev2>>0xAB,A new title\n"
               "activewindow>>mpv,video\nactivewindowv2>>cd\n");
    ASSERT_TRUE(wait_until([&] { return events.size() >= 3; }, 2000ms));
    EXPECT_EQ(events.get(), (std::vector<Event>{sig(signal::WorkspaceSwitched{1, 2}),
                                                sig(signal::FocusChanged{"ab", "kitty"}),
                                                sig(signal::FocusChanged{"cd", "mpv"})}));
    std::this_thread::sleep_for(100ms);
    EXPECT_EQ(events.size(), 3u) << "nothing more arrives";
}

TEST(HyprlandBridge, SubmapAndDisconnectBehaveAsBefore) {
    FakeHyprland hypr_sock;
    ASSERT_TRUE(hypr_sock.listener.has_value()) << hypr_sock.listener.error().message();
    EventLoop loop;
    Events events;
    HyprlandBridge bridge(loop, hypr_sock.paths(), events.sink());
    LoopThread running(loop);

    UniqueFd first = accept_within(hypr_sock.listener->get(), 2000ms);
    ASSERT_TRUE(first) << "connects at start";
    send(first, "activewindow>>kitty,~\nsubmap>>resize\n");
    ASSERT_TRUE(wait_until([&] { return events.size() >= 1; }, 2000ms));
    first.reset(); // Hyprland goes away
    const auto dropped = Clock::now();
    ASSERT_TRUE(wait_until([&] { return events.size() >= 3; }, 2000ms));
    // No submap color sticks, and the fullscreen fact is cleared. UrgentReset is a signal, so it
    // stays out while reactions are off.
    EXPECT_EQ(events.get(), (std::vector<Event>{event::Submap{"resize"}, fullscreen_cleared(),
                                                event::Submap{std::nullopt}}));

    UniqueFd second = accept_within(hypr_sock.listener->get(), 3000ms);
    ASSERT_TRUE(second) << "reconnects within 3 s";
    EXPECT_GE(Clock::now() - dropped, 800ms) << "after the 1 s backoff, not in a loop";
    send(second, "submap>>game\n");
    ASSERT_TRUE(wait_until([&] { return events.size() >= 4; }, 2000ms));
    EXPECT_EQ(events.get().back(), Event{event::Submap{"game"}});
}

// Review focus 5: a lamp must not stay lit when Hyprland goes away.
TEST(HyprlandBridge, DisconnectResetsUrgentWhileReactionsAreOn) {
    FakeHyprland hypr_sock;
    ASSERT_TRUE(hypr_sock.listener.has_value()) << hypr_sock.listener.error().message();
    EventLoop loop;
    Events events;
    HyprlandBridge bridge(loop, hypr_sock.paths(), events.sink());
    bridge.configure(true); // the loop is not running yet
    LoopThread running(loop);

    UniqueFd first = accept_within(hypr_sock.listener->get(), 2000ms);
    ASSERT_TRUE(first);
    send(first, "urgent>>abc\n");
    ASSERT_TRUE(wait_until([&] { return events.size() >= 1; }, 2000ms));
    first.reset();
    ASSERT_TRUE(wait_until([&] { return events.size() >= 4; }, 2000ms));
    EXPECT_EQ(events.get(),
              (std::vector<Event>{sig(signal::UrgentRaised{"abc"}), sig(signal::UrgentReset{}),
                                  fullscreen_cleared(), event::Submap{std::nullopt}}));
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

// Losing the system bus between PrepareForSleep(true) and (false) would leave the daemon
// suspended for good (no frames, no writes); the source ends the sleep when it gives up.
TEST(SystemDbus, BusLostDuringSleepEndsTheSleep) {
    TempDir dir;
    PrivateBus bus(dir.path());
    if (!bus.ok()) {
        GTEST_SKIP() << "dbus-daemon is not available";
    }
    const ScopedEnv env("DBUS_SYSTEM_BUS_ADDRESS", bus.address());
    FakeLogind logind(bus.address());
    EventLoop loop;
    Events events;
    SystemDbusSource source(loop, events.sink());
    const LoopThread running(loop);
    // The source subscribes asynchronously: repeat the signal until it arrives.
    ASSERT_TRUE(wait_until(
        [&] {
            logind.prepare_for_sleep(true);
            return !sleeps(events).empty();
        },
        5000ms));
    ASSERT_EQ(sleeps(events).back(), event::Sleep{true});
    bus.stop();
    EXPECT_TRUE(wait_until([&] { return sleeps(events).back() == event::Sleep{false}; }, 5000ms))
        << "the daemon would stay suspended";
}

TEST(SystemDbus, BusLostWhileAwakeSendsNoSleepEvent) {
    TempDir dir;
    PrivateBus bus(dir.path());
    if (!bus.ok()) {
        GTEST_SKIP() << "dbus-daemon is not available";
    }
    const ScopedEnv env("DBUS_SYSTEM_BUS_ADDRESS", bus.address());
    FakeLogind logind(bus.address());
    EventLoop loop;
    Events events;
    SystemDbusSource source(loop, events.sink());
    const LoopThread running(loop);
    ASSERT_TRUE(wait_until(
        [&] {
            logind.prepare_for_sleep(true);
            return !sleeps(events).empty();
        },
        5000ms));
    logind.prepare_for_sleep(false);
    ASSERT_TRUE(wait_until([&] { return sleeps(events).back() == event::Sleep{false}; }, 5000ms));
    const std::size_t n = sleeps(events).size();
    bus.stop();
    std::this_thread::sleep_for(500ms);
    EXPECT_EQ(sleeps(events).size(), n) << "no extra wake when the bus goes away while awake";
}

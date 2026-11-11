// Integration tests: the real daemon loop against the mock backend, over the real socket, in real
// time. Each test runs Daemon::run() on its own thread and stops it in TearDown.

#include "helpers.hpp"
#include "reaction_rig.hpp"

#include "backend/mock.hpp"
#include "core/effects.hpp"
#include "daemon/daemon.hpp"
#include "ipc/socket.hpp"

#include <gtest/gtest.h>

#include <atomic>
#include <ctime>
#include <future>
#include <pthread.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <thread>
#include <unistd.h>

using namespace omenkbd;
using namespace omenkbd::test;
using namespace std::chrono_literals;

namespace {

constexpr std::string_view kStaticCfg = R"(
[hints]
timeout_s = 1
[hints.modifiers]
super = "#0000ff"
[[profiles]]
name = "plain"
colors = ["#101010", "#202020", "#303030", "#404040"]
)";

constexpr std::string_view kWaveCfg = R"(
[effects]
enabled = true
fps = 10
[hints]
timeout_s = 1
[[profiles]]
name = "wavy"
colors = ["#ff0000", "#00ff00", "#0000ff", "#ffffff"]
effect = "wave"
speed = 50
)";

Rgb rgb(std::string_view hex) { return Rgb::parse(hex).value(); }

std::array<Rgb, 4> all(Rgb c) { return {c, c, c, c}; }

Request set(std::vector<std::string> colors) { return req::Set{std::move(colors)}; }

/// `text` with its first `from` replaced by `to`.
std::string replaced_once(std::string_view text, std::string_view from, std::string_view to) {
    std::string out(text);
    const std::size_t at = out.find(from);
    EXPECT_NE(at, std::string::npos) << from;
    if (at != std::string::npos) {
        out.replace(at, from.size(), to);
    }
    return out;
}

// The colors of kRigConfig (tests/reaction_rig.hpp), whose first profile `plain` is static: the
// reaction tests below give the daemon the stimuli of the write-count tests and expect the frames
// the rig counts there.
constexpr Rgb kL{0x10, 0x10, 0x10};
constexpr Rgb kM{0x20, 0x20, 0x20};
constexpr Rgb kR{0x30, 0x30, 0x30};
constexpr Rgb kW{0x40, 0x40, 0x40};
constexpr Frame kBase{kL, kM, kR, kW};
constexpr Rgb kZapZap{0x20, 0xff, 0x40};
constexpr Rgb kUrgent{0xff, 0xe0, 0x20};
constexpr Rgb kWaiting{0xff, 0x90, 0x40};

/// `frame` with `zones` painted `color`.
Frame paint(Frame frame, std::initializer_list<Zone> zones, Rgb color) {
    for (Zone z : zones) {
        frame[index(z)] = color;
    }
    return frame;
}

/// `frame` with every zone moved half way toward white, as a window opening lifts it.
Frame lifted(Frame frame) {
    for (Rgb &z : frame) {
        z = Rgb::lerp(z, kWhite, 0.5);
    }
    return frame;
}

/// `frame` with every zone dipped to 35%, as a window closing dims it.
Frame dipped(Frame frame) {
    for (Rgb &z : frame) {
        z = multiply(z, 0.35);
    }
    return frame;
}

/// `frame` at brightness `pct`.
Frame scaled(Frame frame, std::uint8_t pct) {
    for (Rgb &z : frame) {
        z = z.scale(pct);
    }
    return frame;
}

/// What the daemon writes for a logical frame: the zones in hardware order (the default map).
std::array<Rgb, 4> hw(const Frame &logical) { return ZoneMap{}.to_hw(logical); }

std::vector<std::array<Rgb, 4>> hw_frames(std::initializer_list<Frame> logical) {
    std::vector<std::array<Rgb, 4>> out;
    for (const Frame &f : logical) {
        out.push_back(hw(f));
    }
    return out;
}

class DaemonTest : public ::testing::Test {
  protected:
    void start(std::string_view cfg_toml) {
        write_file(config_path(), cfg_toml);
        Config config = parse_config(cfg_toml).value();
        const std::string first = config.profiles.front().name;
        Model model(std::move(config), Palette{}, PersistedState::default_for(first), "mock");
        auto [backend, handle] = make_mock(false);
        h_.emplace(std::move(handle));
        auto listen = bind_socket(sock());
        ASSERT_TRUE(listen.has_value()) << listen.error().message();
        RunOptions opts{.backend = BackendKind::Mock,
                        .sys_root = dir_.path(),
                        .config_path = config_path(),
                        .state_path = state_path(),
                        .socket_path = sock(),
                        .mock_render = false,
                        .with_sources = false};
        daemon_ = std::make_unique<Daemon>(std::move(model), std::move(backend), std::move(*listen),
                                           std::move(opts));
        if (before_run_) {
            before_run_(*daemon_);
        }
        thread_ = std::thread([this] { rc_ = daemon_->run(); });
        ASSERT_TRUE(wait_until([this] { return !zone_writes().empty(); }, 2000ms));
    }

    void stop_daemon() {
        if (thread_.joinable()) {
            daemon_->stop();
            thread_.join();
            EXPECT_EQ(rc_.load(), 0);
        }
    }

    void TearDown() override { stop_daemon(); }

    [[nodiscard]] MockHandle &h() { return *h_; }

    [[nodiscard]] std::vector<std::array<Rgb, 4>> zone_writes() const {
        std::vector<std::array<Rgb, 4>> out;
        for (const MockOp &op : h_->ops()) {
            if (op.kind == MockOp::Kind::Zones) {
                out.push_back(op.zones);
            }
        }
        return out;
    }

    /// The zone writes after the first `from`.
    [[nodiscard]] std::vector<std::array<Rgb, 4>> zone_writes_since(std::size_t from) const {
        const auto writes = zone_writes();
        const auto skip = static_cast<std::ptrdiff_t>(std::min(from, writes.size()));
        return {writes.begin() + skip, writes.end()};
    }

    /// Keeps the writer inside one slow zone write for `d` (kRigConfig's fullscreen rule dims the
    /// frame to 15%), so a backlight read asked for meanwhile waits behind it. Later writes are
    /// quick again.
    void stall_writer(std::chrono::milliseconds d) {
        h().set_delay(d);
        daemon_->post(FactUpdate{fact::FullscreenApp{"mpv"}});
        std::this_thread::sleep_for(100ms); // the writer is inside the slow write
        h().set_delay(0ms);
    }

    /// The CPU time the daemon's loop thread has used so far.
    [[nodiscard]] std::chrono::nanoseconds loop_cpu_time() {
        clockid_t clock{};
        timespec ts{};
        if (::pthread_getcpuclockid(thread_.native_handle(), &clock) != 0 ||
            ::clock_gettime(clock, &ts) != 0) {
            ADD_FAILURE() << "cannot read the loop thread's CPU clock";
            return {};
        }
        return std::chrono::seconds(ts.tv_sec) + std::chrono::nanoseconds(ts.tv_nsec);
    }

    Response req(const Request &r) {
        auto resp = request(sock(), r);
        if (!resp) {
            ADD_FAILURE() << "daemon unreachable: " << resp.error().message();
            return Response::failure(resp.error().message());
        }
        return *resp;
    }

    Status status() { return req(req::Status{}).status.value(); }

    /// Writes `out` on a fresh connection and reads `n` replies (2 s timeout per read).
    std::vector<Response> raw_exchange(std::string_view out, std::size_t n) {
        UniqueFd fd(::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0));
        sockaddr_un addr{};
        addr.sun_family = AF_UNIX;
        std::strncpy(addr.sun_path, sock().c_str(), sizeof(addr.sun_path) - 1);
        if (::connect(fd.get(), reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) != 0) {
            ADD_FAILURE() << "connect: " << std::strerror(errno);
            return {};
        }
        timeval tv{.tv_sec = 2, .tv_usec = 0};
        ::setsockopt(fd.get(), SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
        if (::write(fd.get(), out.data(), out.size()) != static_cast<ssize_t>(out.size())) {
            ADD_FAILURE() << "short write";
            return {};
        }
        LineBuffer lines;
        std::vector<Response> got;
        char buf[4096];
        while (got.size() < n) {
            if (auto line = lines.next_line()) {
                got.push_back(decode_response(*line).value());
                continue;
            }
            const ssize_t k = ::read(fd.get(), buf, sizeof(buf));
            if (k <= 0) {
                ADD_FAILURE() << "connection closed or timed out after " << got.size()
                              << " replies";
                return got;
            }
            lines.append({buf, static_cast<std::size_t>(k)});
        }
        return got;
    }

    [[nodiscard]] std::filesystem::path config_path() const { return dir_.path() / "config.toml"; }
    [[nodiscard]] std::filesystem::path state_path() const { return dir_.path() / "state.json"; }
    [[nodiscard]] std::filesystem::path sock() const { return dir_.path() / "omen-kbd.sock"; }

    TempDir dir_;
    std::optional<MockHandle> h_;
    std::unique_ptr<Daemon> daemon_;
    std::thread thread_;
    std::atomic<int> rc_{-1};
    /// Runs on the test thread before the daemon thread starts.
    std::function<void(Daemon &)> before_run_;
};

} // namespace

TEST_F(DaemonTest, StartupAppliesTheProfile) {
    start(kStaticCfg);
    // logical [10,20,30,40] → hardware [right, middle, left, wasd]
    EXPECT_EQ(zone_writes(), (std::vector<std::array<Rgb, 4>>{
                                 {rgb("303030"), rgb("202020"), rgb("101010"), rgb("404040")}}));
    EXPECT_EQ(h().ops().front(), MockOp::backlight_op(true));
}

TEST_F(DaemonTest, SetRequestWritesOnceInHwOrderAndRepliesOk) {
    start(kStaticCfg);
    const auto before = zone_writes().size();
    const Response r = req(set({"#ff0000", "#00ff00", "#0000ff", "#ffffff"}));
    EXPECT_TRUE(r.ok) << r.error.value_or("");
    const auto zones = zone_writes();
    ASSERT_EQ(zones.size(), before + 1) << "exactly one write";
    EXPECT_EQ(zones.back(),
              (std::array<Rgb, 4>{rgb("0000ff"), rgb("00ff00"), rgb("ff0000"), rgb("ffffff")}));
}

TEST_F(DaemonTest, BurstOf100SetsCoalescesToLatest) {
    start(kStaticCfg);
    h().set_delay(20ms);
    std::vector<std::thread> clients;
    std::atomic<int> ok{0};
    for (int i = 0; i < 100; ++i) {
        clients.emplace_back([this, i, &ok] {
            const auto r = request(sock(), set({std::format("#{:02x}{:02x}00", i, 255 - i)}));
            if (r && r->ok) {
                ++ok;
            }
        });
    }
    for (auto &t : clients) {
        t.join();
    }
    EXPECT_EQ(ok.load(), 100);
    const auto writes = zone_writes();
    EXPECT_LE(writes.size(), 21u) << writes.size() << " writes for 100 sets";
    const auto shown = status().zones;
    const std::array<Rgb, 4> hw{rgb(shown[2]), rgb(shown[1]), rgb(shown[0]), rgb(shown[3])};
    EXPECT_EQ(writes.back(), hw) << "the keyboard ends where the daemon says it is";
}

TEST_F(DaemonTest, WriteErrorReachesCliResponse) {
    start(kStaticCfg);
    h().fail_with(BackendError{BackendError::Kind::Io, "boom"});
    const Response r = req(set({"#ff0000"}));
    EXPECT_FALSE(r.ok);
    EXPECT_NE(r.error.value_or("").find("boom"), std::string::npos) << r.error.value_or("");
}

TEST_F(DaemonTest, FailedWriteIsRetriedOnNextChangeNotInALoop) {
    start(kStaticCfg);
    const auto before = zone_writes().size();
    h().fail_with(BackendError{BackendError::Kind::Io, "eacces"});
    EXPECT_FALSE(req(set({"#ff0000"})).ok);
    h().fail_with(std::nullopt);
    std::this_thread::sleep_for(300ms);
    EXPECT_EQ(zone_writes().size(), before) << "no retry loop after a failure";
    const Response r = req(req::Brightness{"50"});
    EXPECT_TRUE(r.ok) << r.error.value_or("");
    EXPECT_EQ(zone_writes().back(), all(rgb("800000")));
}

TEST_F(DaemonTest, OffTurnsBacklightOffWithoutZoneWrite) {
    start(kStaticCfg);
    auto expected = h().ops();
    const Response r = req(req::Power{req::PowerAction::Off});
    EXPECT_TRUE(r.ok) << r.error.value_or("");
    expected.push_back(MockOp::backlight_op(false));
    EXPECT_EQ(h().ops(), expected);
}

// The Fn key changes the firmware backlight behind the daemon's back (nothing reports it while a
// static profile shows), so `on` must write the backlight even when the daemon believes it is on.
TEST_F(DaemonTest, OnAfterFnKeyOffWritesTheBacklight) {
    start(kStaticCfg);
    h().set_hw_backlight(false); // the Fn key
    auto expected = h().ops();
    const Response r = req(req::Power{req::PowerAction::On});
    EXPECT_TRUE(r.ok) << r.error.value_or("");
    expected.push_back(MockOp::backlight_op(true));
    EXPECT_EQ(h().ops(), expected) << "one backlight write, no zone write";
}

// With a static profile nothing polls the backlight, so `status` and `toggle` read it on demand:
// after the Fn key turns the light off, status says off and one toggle turns it back on.
TEST_F(DaemonTest, StatusReadsTheBacklightAfterFnKeyOff) {
    start(kStaticCfg);
    h().set_hw_backlight(false); // the Fn key
    EXPECT_FALSE(status().on);
    h().set_hw_backlight(true); // and on again
    EXPECT_TRUE(status().on);
}

TEST_F(DaemonTest, ToggleAfterFnKeyOffTurnsTheLightOnInOnePress) {
    start(kStaticCfg);
    h().set_hw_backlight(false); // the Fn key
    auto expected = h().ops();
    const Response r = req(req::Power{req::PowerAction::Toggle});
    EXPECT_TRUE(r.ok) << r.error.value_or("");
    expected.push_back(MockOp::backlight_op(true));
    EXPECT_EQ(h().ops(), expected) << "one backlight write, no zone write";
    EXPECT_TRUE(status().on);
}

// P5 step 6 on the real keyboard: after these commands, Fn off, status, then one toggle left the
// light off and recorded "off".
TEST_F(DaemonTest, ToggleAfterFnKeyOffWorksAfterAnOffOnSequence) {
    start(kStaticCfg);
    ASSERT_TRUE(req(set({"#ff0000", "#00ff00", "#0000ff", "#ffffff"})).ok);
    ASSERT_TRUE(req(req::Brightness{"30"}).ok);
    ASSERT_TRUE(req(req::Brightness{"100"}).ok);
    ASSERT_TRUE(req(req::Power{req::PowerAction::Off}).ok);
    ASSERT_TRUE(req(req::Power{req::PowerAction::On}).ok);
    ASSERT_TRUE(req(req::Profile{"plain"}).ok);
    h().set_hw_backlight(false); // the Fn key
    EXPECT_FALSE(status().on);
    auto expected = h().ops();
    const Response r = req(req::Power{req::PowerAction::Toggle});
    EXPECT_TRUE(r.ok) << r.error.value_or("");
    expected.push_back(MockOp::backlight_op(true));
    EXPECT_EQ(h().ops(), expected) << "one backlight-on write";
    EXPECT_TRUE(status().on);
}

TEST_F(DaemonTest, ToggleAfterFnKeyOnTurnsTheLightOffInOnePress) {
    start(kStaticCfg);
    ASSERT_TRUE(req(req::Power{req::PowerAction::Off}).ok);
    h().set_hw_backlight(true); // the Fn key
    EXPECT_TRUE(status().on);
    auto expected = h().ops();
    const Response r = req(req::Power{req::PowerAction::Toggle});
    EXPECT_TRUE(r.ok) << r.error.value_or("");
    expected.push_back(MockOp::backlight_op(false));
    EXPECT_EQ(h().ops(), expected);
    EXPECT_FALSE(status().on);
}

// Gates that switch the light off are the daemon's own doing, not the Fn key: status keeps the
// user's on/off.
TEST_F(DaemonTest, StatusKeepsOnWhileAGateHoldsTheLightOff) {
    start(kStaticCfg);
    ASSERT_TRUE(req(req::Flag{"locked", true}).ok); // default on_lock = off
    EXPECT_FALSE(h().ops().back().on) << "the lock gate wrote the backlight off";
    const Status s = status();
    EXPECT_TRUE(s.on);
    EXPECT_FALSE(s.backlight);
}

// The writer reads before it takes a waiting job, so a read can show the light as it was before
// the daemon's own `off` landed. That read must not count as the Fn key turning it back on.
TEST_F(DaemonTest, AReadTakenBeforeOurOwnOffIsNotTheFnKey) {
    start(kStaticCfg);
    h().set_delay(300ms);
    auto slow = std::async(std::launch::async, [this] { return req(set({"#ff0000"})); });
    std::this_thread::sleep_for(50ms); // the writer is inside the slow zone write
    auto asked = std::async(std::launch::async, [this] {
        return raw_exchange(encode(Request{req::Status{}}) + "\n", 1);
    });
    std::this_thread::sleep_for(50ms); // the status read waits behind the zone write
    const Response off = req(req::Power{req::PowerAction::Off}); // queued behind that read
    EXPECT_TRUE(off.ok) << off.error.value_or("");
    EXPECT_TRUE(slow.get().ok);
    const auto replies = asked.get();
    ASSERT_EQ(replies.size(), 1u);
    ASSERT_TRUE(replies[0].status.has_value());
    EXPECT_FALSE(replies[0].status->on) << "the stale read said on; the light is off";
    EXPECT_FALSE(status().on);
}

TEST_F(DaemonTest, ToggleTwiceIsAPlainOffThenOn) {
    start(kStaticCfg);
    auto expected = h().ops();
    ASSERT_TRUE(req(req::Power{req::PowerAction::Toggle}).ok);
    EXPECT_FALSE(status().on);
    ASSERT_TRUE(req(req::Power{req::PowerAction::Toggle}).ok);
    EXPECT_TRUE(status().on);
    expected.push_back(MockOp::backlight_op(false));
    expected.push_back(MockOp::backlight_op(true));
    EXPECT_EQ(h().ops(), expected);
}

TEST_F(DaemonTest, EffectTicksAtConfiguredFps) {
    start(kWaveCfg);
    std::this_thread::sleep_for(300ms);
    const auto a = zone_writes().size();
    std::this_thread::sleep_for(1s);
    const auto n = zone_writes().size() - a;
    EXPECT_GE(n, 8u) << n << " writes in 1 s at 10 fps";
    EXPECT_LE(n, 12u) << n << " writes in 1 s at 10 fps";
}

TEST_F(DaemonTest, FnKeyOffPausesEffect) {
    start(kWaveCfg);
    h().set_hw_backlight(false);
    std::this_thread::sleep_for(4s);
    const auto a = zone_writes().size();
    std::this_thread::sleep_for(1s);
    EXPECT_EQ(zone_writes().size(), a) << "paused while the backlight is off";
    h().set_hw_backlight(true);
    EXPECT_TRUE(wait_until([&] { return zone_writes().size() > a + 3; }, 5000ms))
        << "resumes after the Fn key turns it back on";
}

// Spec: "idle/lock/Fn-off/suspend → stop ticking"; on_idle = dim (the default) and on_lock = dim
// keep the backlight on, so only the effect gate can stop the writes.
TEST_F(DaemonTest, IdleAndLockStopTheEffectUntilTheyEnd) {
    start(std::string(kWaveCfg) + "[gates]\non_lock = \"dim\"\n");
    const auto writes_in_500ms = [this] {
        const auto a = zone_writes().size();
        std::this_thread::sleep_for(500ms);
        return zone_writes().size() - a;
    };
    ASSERT_TRUE(req(req::Idle{true}).ok);
    EXPECT_EQ(writes_in_500ms(), 0u) << "idle stops ticking";
    ASSERT_TRUE(req(req::Idle{false}).ok);
    EXPECT_GE(writes_in_500ms(), 3u) << "ticking resumes after idle";
    ASSERT_TRUE(req(req::Flag{"locked", true}).ok);
    EXPECT_EQ(writes_in_500ms(), 0u) << "lock stops ticking";
    ASSERT_TRUE(req(req::Flag{"locked", false}).ok);
    EXPECT_GE(writes_in_500ms(), 3u) << "ticking resumes after unlock";
}

TEST_F(DaemonTest, BudgetErrorDropsToStatic) {
    start(kWaveCfg);
    h().fail_with(BackendError{BackendError::Kind::Budget, ""});
    std::this_thread::sleep_for(500ms);
    const Status s = status();
    EXPECT_TRUE(s.budget_exhausted);
    EXPECT_FALSE(s.animated);
    h().fail_with(std::nullopt);
    const auto a = zone_writes().size();
    std::this_thread::sleep_for(500ms);
    EXPECT_EQ(zone_writes().size(), a) << "no more ticking";
}

TEST_F(DaemonTest, ResumeForcesIdenticalRewrite) {
    start(kStaticCfg);
    const auto before = zone_writes();
    daemon_->post(event::Sleep{true});
    daemon_->post(event::Sleep{false});
    EXPECT_TRUE(wait_until([&] { return zone_writes().size() == before.size() + 1; }, 2000ms));
    std::this_thread::sleep_for(200ms);
    const auto after = zone_writes();
    ASSERT_EQ(after.size(), before.size() + 1) << "exactly one extra write";
    EXPECT_EQ(after.back(), before.back()) << "identical frame";
}

// hypridle locks before sleep and on_lock = "off" is the default, so the frame at resume turns the
// backlight off: the colors must still be re-asserted once the lock ends.
TEST_F(DaemonTest, ResumeWhileLockedRewritesTheZonesAtUnlock) {
    start(kStaticCfg);
    ASSERT_TRUE(req(req::Flag{"locked", true}).ok);
    const auto before = zone_writes().size();
    const auto ops = h().ops().size();
    daemon_->post(event::Sleep{true});
    daemon_->post(event::Sleep{false});
    EXPECT_TRUE(wait_until([&] { return h().ops().size() > ops; }, 2000ms))
        << "resume re-asserts the backlight state";
    std::this_thread::sleep_for(200ms);
    EXPECT_EQ(h().ops().back(), MockOp::backlight_op(false));
    EXPECT_EQ(zone_writes().size(), before) << "no zone write while locked";
    ASSERT_TRUE(req(req::Flag{"locked", false}).ok);
    EXPECT_EQ(zone_writes().size(), before + 1) << "colors re-asserted at unlock";
}

// hypridle's on_lock_cmd can also land after PrepareForSleep(true): it locks because of the sleep.
TEST_F(DaemonTest, LockDuringSuspendStillRewritesTheZonesAtUnlock) {
    start(kStaticCfg);
    const auto before = zone_writes().size();
    daemon_->post(event::Sleep{true});
    std::this_thread::sleep_for(100ms);
    ASSERT_TRUE(req(req::Flag{"locked", true}).ok);
    const auto ops = h().ops().size();
    daemon_->post(event::Sleep{false});
    EXPECT_TRUE(wait_until([&] { return h().ops().size() > ops; }, 2000ms));
    std::this_thread::sleep_for(200ms);
    EXPECT_EQ(h().ops().back(), MockOp::backlight_op(false));
    EXPECT_EQ(zone_writes().size(), before) << "no zone write while locked";
    ASSERT_TRUE(req(req::Flag{"locked", false}).ok);
    EXPECT_EQ(zone_writes().size(), before + 1) << "colors re-asserted at unlock";
}

// The real module spaces writes >= 50 ms apart, so the forced resume frame can wait in the
// writer's one-slot mailbox and be replaced by a later frame; the rewrite must survive that.
TEST_F(DaemonTest, ResumeRewriteSurvivesBeingReplacedInTheWriterSlot) {
    start(kStaticCfg);
    const auto before = zone_writes().size();
    h().set_delay(300ms);
    daemon_->post(FactUpdate{fact::LidClosed{true}}); // the writer turns the backlight off: busy
    std::this_thread::sleep_for(50ms);
    daemon_->post(FactUpdate{fact::LidClosed{false}}); // {on} waits in the slot
    daemon_->post(event::Sleep{true});
    daemon_->post(event::Sleep{false});            // the forced {on} replaces it
    daemon_->post(FactUpdate{fact::Locked{true}}); // {off} replaces the forced frame
    std::this_thread::sleep_for(800ms);            // both backlight writes are done
    h().set_delay(0ms);
    ASSERT_TRUE(req(req::Flag{"locked", false}).ok);
    EXPECT_EQ(zone_writes().size(), before + 1) << "colors re-asserted after resume";
}

TEST_F(DaemonTest, BadReloadKeepsPreviousConfig) {
    start(kStaticCfg);
    write_file(config_path(), "[[profiles]\nthis is not toml");
    const Response r = req(req::Reload{});
    EXPECT_FALSE(r.ok);
    EXPECT_NE(r.error.value_or("").find("config.toml"), std::string::npos) << r.error.value_or("");
    EXPECT_EQ(status().profile, "plain");
}

// Not in the Rust suite: sources (hwmon, theme watcher) are rebuilt from the reloaded config.
TEST_F(DaemonTest, ReloadNotifiesConfigListenersOnlyOnSuccess) {
    std::atomic<int> calls{0};
    std::atomic<bool> needs_temp{false};
    before_run_ = [&](Daemon &d) {
        d.on_config_replaced([&](const Config &c) {
            needs_temp = c.needs_cpu_temp();
            ++calls;
        });
    };
    start(kStaticCfg);
    write_file(config_path(), "[[profiles]\nthis is not toml");
    EXPECT_FALSE(req(req::Reload{}).ok);
    EXPECT_EQ(calls.load(), 0) << "a failed reload changes nothing";
    write_file(config_path(), std::string(kStaticCfg) +
                                  "\n[[rules]]\nname = 'hot'\nwhen = { cpu_temp_above = 90 }\n"
                                  "brightness_cap = 10\n");
    EXPECT_TRUE(req(req::Reload{}).ok);
    EXPECT_EQ(calls.load(), 1);
    EXPECT_TRUE(needs_temp.load());
}

TEST_F(DaemonTest, HintClearsAfterTimeout) {
    start(kStaticCfg);
    const Response r = req(req::Hint{"super"});
    EXPECT_TRUE(r.ok) << r.error.value_or("");
    EXPECT_EQ(status().hint, "super");
    EXPECT_EQ(zone_writes().back(), all(rgb("0000ff")));
    std::this_thread::sleep_for(1300ms);
    EXPECT_EQ(status().hint, std::nullopt);
    EXPECT_EQ(zone_writes().back()[2], rgb("101010"));
}

TEST_F(DaemonTest, PersistDebouncedOneSecond) {
    start(kStaticCfg);
    for (const char *v : {"90", "80", "70"}) {
        const Response r = req(req::Brightness{v});
        EXPECT_TRUE(r.ok) << r.error.value_or("");
    }
    std::this_thread::sleep_for(500ms);
    EXPECT_FALSE(std::filesystem::exists(state_path())) << "not saved before the debounce";
    std::this_thread::sleep_for(1000ms);
    const auto [s, w] = PersistedState::load(state_path(), "plain");
    EXPECT_EQ(w, std::nullopt);
    EXPECT_EQ(s.brightness, 70);
}

TEST_F(DaemonTest, FactsChangeTheFrame) {
    start(std::string(kStaticCfg) + "\n[[rules]]\nname = 'ps'\nwhen = { power_profile = "
                                    "'power-saver' }\nbrightness_cap = 50\n");
    daemon_->post(FactUpdate{fact::PowerProfile{"power-saver"}});
    const std::array<Rgb, 4> want{rgb("181818"), rgb("101010"), rgb("080808"), rgb("202020")};
    EXPECT_TRUE(wait_until([&] { return zone_writes().back() == want; }, 2000ms))
        << zone_writes().back()[0];
}

TEST_F(DaemonTest, SubscribeStreamsStatusChanges) {
    start(kStaticCfg);
    auto sub = subscribe(sock());
    ASSERT_TRUE(sub.has_value()) << sub.error().message();
    const auto first = sub->next_status();
    ASSERT_TRUE(first.has_value() && first->has_value());
    EXPECT_EQ((*first)->brightness, 100);
    EXPECT_TRUE(req(req::Brightness{"40"}).ok);
    auto next = std::async(std::launch::async, [&] { return sub->next_status(); });
    if (next.wait_for(2s) != std::future_status::ready) {
        stop_daemon(); // closes the stream so the reader returns
        FAIL() << "no status update within 2 s";
    }
    const auto s = next.get();
    ASSERT_TRUE(s.has_value() && s->has_value());
    EXPECT_EQ((*s)->brightness, 40);
}

// Not in the Rust suite: the hand-written C++ connection handling.
TEST_F(DaemonTest, GarbageLineGetsAnErrorAndTheConnectionStaysUsable) {
    start(kStaticCfg);
    const auto got = raw_exchange("not json\n" + encode(Request{req::Status{}}) + "\n", 2);
    ASSERT_EQ(got.size(), 2u);
    EXPECT_FALSE(got[0].ok);
    EXPECT_TRUE(got[1].ok);
    EXPECT_EQ(got[1].status.value().profile, "plain");
}

// nlohmann's parse error quotes the offending byte; replying with it must not end the daemon.
TEST_F(DaemonTest, InvalidUtf8LineGetsAnErrorAndTheDaemonSurvives) {
    start(kStaticCfg);
    const auto got = raw_exchange("\xff\n" + encode(Request{req::Status{}}) + "\n", 2);
    ASSERT_EQ(got.size(), 2u);
    EXPECT_FALSE(got[0].ok);
    EXPECT_TRUE(got[1].ok);
    EXPECT_EQ(status().profile, "plain") << "new connections are still served";
}

// ── reactions ──────────────────────────────────────────────────────────────
// Each test starts on kRigConfig's static profile. The startup frame writes the backlight, so the
// daemon holds no backlight reading until something reads it.

TEST_F(DaemonTest, WindowOpenedSignalPlaysAPulseAndRestores) {
    start(kRigConfig);
    const auto before = zone_writes().size();
    daemon_->post(Event{signal::WindowOpened{}});
    EXPECT_TRUE(wait_until([&] { return zone_writes().size() >= before + 2; }, 2000ms));
    std::this_thread::sleep_for(300ms); // nothing follows
    EXPECT_EQ(zone_writes_since(before), hw_frames({lifted(kBase), kBase}))
        << "50% toward white, then back";
}

TEST_F(DaemonTest, NotifyRequestIsAnsweredAtOnceAndFlashes) {
    start(kRigConfig);
    const auto before = zone_writes().size();
    h().set_delay(800ms); // every write is slow: a reply that waited for one would come late
    const auto sent = Clock::now();
    const Response r = req(req::Notify{"ZapZap", "com.rtosta.zapzap", "normal"});
    const auto took = Clock::now() - sent;
    EXPECT_TRUE(r.ok) << r.error.value_or("");
    EXPECT_TRUE(r.status.has_value());
    EXPECT_LT(took, 500ms) << "the reply does not wait for the read or the write";
    EXPECT_TRUE(wait_until([&] { return zone_writes().size() >= before + 2; }, 3000ms));
    h().set_delay(0ms);
    std::this_thread::sleep_for(300ms);
    EXPECT_EQ(zone_writes_since(before), hw_frames({all(kZapZap), kBase}));
}

TEST_F(DaemonTest, AgentWaitingHoldsTheLampWithoutFurtherWrites) {
    start(kRigConfig);
    const auto before = zone_writes().size();
    const Frame lit = paint(kBase, {Zone::Wasd}, kWaiting);
    const Response waiting = req(req::Agent{"waiting", "s1", std::nullopt, "test"});
    EXPECT_TRUE(waiting.ok) << waiting.error.value_or("");
    EXPECT_TRUE(wait_until([&] { return zone_writes().size() >= before + 3; }, 2000ms));
    std::this_thread::sleep_for(1s);
    EXPECT_EQ(zone_writes_since(before), hw_frames({lit, kBase, lit}))
        << "on, off, on, then held with no further write";

    const Response working = req(req::Agent{"working", "s1", std::nullopt, "test"});
    EXPECT_TRUE(working.ok) << working.error.value_or("");
    EXPECT_TRUE(wait_until([&] { return zone_writes().size() >= before + 4; }, 2000ms));
    std::this_thread::sleep_for(300ms);
    EXPECT_EQ(zone_writes_since(before), hw_frames({lit, kBase, lit, kBase}));
}

TEST_F(DaemonTest, BadNotifyOrAgentRequestIsAFailure) {
    start(kRigConfig);
    const Response n = req(req::Notify{"ZapZap", "com.rtosta.zapzap", "loud"});
    EXPECT_FALSE(n.ok);
    EXPECT_NE(n.error.value_or("").find("unknown urgency"), std::string::npos)
        << n.error.value_or("");
    const Response a = req(req::Agent{"sleeping", "s1", std::nullopt, "test"});
    EXPECT_FALSE(a.ok);
    EXPECT_NE(a.error.value_or("").find("unknown agent state"), std::string::npos)
        << a.error.value_or("");
}

TEST_F(DaemonTest, StaleBacklightReadingCausesOneReadBeforeAReaction) {
    start(kRigConfig);
    const auto before = zone_writes().size();
    const auto reads = h().backlight_reads();
    // Signals that only update or clear what shows need no reading.
    daemon_->post(Event{signal::FocusChanged{"a1", "kitty"}});
    daemon_->post(Event{signal::UrgentCleared{"a1"}});
    daemon_->post(Event{signal::AgentWorking{"s1"}});
    std::this_thread::sleep_for(300ms);
    EXPECT_EQ(h().backlight_reads(), reads);
    EXPECT_EQ(zone_writes().size(), before);

    daemon_->post(Event{signal::WindowOpened{}});
    EXPECT_TRUE(wait_until([&] { return zone_writes().size() >= before + 2; }, 2000ms));
    std::this_thread::sleep_for(300ms);
    EXPECT_EQ(h().backlight_reads(), reads + 1);
    EXPECT_EQ(zone_writes_since(before), hw_frames({lifted(kBase), kBase}));
}

TEST_F(DaemonTest, TwoReactionsWithinFiveSecondsCauseOneRead) {
    start(kRigConfig);
    const auto before = zone_writes().size();
    const auto reads = h().backlight_reads();
    daemon_->post(Event{signal::WindowOpened{}});
    EXPECT_TRUE(wait_until([&] { return zone_writes().size() >= before + 2; }, 2000ms));
    std::this_thread::sleep_for(600ms); // past the 500 ms window cooldown
    daemon_->post(Event{signal::WindowClosed{}});
    EXPECT_TRUE(wait_until([&] { return zone_writes().size() >= before + 4; }, 2000ms));
    std::this_thread::sleep_for(300ms);
    EXPECT_EQ(h().backlight_reads(), reads + 1) << "the second reaction uses the first reading";
    EXPECT_EQ(zone_writes_since(before), hw_frames({lifted(kBase), kBase, dipped(kBase), kBase}));
}

TEST_F(DaemonTest, ReactionOnADarkKeyboardWritesNothing) {
    start(kRigConfig);
    const auto before = zone_writes().size();
    const auto ops = h().ops();
    const auto reads = h().backlight_reads();
    h().set_hw_backlight(false); // the Fn key: nothing reports it
    daemon_->post(Event{signal::WindowOpened{}});
    EXPECT_TRUE(wait_until([&] { return h().backlight_reads() > reads; }, 2000ms));
    daemon_->post(Event{signal::UrgentRaised{"a1"}});
    daemon_->post(Event{signal::AgentWaiting{"s1", std::nullopt}});
    std::this_thread::sleep_for(500ms);
    EXPECT_EQ(h().ops(), ops) << "no zone or backlight write";
    EXPECT_EQ(h().backlight_reads(), reads + 1) << "one read covers the three signals";

    // The Fn key turns the light back on, and the next read shows both lamps steady: one write.
    h().set_hw_backlight(true);
    EXPECT_TRUE(status().on);
    EXPECT_TRUE(wait_until([&] { return zone_writes().size() >= before + 1; }, 2000ms));
    std::this_thread::sleep_for(500ms);
    EXPECT_EQ(zone_writes_since(before),
              hw_frames({paint(paint(kBase, {Zone::Right}, kUrgent), {Zone::Wasd}, kWaiting)}))
        << "both lamps, steady, no blink";
}

TEST_F(DaemonTest, DisabledReactionsIgnoreSignalsWithoutReading) {
    start(kStaticCfg); // no [reactions] table: off
    const auto ops = h().ops();
    const auto reads = h().backlight_reads();
    daemon_->post(Event{signal::WindowOpened{}});
    daemon_->post(Event{signal::UrgentRaised{"a1"}});
    const Response n = req(req::Notify{"ZapZap", "com.rtosta.zapzap", "critical"});
    EXPECT_TRUE(n.ok) << n.error.value_or("");
    const Response a = req(req::Agent{"waiting", "s1", std::nullopt, "test"});
    EXPECT_TRUE(a.ok) << a.error.value_or("");
    std::this_thread::sleep_for(300ms);
    EXPECT_EQ(h().backlight_reads(), reads) << "off means off: not even a read";
    EXPECT_EQ(h().ops(), ops);
}

TEST_F(DaemonTest, OrderIsKeptWhileAReadIsPending) {
    start(kRigConfig);
    const auto before = zone_writes().size();
    stall_writer(300ms);
    // The waiting session asks for a read, which waits behind the slow write; the session goes
    // back to work before the read lands. Applied in order, the lamp never lights.
    daemon_->post(Event{signal::AgentWaiting{"s1", std::nullopt}});
    daemon_->post(Event{signal::AgentWorking{"s1"}});
    EXPECT_TRUE(wait_until([&] { return zone_writes().size() >= before + 1; }, 2000ms));
    std::this_thread::sleep_for(500ms);
    EXPECT_EQ(zone_writes_since(before), hw_frames({scaled(kBase, 15)}))
        << "only the fullscreen dim: the lamp never lit";
    EXPECT_TRUE(status().reactions.lamps.empty());
}

TEST_F(DaemonTest, FloodWhileAReadIsPendingStaysBounded) {
    start(kRigConfig);
    const auto before = zone_writes().size();
    const auto reads = h().backlight_reads();
    stall_writer(500ms);
    for (int i = 0; i < 5000; ++i) {
        daemon_->post(Event{signal::WindowOpened{}});
    }
    const auto asked = Clock::now();
    const Response r = req(req::Status{});
    EXPECT_TRUE(r.ok) << r.error.value_or("");
    EXPECT_LT(Clock::now() - asked, 2s) << "the daemon keeps answering";
    std::this_thread::sleep_for(1s); // every clip has ended
    const auto writes = zone_writes_since(before);
    EXPECT_LE(writes.size(), 8u);
    ASSERT_FALSE(writes.empty());
    EXPECT_EQ(writes.back(), hw(scaled(kBase, 15))) << "the keyboard ends on the dimmed base";
    EXPECT_LE(h().backlight_reads() - reads, 10u) << "reads do not follow the flood";
}

// Ruling P16: while an effect ticks, a signal adds no frame of its own; the ticks sample it.
TEST_F(DaemonTest, SignalsOnARunningEffectAddNoWrites) {
    start(kRigConfig);
    ASSERT_TRUE(req(req::Profile{"wavy"}).ok);
    EXPECT_TRUE(status().animated); // also a fresh reading: no signal below waits for a read
    const auto a = zone_writes().size();
    const auto end = Clock::now() + 1s;
    for (int i = 0; Clock::now() < end; ++i) { // about 40 signals, 25 ms apart
        if (i % 2 == 0) {
            daemon_->post(Event{signal::WindowOpened{}});
        } else {
            daemon_->post(Event{signal::FocusChanged{i % 4 == 1 ? "a1" : "b2", "kitty"}});
        }
        std::this_thread::sleep_for(25ms);
    }
    const auto n = zone_writes().size() - a;
    // The effect's own 10 ticks a second (as in EffectTicksAtConfiguredFps), plus the frame of a
    // periodic backlight probe; a frame per signal would add about 40.
    EXPECT_LE(n, 13u) << n << " writes in 1 s at 10 fps with about 40 signals";
    EXPECT_GE(n, 5u) << n << " writes in 1 s at 10 fps";
}

// Ruling P17: a lamp is held while the Fn key has the backlight off; `toggle` brings the light
// back, and the lamp shows in one more write with no other event, and the loop then idles.
TEST_F(DaemonTest, FnKeyThenToggleShowsTheHeldLampInOneWrite) {
    start(kRigConfig);
    const auto before = zone_writes().size();
    const Frame lit = paint(kBase, {Zone::Right}, kUrgent);
    daemon_->post(Event{signal::UrgentRaised{"a1"}});
    EXPECT_TRUE(wait_until([&] { return zone_writes().size() >= before + 3; }, 2000ms));
    std::this_thread::sleep_for(300ms);
    ASSERT_EQ(zone_writes_since(before), hw_frames({lit, kBase, lit}));

    // The read before `status` sees the Fn key's off, and the lamp is no longer drawn: the write
    // the rig counts too.
    h().set_hw_backlight(false);
    EXPECT_FALSE(status().on);
    EXPECT_TRUE(wait_until([&] { return zone_writes().size() >= before + 4; }, 2000ms));
    EXPECT_EQ(zone_writes().back(), hw(kBase));
    auto expected = h().ops();

    const Response r = req(req::Power{req::PowerAction::Toggle});
    EXPECT_TRUE(r.ok) << r.error.value_or("");
    EXPECT_TRUE(wait_until([&] { return h().ops().size() >= expected.size() + 2; }, 2000ms));
    std::this_thread::sleep_for(300ms);
    expected.push_back(MockOp::backlight_op(true));
    expected.push_back(MockOp::zones_op(hw(lit)));
    EXPECT_EQ(h().ops(), expected) << "the backlight on, then the lamp, steady";

    // The deadline that woke the loop moved on: nothing more is written, and the loop sleeps.
    const auto cpu = loop_cpu_time();
    const auto ops = h().ops().size();
    std::this_thread::sleep_for(1s);
    EXPECT_EQ(h().ops().size(), ops);
    const auto busy = std::chrono::duration_cast<std::chrono::milliseconds>(loop_cpu_time() - cpu);
    EXPECT_LT(busy, 250ms) << "the loop thread used " << busy.count() << " ms of CPU in 1 s";
}

// Task 13 minor 1: the read a new reaction waits for finds the Fn key turned the light back on
// while a lamp was held in the dark. The reading and the waiting signal show in one frame: the
// pulse over the lamp, then the lamp. Recomputing for the reading first would cost one more write
// (the lamp alone, then the pulse, then the lamp).
TEST_F(DaemonTest, AReadThatFindsTheLightBackOnShowsTheWaitingReactionInItsFirstFrame) {
    start(kRigConfig);
    const auto before = zone_writes().size();
    const auto ops = h().ops();
    const auto reads = h().backlight_reads();
    const Frame lit = paint(kBase, {Zone::Right}, kUrgent);
    h().set_hw_backlight(false); // the Fn key
    daemon_->post(Event{signal::UrgentRaised{"a1"}});
    EXPECT_TRUE(wait_until([&] { return h().backlight_reads() > reads; }, 2000ms));
    std::this_thread::sleep_for(300ms);
    ASSERT_EQ(h().ops(), ops) << "the lamp is held in the dark";

    std::this_thread::sleep_for(5200ms); // the off reading is no longer fresh
    h().set_hw_backlight(true);          // the Fn key again: nothing reports it
    daemon_->post(Event{signal::WindowOpened{}});
    EXPECT_TRUE(wait_until([&] { return zone_writes().size() >= before + 2; }, 2000ms));
    std::this_thread::sleep_for(500ms); // the pulse has ended
    EXPECT_EQ(h().backlight_reads(), reads + 2);
    EXPECT_EQ(zone_writes_since(before), hw_frames({lifted(lit), lit}))
        << "the pulse over the lamp, then the lamp: two writes";
}

// Task 13 minor 3 (R4): a reload that turns `[reactions]` off drops the signals waiting for a read,
// so a read that predates the daemon's own backlight write is not asked for again.
TEST_F(DaemonTest, AReloadThatTurnsReactionsOffDropsTheSignalsWaitingForARead) {
    start(kRigConfig);
    h().set_delay(500ms);
    daemon_->post(FactUpdate{fact::FullscreenApp{"mpv"}}); // a slow zone write
    std::this_thread::sleep_for(100ms);                    // the writer is inside it
    h().set_delay(0ms);
    // A backlight write waits in the writer's slot; the read asked for next goes before it, so it
    // predates that write (the stale read the daemon would ask again for).
    auto off =
        std::async(std::launch::async, [this] { return req(req::Power{req::PowerAction::Off}); });
    std::this_thread::sleep_for(50ms);
    const auto reads = h().backlight_reads();
    daemon_->post(Event{signal::WindowOpened{}}); // waits for a read
    write_file(config_path(), replaced_once(kRigConfig, "[reactions]\nenabled = true",
                                            "[reactions]\nenabled = false"));
    const Response r = req(req::Reload{});
    ASSERT_TRUE(r.ok) << r.error.value_or("");
    EXPECT_TRUE(off.get().ok);
    std::this_thread::sleep_for(500ms);
    EXPECT_EQ(h().backlight_reads(), reads + 1)
        << "only the read asked for before the reload: off means not even a read";
}

TEST(EventLoop, TimersRunInOrderPostWorksAcrossThreadsAndCancelSkips) {
    EventLoop loop;
    std::vector<int> seen;
    const auto now = Clock::now();
    loop.at(now + 30ms, [&] { seen.push_back(2); });
    const auto id = loop.at(now + 20ms, [&] { seen.push_back(99); });
    loop.at(now + 10ms, [&] { seen.push_back(1); });
    loop.cancel(id);
    loop.at(now + 50ms, [&] {
        std::thread([&] {
            loop.post([&] {
                seen.push_back(3);
                loop.stop();
            });
        }).join();
    });
    loop.run();
    EXPECT_EQ(seen, (std::vector<int>{1, 2, 3}));
}

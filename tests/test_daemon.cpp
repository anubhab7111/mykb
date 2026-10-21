// Integration tests: the real daemon loop against the mock backend, over the real socket, in real
// time. Each test runs Daemon::run() on its own thread and stops it in TearDown.

#include "helpers.hpp"

#include "backend/mock.hpp"
#include "daemon/daemon.hpp"
#include "ipc/socket.hpp"

#include <gtest/gtest.h>

#include <atomic>
#include <future>
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

    Response req(const Request &r) {
        auto resp = request(sock(), r);
        if (!resp) {
            ADD_FAILURE() << "daemon unreachable: " << resp.error().message();
            return Response::failure(resp.error().message());
        }
        return *resp;
    }

    Status status() { return req(req::Status{}).status.value(); }

    [[nodiscard]] std::filesystem::path config_path() const { return dir_.path() / "config.toml"; }
    [[nodiscard]] std::filesystem::path state_path() const { return dir_.path() / "state.json"; }
    [[nodiscard]] std::filesystem::path sock() const { return dir_.path() / "omen-kbd.sock"; }

    TempDir dir_;
    std::optional<MockHandle> h_;
    std::unique_ptr<Daemon> daemon_;
    std::thread thread_;
    std::atomic<int> rc_{-1};
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

TEST_F(DaemonTest, BadReloadKeepsPreviousConfig) {
    start(kStaticCfg);
    write_file(config_path(), "[[profiles]\nthis is not toml");
    const Response r = req(req::Reload{});
    EXPECT_FALSE(r.ok);
    EXPECT_NE(r.error.value_or("").find("config.toml"), std::string::npos) << r.error.value_or("");
    EXPECT_EQ(status().profile, "plain");
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
    UniqueFd fd(::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0));
    sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    std::strncpy(addr.sun_path, sock().c_str(), sizeof(addr.sun_path) - 1);
    ASSERT_EQ(::connect(fd.get(), reinterpret_cast<sockaddr *>(&addr), sizeof(addr)), 0);
    timeval tv{.tv_sec = 2, .tv_usec = 0};
    ::setsockopt(fd.get(), SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    const std::string out = "not json\n" + encode(Request{req::Status{}}) + "\n";
    ASSERT_EQ(::write(fd.get(), out.data(), out.size()), static_cast<ssize_t>(out.size()));
    LineBuffer lines;
    std::vector<Response> got;
    char buf[4096];
    while (got.size() < 2) {
        if (auto line = lines.next_line()) {
            got.push_back(decode_response(*line).value());
            continue;
        }
        const ssize_t n = ::read(fd.get(), buf, sizeof(buf));
        ASSERT_GT(n, 0) << "connection closed or timed out";
        lines.append({buf, static_cast<std::size_t>(n)});
    }
    EXPECT_FALSE(got[0].ok);
    EXPECT_TRUE(got[1].ok);
    EXPECT_EQ(got[1].status.value().profile, "plain");
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

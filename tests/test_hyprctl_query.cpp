// The one-shot `j/activewindow` query to Hyprland's request socket: the pure reply parser, and the
// async query on a real loop against a fake request socket.

#include "fake_hyprland.hpp"
#include "helpers.hpp"

#include "sources/hyprctl_query.hpp"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <cstddef>
#include <filesystem>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <utility>
#include <vector>

using namespace omenkbd;
using namespace omenkbd::test;
using namespace std::chrono_literals;
namespace fs = std::filesystem;

namespace {

using Info = std::optional<ActiveWindowInfo>;

struct Delivery {
    TimePoint at;
    Info info;
};

constexpr std::string_view kMpv = R"({"address":"0x2","class":"mpv","fullscreen":2})";

/// A query on a running loop, talking to a fake request socket at `<tmp>/hypr/sig/.socket.sock`.
/// `request`, `cancel` and `destroy_query` run on the loop thread, as the daemon's code does.
struct Rig {
    TempDir dir;
    fs::path socket = dir.path() / "hypr/sig/.socket.sock";
    FakeRequestServer server{socket};
    EventLoop loop;
    mutable std::mutex mu;
    std::vector<Delivery> delivered;
    std::optional<HyprctlQuery> query;
    std::optional<LoopThread> running; // last: stops the loop before the rest goes

    Rig() {
        query.emplace(loop, [this](Info info) {
            const std::lock_guard lock(mu);
            delivered.push_back({Clock::now(), std::move(info)});
        });
        running.emplace(loop);
    }

    void request() { request_to(socket); }
    void request_to(const fs::path &path) {
        loop.post([this, path] { query->request(path); });
    }
    void cancel() {
        loop.post([this] { query->cancel(); });
    }
    /// Destroys the query on the loop thread, and waits until it is gone.
    void destroy_query() {
        auto done = std::make_shared<std::atomic<bool>>(false);
        loop.post([this, done] {
            query.reset();
            *done = true;
        });
        ASSERT_TRUE(wait_until([done] { return done->load(); }, 2000ms));
    }

    [[nodiscard]] std::vector<Delivery> deliveries() const {
        const std::lock_guard lock(mu);
        return delivered;
    }
    /// True once `n` results (in all) have been delivered.
    [[nodiscard]] bool wait_for(std::size_t n, std::chrono::milliseconds timeout = 3000ms) const {
        return wait_until([&] { return deliveries().size() >= n; }, timeout);
    }
};

std::size_t open_fds() {
    std::size_t n = 0;
    for ([[maybe_unused]] const auto &entry : fs::directory_iterator("/proc/self/fd")) {
        ++n;
    }
    return n;
}

} // namespace

TEST(HyprctlQuery, ParsesActiveWindowJson) {
    EXPECT_EQ(parse_active_window(R"({"address":"0xABC","class":"mpv","fullscreen":2})"),
              (ActiveWindowInfo{"abc", "mpv", 2}));
    EXPECT_EQ(parse_active_window(R"({"address":"55666F363B20","class":"Firefox","fullscreen":0})"),
              (ActiveWindowInfo{"55666f363b20", "Firefox", 0}))
        << "the address is normalized, the class is left as Hyprland says it";
    EXPECT_EQ(parse_active_window(R"({"address":"0x1","class":"","fullscreen":3})"),
              (ActiveWindowInfo{"1", "", 3}))
        << "an empty class is still a window";

    // Hyprland sends much more than we use; the title is never kept (the struct has no room).
    EXPECT_EQ(parse_active_window(R"({"address":"0xab","mapped":true,"hidden":false,
        "at":[0,0],"size":[1920,1080],"workspace":{"id":1,"name":"1"},"floating":false,
        "class":"mpv","title":"Secret, \"quoted\" title","pid":42,"fullscreen":2,
        "fullscreenClient":2,"tags":[],"xwayland":false})"),
              (ActiveWindowInfo{"ab", "mpv", 2}));

    EXPECT_EQ(parse_active_window("{}"), std::nullopt) << "nothing is focused";
    EXPECT_EQ(parse_active_window(""), std::nullopt);
}

TEST(HyprctlQuery, GarbageNeverThrowsAndNeverMakesAWindow) {
    const std::vector<std::string> garbage{
        "Invalid",
        "not json at all",
        "{",
        R"({"address":"0xab","class":"mpv","fullscreen":)", // cut off
        "[]",
        "null",
        "42",
        R"("a string")",
        R"({"class":"mpv","fullscreen":2})",                // no address
        R"({"address":"0xab","fullscreen":2})",             // no class
        R"({"address":"0xab","class":"mpv"})",              // no fullscreen
        R"({"address":"","class":"mpv","fullscreen":2})",   // empty address
        R"({"address":"0x","class":"mpv","fullscreen":2})", // a bare prefix
        R"({"address":7,"class":"mpv","fullscreen":2})",    // wrong types
        R"({"address":"0xab","class":["mpv"],"fullscreen":2})",
        R"({"address":"0xab","class":"mpv","fullscreen":"2"})",
        R"({"address":"0xab","class":"mpv","fullscreen":2.5})",
        R"({"address":"0xab","class":"mpv","fullscreen":null})",
        R"({"address":"0xab","class":"mpv","fullscreen":-1})", // a mode is never negative
        R"({"address":"0xab","class":"mpv","fullscreen":99999999999999})", // beyond an int
        R"({"address":"0xab","class":"mpv","fullscreen":18446744073709551615})",
        R"({"address":"0xab","class":"mpv","fullscreen":1e400})",
        std::string(1000, '['),
    };
    for (const std::string &text : garbage) {
        EXPECT_EQ(parse_active_window(text), std::nullopt) << text.substr(0, 80);
    }
}

TEST(HyprctlQuery, TrueFullscreenIsTheFullscreenBitOfHyprlandsMode) {
    static_assert(!is_true_fullscreen(0), "no fullscreen");
    static_assert(!is_true_fullscreen(1), "maximized is not fullscreen");
    static_assert(is_true_fullscreen(2), "fullscreen");
    static_assert(is_true_fullscreen(3), "maximized and fullscreen");
    EXPECT_TRUE(is_true_fullscreen(2));
    EXPECT_FALSE(is_true_fullscreen(1));
}

TEST(HyprctlQuery, AnswersFromAFakeRequestSocket) {
    Rig rig;
    rig.server.reply_with(
        R"({"address":"0xABC","class":"mpv","fullscreen":2,"title":"a private title"})");
    const TimePoint t0 = Clock::now();
    rig.request();
    ASSERT_TRUE(rig.wait_for(1));
    const std::vector<Delivery> got = rig.deliveries();
    ASSERT_EQ(got.size(), 1u);
    EXPECT_EQ(got[0].info, (ActiveWindowInfo{"abc", "mpv", 2}));
    EXPECT_GE(got[0].at - t0, 90ms) << "debounced by 100 ms";
    EXPECT_LT(got[0].at - t0, 1s);
    EXPECT_EQ(rig.server.requests(), std::vector<std::string>{"j/activewindow"});
    EXPECT_EQ(rig.server.connections(), 1);
}

TEST(HyprctlQuery, AccumulatesAReplyThatArrivesInPieces) {
    Rig rig;
    rig.server.reply_with(std::string(kMpv), 60ms);
    rig.request();
    ASSERT_TRUE(rig.wait_for(1));
    EXPECT_EQ(rig.deliveries().at(0).info, (ActiveWindowInfo{"2", "mpv", 2}))
        << "it reads to EOF, not to the first chunk";
}

// Nothing focused is a real answer (the bridge clears the fullscreen fact on it); a failure is
// nullopt (the bridge keeps the fact).
TEST(HyprctlQuery, NothingFocusedIsAnAnswerAndAnythingElseIsNot) {
    Rig rig;
    const std::vector<std::pair<std::string, Info>> cases{
        {"{}", ActiveWindowInfo{}},
        {"", ActiveWindowInfo{}},
        {" \n", ActiveWindowInfo{}},
        {"{ }\n", ActiveWindowInfo{}},
        {"Invalid", std::nullopt},
        {"[]", std::nullopt},
        {R"({"x":1})", std::nullopt}, // an object, but not a window
        {std::string(kMpv), ActiveWindowInfo{"2", "mpv", 2}},
    };
    std::size_t expected = 0;
    for (const auto &[reply, want] : cases) {
        rig.server.reply_with(reply);
        rig.request();
        ASSERT_TRUE(rig.wait_for(++expected)) << reply;
        EXPECT_EQ(rig.deliveries().back().info, want) << "reply: " << reply;
    }
}

TEST(HyprctlQuery, TimesOutAndReportsNullopt) {
    Rig rig;
    rig.server.stay_silent();
    const TimePoint t0 = Clock::now();
    rig.request();
    ASSERT_TRUE(rig.wait_for(1, 2000ms)) << "nullopt within about 400 ms";
    const std::vector<Delivery> got = rig.deliveries();
    EXPECT_EQ(got.at(0).info, std::nullopt);
    EXPECT_LT(got[0].at - t0, 1s);
    const std::optional<TimePoint> accepted = rig.server.first_accept();
    ASSERT_TRUE(accepted.has_value());
    EXPECT_GE(got[0].at - *accepted, 250ms) << "300 ms from the connect, not from the request";
    EXPECT_TRUE(wait_until([&] { return rig.server.closed_by_client() == 1; }, 1000ms))
        << "the timed-out connection is closed";
    std::this_thread::sleep_for(150ms);
    EXPECT_EQ(rig.deliveries().size(), 1u) << "one result per request";
}

TEST(HyprctlQuery, DebouncesBurstsIntoOneRequest) {
    Rig rig;
    rig.server.reply_with(std::string(kMpv));
    for (int i = 0; i < 5; ++i) { // five requests in about 50 ms
        rig.request();
        std::this_thread::sleep_for(10ms);
    }
    ASSERT_TRUE(rig.wait_for(1));
    std::this_thread::sleep_for(250ms);
    EXPECT_EQ(rig.server.connections(), 1);
    EXPECT_EQ(rig.deliveries().size(), 1u);
}

TEST(HyprctlQuery, EachRequestRestartsTheDebounce) {
    Rig rig;
    rig.server.reply_with(std::string(kMpv));
    TimePoint last = Clock::now();
    for (int i = 0; i < 4; ++i) { // 0, 40, 80, 120 ms: the first one alone would fire at 100 ms
        rig.request();
        last = Clock::now();
        std::this_thread::sleep_for(40ms);
    }
    ASSERT_TRUE(rig.wait_for(1));
    EXPECT_GE(rig.deliveries().at(0).at - last, 90ms) << "100 ms after the last request";
    std::this_thread::sleep_for(150ms);
    EXPECT_EQ(rig.server.connections(), 1) << "the earlier requests never connected";
    EXPECT_EQ(rig.deliveries().size(), 1u);
}

TEST(HyprctlQuery, CapsTheReplySize) {
    Rig rig;
    rig.server.reply_with(std::string(std::size_t{1} << 20, 'x')); // 1 MiB
    rig.request();
    ASSERT_TRUE(rig.wait_for(1));
    EXPECT_EQ(rig.deliveries().at(0).info, std::nullopt);
}

TEST(HyprctlQuery, TheCapIs64KiBExactly) {
    constexpr std::size_t kCap = std::size_t{64} * 1024;
    const std::string window(kMpv);
    Rig rig;
    rig.server.reply_with(window + std::string(kCap - window.size(), ' '));
    rig.request();
    ASSERT_TRUE(rig.wait_for(1));
    EXPECT_EQ(rig.deliveries().at(0).info, (ActiveWindowInfo{"2", "mpv", 2}))
        << "64 KiB is allowed";
    rig.server.reply_with(window + std::string(kCap - window.size() + 1, ' '));
    rig.request();
    ASSERT_TRUE(rig.wait_for(2));
    EXPECT_EQ(rig.deliveries().at(1).info, std::nullopt) << "one byte more is not";
}

TEST(HyprctlQuery, ANonexistentSocketIsAnError) {
    Rig rig;
    rig.request_to(rig.socket.parent_path() / "gone.sock");
    ASSERT_TRUE(rig.wait_for(1));
    EXPECT_EQ(rig.deliveries().at(0).info, std::nullopt);
    EXPECT_EQ(rig.server.connections(), 0);
}

TEST(HyprctlQuery, ALaterRequestReplacesOneInFlight) {
    Rig rig;
    rig.server.stay_silent();
    rig.request();
    ASSERT_TRUE(rig.server.wait_for_requests(1)) << "the first request is in flight";
    rig.server.reply_with(R"({"address":"0x2","class":"second","fullscreen":2})");
    rig.request();
    EXPECT_TRUE(wait_until([&] { return rig.server.closed_by_client() == 1; }, 1000ms))
        << "the stale connection is closed at once";
    ASSERT_TRUE(rig.wait_for(1));
    std::this_thread::sleep_for(450ms); // past the stale request's own timeout
    const std::vector<Delivery> got = rig.deliveries();
    ASSERT_EQ(got.size(), 1u) << "the stale request delivers nothing, not even its timeout";
    EXPECT_EQ(got[0].info, (ActiveWindowInfo{"2", "second", 2}));
    EXPECT_EQ(rig.server.connections(), 2);
}

TEST(HyprctlQuery, CancelDropsARequestStillInTheDebounce) {
    Rig rig;
    rig.request();
    rig.cancel();
    std::this_thread::sleep_for(450ms);
    EXPECT_EQ(rig.server.connections(), 0);
    EXPECT_EQ(rig.deliveries().size(), 0u);
}

TEST(HyprctlQuery, CancelClosesARequestInFlightAndItsReplyIsNeverDelivered) {
    Rig rig;
    rig.server.stay_silent();
    rig.request();
    ASSERT_TRUE(rig.server.wait_for_requests(1));
    rig.cancel();
    EXPECT_TRUE(wait_until([&] { return rig.server.closed_by_client() == 1; }, 1000ms));
    rig.server.answer_held(std::string(kMpv)); // too late, and nobody is listening
    std::this_thread::sleep_for(450ms);        // past what would have been its timeout
    EXPECT_EQ(rig.deliveries().size(), 0u);
}

TEST(HyprctlQuery, DestroyingItInTheDebounceNeverConnects) {
    Rig rig;
    rig.request();
    rig.destroy_query();
    std::this_thread::sleep_for(300ms);
    EXPECT_EQ(rig.server.connections(), 0);
    EXPECT_EQ(rig.deliveries().size(), 0u);
}

TEST(HyprctlQuery, DestroyingItMidRequestClosesTheConnectionAndCallsNothing) {
    Rig rig;
    rig.server.stay_silent();
    rig.request();
    ASSERT_TRUE(rig.server.wait_for_requests(1));
    rig.destroy_query();
    EXPECT_TRUE(wait_until([&] { return rig.server.closed_by_client() == 1; }, 1000ms));
    std::this_thread::sleep_for(450ms);
    EXPECT_EQ(rig.deliveries().size(), 0u) << "the destructor does not call the callback";
}

TEST(HyprctlQuery, ARequestMadeFromTheResultCallbackStartsANewQuery) {
    TempDir dir;
    const fs::path socket = dir.path() / "hypr/sig/.socket.sock";
    FakeRequestServer server(socket);
    server.reply_with(std::string(kMpv));
    EventLoop loop;
    std::atomic<int> results{0};
    std::optional<HyprctlQuery> query;
    query.emplace(loop, [&](Info) {
        if (++results < 2) {
            query->request(socket); // from inside the callback
        }
    });
    const LoopThread running(loop);
    loop.post([&] { query->request(socket); });
    ASSERT_TRUE(wait_until([&] { return results.load() >= 2; }, 3000ms));
    EXPECT_EQ(server.connections(), 2);
}

TEST(HyprctlQuery, DoesNotBlockTheLoopWhileWaiting) {
    Rig rig;
    rig.server.stay_silent();
    rig.request();
    ASSERT_TRUE(rig.server.wait_for_requests(1));
    auto fired = std::make_shared<std::atomic<bool>>(false);
    rig.loop.post([&rig, fired] { rig.loop.at(Clock::now() + 30ms, [fired] { *fired = true; }); });
    EXPECT_TRUE(wait_until([fired] { return fired->load(); }, 250ms))
        << "a 30 ms timer fires while the query waits for its reply";
    EXPECT_EQ(rig.deliveries().size(), 0u) << "and the query is still waiting";
}

TEST(HyprctlQuery, EveryPathClosesItsDescriptor) {
    Rig rig;
    const std::size_t baseline = open_fds();
    std::size_t delivered = 0;
    const auto run_to_result = [&] {
        rig.request();
        ASSERT_TRUE(rig.wait_for(++delivered));
    };

    rig.server.reply_with(std::string(kMpv)); // success
    run_to_result();
    rig.server.reply_with("garbage"); // an unusable reply
    run_to_result();
    rig.server.reply_with(std::string(std::size_t{1} << 20, 'x')); // over the cap
    run_to_result();
    rig.request_to(rig.socket.parent_path() / "gone.sock"); // nothing to connect to
    ASSERT_TRUE(rig.wait_for(++delivered));
    rig.server.stay_silent(); // timeout
    run_to_result();
    rig.request(); // cancelled in flight
    ASSERT_TRUE(rig.server.wait_for_requests(5));
    rig.cancel();
    rig.request(); // replaced in flight
    ASSERT_TRUE(rig.server.wait_for_requests(6));
    rig.request();
    ASSERT_TRUE(rig.server.wait_for_requests(7));
    rig.destroy_query(); // destroyed in flight

    EXPECT_TRUE(wait_until([&] { return open_fds() == baseline; }, 2000ms))
        << "descriptors now " << open_fds() << ", before " << baseline;
}

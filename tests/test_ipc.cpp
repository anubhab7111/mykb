#include "helpers.hpp"

#include "ipc/protocol.hpp"
#include "ipc/socket.hpp"

#include <gtest/gtest.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <thread>
#include <unistd.h>

using namespace omenkbd;
using namespace omenkbd::test;

namespace {

/// Accepts one client on a (non-blocking) listening socket, waiting up to 2 s.
UniqueFd accept_one(int listen_fd) {
    pollfd p{listen_fd, POLLIN, 0};
    if (::poll(&p, 1, 2000) != 1) {
        return UniqueFd();
    }
    return UniqueFd(::accept4(listen_fd, nullptr, nullptr, SOCK_CLOEXEC));
}

std::string read_line(int fd) {
    std::string line;
    char c = 0;
    while (::read(fd, &c, 1) == 1 && c != '\n') {
        line += c;
    }
    return line;
}

void write_all(int fd, std::string_view s) {
    while (!s.empty()) {
        const ssize_t n = ::write(fd, s.data(), s.size());
        ASSERT_GT(n, 0);
        s.remove_prefix(static_cast<std::size_t>(n));
    }
}

} // namespace

TEST(Ipc, RequestWireFormat) {
    EXPECT_EQ(encode(Request{req::Set{{"#ff0000"}}}), R"({"cmd":"set","colors":["#ff0000"]})");
    EXPECT_EQ(encode(Request{req::Power{req::PowerAction::Toggle}}),
              R"({"cmd":"power","action":"toggle"})");
    EXPECT_EQ(encode(Request{req::Status{}}), R"({"cmd":"status"})");
}

TEST(Ipc, EveryRequestRoundtrips) {
    const std::vector<Request> all{
        req::Status{},
        req::Set{{"#ff0000", "theme:blue", "#00ff00", "#0000ff"}},
        req::Profile{"next"},
        req::Brightness{"-10"},
        req::Power{req::PowerAction::On},
        req::Power{req::PowerAction::Off},
        req::Effect{"wave", std::uint8_t{80}},
        req::Effect{"static", std::nullopt},
        req::Hint{"super+shift"},
        req::Idle{true},
        req::Flag{"gamemode", false},
        req::Reload{},
        req::Subscribe{},
    };
    for (const Request &r : all) {
        const auto back = decode_request(encode(r));
        ASSERT_TRUE(back.has_value()) << encode(r) << " -> " << back.error();
        EXPECT_EQ(*back, r) << encode(r);
    }
    EXPECT_TRUE(decode_request(R"({"cmd":"effect","name":"wave"})").has_value())
        << "speed may be omitted";
}

TEST(Ipc, StatusResponseRoundtrips) {
    Status s;
    s.profile = "default";
    s.effect = "static";
    s.effects_enabled = true;
    s.animated = false;
    s.brightness = 70;
    s.on = true;
    s.backlight = true;
    s.zones = {"FF0000", "00FF00", "0000FF", "FFFFFF"};
    s.hint = "super";
    s.active_rules = {"gamemode"};
    s.facts.power_profile = "balanced";
    s.facts.on_battery = false;
    s.facts.battery_pct = 81.5;
    s.facts.gamemode = true;
    s.backend = "mock";
    s.last_error = "boom";
    s.budget_exhausted = false;
    s.lifetime_writes = 42;
    s.warnings = {"w1"};
    const Response r = Response::success(s);
    const auto back = decode_response(encode(r));
    ASSERT_TRUE(back.has_value()) << back.error();
    EXPECT_EQ(*back, r);
    const auto err = decode_response(encode(Response::failure("nope")));
    ASSERT_TRUE(err.has_value());
    EXPECT_FALSE(err->ok);
    EXPECT_EQ(err->error, "nope");
}

TEST(Ipc, StatusFactsRoundTripFullscreenApp) {
    Status s;
    s.profile = "default";
    s.facts.fullscreen_app = "mpv";
    const std::string wire = encode(Response::success(s));
    EXPECT_NE(wire.find(R"("fullscreen_app":"mpv")"), std::string::npos) << wire;
    const auto back = decode_response(wire);
    ASSERT_TRUE(back.has_value()) << back.error();
    EXPECT_EQ(back->status.value().facts.fullscreen_app, "mpv");

    s.facts.fullscreen_app = std::nullopt;
    std::string absent = encode(Response::success(s));
    const std::string null_key = R"("fullscreen_app":null,)";
    const auto pos = absent.find(null_key);
    ASSERT_NE(pos, std::string::npos) << "absent is written as null: " << absent;
    const auto nulled = decode_response(absent);
    ASSERT_TRUE(nulled.has_value()) << nulled.error();
    EXPECT_EQ(nulled->status.value().facts.fullscreen_app, std::nullopt);

    absent.erase(pos, null_key.size()); // JSON from a daemon that predates the fact
    const auto older = decode_response(absent);
    ASSERT_TRUE(older.has_value()) << older.error();
    EXPECT_EQ(older->status.value().facts.fullscreen_app, std::nullopt);
    EXPECT_EQ(older->status.value().profile, "default");
}

TEST(Ipc, StatusReactionsRoundTrip) {
    Status s;
    s.profile = "default";
    s.reactions = ReactionsStatus{true, {"agent", "urgent"}, "workspace"};
    const std::string wire = encode(Response::success(s));
    EXPECT_NE(
        wire.find(
            R"("reactions":{"enabled":true,"lamps":["agent","urgent"],"playing":"workspace"})"),
        std::string::npos)
        << wire;
    const auto back = decode_response(wire);
    ASSERT_TRUE(back.has_value()) << back.error();
    EXPECT_EQ(back->status.value().reactions, s.reactions);

    s.reactions = ReactionsStatus{false, {}, std::nullopt};
    const std::string idle = encode(Response::success(s));
    EXPECT_NE(idle.find(R"("reactions":{"enabled":false,"lamps":[],"playing":null})"),
              std::string::npos)
        << idle;
    const auto back_idle = decode_response(idle);
    ASSERT_TRUE(back_idle.has_value()) << back_idle.error();
    EXPECT_EQ(*back_idle, Response::success(s));
}

TEST(Ipc, StatusWithoutReactionsStillDecodes) {
    Status s;
    s.profile = "default";
    s.reactions = ReactionsStatus{true, {"agent"}, "notify"};
    std::string wire = encode(Response::success(s));
    const std::size_t from = wire.find(R"(,"reactions":{)");
    ASSERT_NE(from, std::string::npos) << wire;
    wire.erase(from, wire.find('}', from) + 1 - from); // JSON from a daemon that predates it
    ASSERT_EQ(wire.find("reactions"), std::string::npos) << wire;
    const auto older = decode_response(wire);
    ASSERT_TRUE(older.has_value()) << older.error();
    EXPECT_EQ(older->status.value().reactions, ReactionsStatus{});
    EXPECT_EQ(older->status.value().profile, "default");
}

// Error text can quote raw bytes: nlohmann's parse errors echo the offending byte (a bad request
// line, a corrupt state file's warning), and CLI arguments arrive in any encoding. Encoding
// replaces invalid UTF-8 with U+FFFD; throwing would end the daemon (or abort the CLI).
TEST(Ipc, EncodingReplacesInvalidUtf8) {
    const std::string bad = "caf\xe9";
    const std::string replaced = "caf\xef\xbf\xbd";
    const auto err = decode_response(encode(Response::failure(bad)));
    ASSERT_TRUE(err.has_value()) << err.error();
    EXPECT_EQ(err->error, replaced);
    EXPECT_EQ(decode_request(encode(Request{req::Profile{bad}})), Request{req::Profile{replaced}});
    Status s;
    s.warnings = {bad};
    const auto ok = decode_response(encode(Response::success(s)));
    ASSERT_TRUE(ok.has_value()) << ok.error();
    EXPECT_EQ(ok->status.value().warnings, std::vector<std::string>{replaced});
    EXPECT_NE(encode_status(s, 2).find(replaced), std::string::npos);
}

// Decoding never yields invalid UTF-8 (ill-formed bytes and lone surrogates are rejected), so what
// a client sends cannot reach the state file or the Waybar stream as raw bytes.
TEST(Ipc, DecodingRejectsIllFormedUtf8) {
    EXPECT_FALSE(decode_request("{\"cmd\":\"profile\",\"select\":\"caf\xe9\"}").has_value());
    EXPECT_FALSE(decode_request(R"({"cmd":"profile","select":"\ud800"})").has_value());
    EXPECT_FALSE(decode_response("{\"ok\":false,\"error\":\"\xff\",\"status\":null}").has_value());
}

TEST(Ipc, BindSetsMode0600) {
    TempDir d;
    const auto p = d.path() / "s.sock";
    auto l = bind_socket(p);
    ASSERT_TRUE(l.has_value()) << l.error().message();
    struct stat st{};
    ASSERT_EQ(::stat(p.c_str(), &st), 0);
    EXPECT_EQ(st.st_mode & 0777, 0600u);
}

TEST(Ipc, BindReplacesStaleSocket) {
    TempDir d;
    const auto p = d.path() / "s.sock";
    {
        UniqueFd s(::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0));
        sockaddr_un addr{};
        addr.sun_family = AF_UNIX;
        std::strncpy(addr.sun_path, p.c_str(), sizeof(addr.sun_path) - 1);
        ASSERT_EQ(::bind(s.get(), reinterpret_cast<sockaddr *>(&addr), sizeof(addr)), 0);
    }
    ASSERT_TRUE(std::filesystem::exists(p)) << "a stale socket file is left behind";
    EXPECT_TRUE(bind_socket(p).has_value());
}

TEST(Ipc, BindRefusesWhenRunning) {
    TempDir d;
    const auto p = d.path() / "s.sock";
    auto first = bind_socket(p);
    ASSERT_TRUE(first.has_value());
    const auto second = bind_socket(p);
    ASSERT_FALSE(second.has_value());
    EXPECT_EQ(second.error().kind, IpcError::Kind::AlreadyRunning);
}

TEST(Ipc, RequestRoundtrip) {
    TempDir d;
    const auto p = d.path() / "s.sock";
    auto l = bind_socket(p);
    ASSERT_TRUE(l.has_value());
    std::string received;
    std::thread server([&] {
        UniqueFd c = accept_one(l->get());
        received = read_line(c.get());
        write_all(c.get(), encode(Response::success(std::nullopt)) + "\n");
    });
    const auto resp = request(p, req::Reload{});
    server.join();
    ASSERT_TRUE(resp.has_value()) << resp.error().message();
    EXPECT_EQ(*resp, Response::success(std::nullopt));
    EXPECT_EQ(decode_request(received).value(), Request{req::Reload{}});
}

TEST(Ipc, ReadRequestRejectsGarbage) {
    EXPECT_FALSE(decode_request(R"({"cmd":"explode"})").has_value());
    EXPECT_FALSE(decode_request("not json").has_value());
    LineBuffer b;
    b.append("{\"cmd\":\"sta");
    EXPECT_EQ(b.next_line(), std::nullopt) << "partial line";
    b.append("tus\"}\n{\"cmd\":\"reload\"}\n");
    EXPECT_EQ(b.next_line(), R"({"cmd":"status"})");
    EXPECT_EQ(b.next_line(), R"({"cmd":"reload"})");
    EXPECT_EQ(b.next_line(), std::nullopt);
}

TEST(Ipc, SubscriptionYieldsStatusesUntilClose) {
    TempDir d;
    const auto p = d.path() / "s.sock";
    auto l = bind_socket(p);
    ASSERT_TRUE(l.has_value());
    std::thread server([&] {
        UniqueFd c = accept_one(l->get());
        EXPECT_EQ(decode_request(read_line(c.get())).value(), Request{req::Subscribe{}});
        for (std::uint8_t b : {std::uint8_t{10}, std::uint8_t{20}}) {
            Status s;
            s.brightness = b;
            write_all(c.get(), encode(Response::success(s)) + "\n");
        }
    });
    auto sub = subscribe(p);
    ASSERT_TRUE(sub.has_value()) << sub.error().message();
    EXPECT_EQ(sub->next_status().value()->brightness, 10);
    EXPECT_EQ(sub->next_status().value()->brightness, 20);
    server.join();
    EXPECT_EQ(sub->next_status().value(), std::nullopt) << "server closed";
}

TEST(Ipc, RequestWithoutDaemonIsNotRunning) {
    TempDir d;
    const auto r = request(d.path() / "nobody.sock", req::Status{});
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().kind, IpcError::Kind::NotRunning);
}

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
        req::Notify{"ZapZap", "com.rtosta.zapzap", "critical"},
        req::Notify{"", "", "normal"},
        req::Agent{"waiting", "s1", "0xabc", "claude"},
        req::Agent{"end", "s1", std::nullopt, "claude"},
    };
    for (const Request &r : all) {
        const auto back = decode_request(encode(r));
        ASSERT_TRUE(back.has_value()) << encode(r) << " -> " << back.error();
        EXPECT_EQ(*back, r) << encode(r);
    }
    EXPECT_TRUE(decode_request(R"({"cmd":"effect","name":"wave"})").has_value())
        << "speed may be omitted";
}

TEST(Ipc, NotifyRoundTripsWithKebabCaseKeys) {
    const Request notify{req::Notify{"ZapZap", "com.rtosta.zapzap", "critical"}};
    EXPECT_EQ(
        encode(notify),
        R"({"cmd":"notify","app":"ZapZap","desktop-entry":"com.rtosta.zapzap","urgency":"critical"})");
    const auto back = decode_request(encode(notify));
    ASSERT_TRUE(back.has_value()) << back.error();
    EXPECT_EQ(*back, notify);
    EXPECT_FALSE(decode_request(R"({"cmd":"notify","app":"ZapZap"})").has_value())
        << "every notify field is required on the wire";
}

TEST(Ipc, AgentRoundTripsWithAndWithoutWindow) {
    const Request with{req::Agent{"waiting", "s1", "0xabc", "claude"}};
    EXPECT_EQ(
        encode(with),
        R"({"cmd":"agent","state":"waiting","session":"s1","window":"0xabc","source":"claude"})");
    const auto with_back = decode_request(encode(with));
    ASSERT_TRUE(with_back.has_value()) << with_back.error();
    EXPECT_EQ(*with_back, with);

    const Request without{req::Agent{"end", "s1", std::nullopt, "other"}};
    EXPECT_EQ(encode(without),
              R"({"cmd":"agent","state":"end","session":"s1","window":null,"source":"other"})");
    const auto without_back = decode_request(encode(without));
    ASSERT_TRUE(without_back.has_value()) << without_back.error();
    EXPECT_EQ(*without_back, without);
    const auto *agent = std::get_if<req::Agent>(&*without_back);
    ASSERT_NE(agent, nullptr);
    EXPECT_FALSE(agent->window.has_value());

    EXPECT_FALSE(decode_request(R"({"cmd":"agent","state":"done","source":"claude"})").has_value())
        << "the session is required on the wire";
}

namespace {

/// What `to_signal` made of a request that must be a signal; fails the test otherwise.
std::optional<Signal> signal_of(const Request &r) {
    const auto s = to_signal(r);
    EXPECT_TRUE(s.has_value()) << "not a signal request";
    if (!s) {
        return std::nullopt;
    }
    EXPECT_TRUE(s->has_value()) << s->error();
    if (!*s) {
        return std::nullopt;
    }
    return **s;
}

/// The error `to_signal` gave for a request that must be a bad signal request; empty otherwise.
std::string signal_error_of(const Request &r) {
    const auto s = to_signal(r);
    EXPECT_TRUE(s.has_value()) << "not a signal request";
    if (!s) {
        return {};
    }
    EXPECT_FALSE(s->has_value()) << "expected an error";
    return s->has_value() ? std::string{} : s->error();
}

/// An `agent` request from session "s" (the commas of an aggregate would split a macro argument).
Request agent_request(std::string state, std::optional<std::string> window) {
    return req::Agent{std::move(state), "s", std::move(window), "claude"};
}

Request notify_request(std::string urgency) {
    return req::Notify{"ZapZap", "com.rtosta.zapzap", std::move(urgency)};
}

} // namespace

TEST(Ipc, ToSignalMapsEveryStateAndNormalizesTheWindow) {
    EXPECT_EQ(signal_of(notify_request("Critical")),
              (Signal{signal::Notified{"ZapZap", "com.rtosta.zapzap", Urgency::Critical}}));

    // R8: lowercase hex without `0x`; the hook sends the address as `hyprctl clients -j` prints it.
    EXPECT_EQ(signal_of(agent_request("waiting", "0xABC")),
              (Signal{signal::AgentWaiting{"s", "abc"}}));
    EXPECT_EQ(signal_of(agent_request("done", "0x55666F363B20")),
              (Signal{signal::AgentDone{"s", "55666f363b20"}}));
    EXPECT_EQ(signal_of(agent_request("waiting", "ABC")),
              (Signal{signal::AgentWaiting{"s", "abc"}}))
        << "no prefix is fine";
    EXPECT_EQ(signal_of(agent_request("waiting", "0Xab")),
              (Signal{signal::AgentWaiting{"s", "ab"}}))
        << "the prefix is matched in either case";
    EXPECT_EQ(signal_of(agent_request("waiting", std::nullopt)),
              (Signal{signal::AgentWaiting{"s", std::nullopt}}));
    EXPECT_EQ(signal_of(agent_request("done", std::nullopt)),
              (Signal{signal::AgentDone{"s", std::nullopt}}));

    // `working` and `end` carry no window, whatever the request says.
    EXPECT_EQ(signal_of(agent_request("working", std::nullopt)),
              (Signal{signal::AgentWorking{"s"}}));
    EXPECT_EQ(signal_of(agent_request("working", "0xabc")), (Signal{signal::AgentWorking{"s"}}));
    EXPECT_EQ(signal_of(agent_request("end", std::nullopt)), (Signal{signal::AgentEnded{"s"}}));
    EXPECT_EQ(signal_of(agent_request("end", "0xabc")), (Signal{signal::AgentEnded{"s"}}));

    // A bad state or urgency is an error; a request that is not a signal is nullopt.
    EXPECT_FALSE(signal_error_of(agent_request("sleeping", std::nullopt)).empty());
    EXPECT_FALSE(signal_error_of(notify_request("urgent")).empty());
    EXPECT_FALSE(to_signal(req::Status{}).has_value());
}

TEST(Ipc, ToSignalAcceptsUrgencyInAnyCase) {
    const std::pair<const char *, Urgency> cases[] = {
        {"low", Urgency::Low},           {"Low", Urgency::Low},
        {"NORMAL", Urgency::Normal},     {"normal", Urgency::Normal},
        {"Critical", Urgency::Critical}, {"critical", Urgency::Critical},
    };
    for (const auto &[text, urgency] : cases) {
        EXPECT_EQ(signal_of(notify_request(text)),
                  (Signal{signal::Notified{"ZapZap", "com.rtosta.zapzap", urgency}}))
            << text;
    }
    // swaync leaves the app name and the desktop entry empty for some senders.
    EXPECT_EQ(signal_of(req::Notify{"", "", "normal"}),
              (Signal{signal::Notified{"", "", Urgency::Normal}}));
}

TEST(Ipc, ToSignalMapsAnEmptyWindowToNone) {
    // R8 and the Task 10 review: no key may ever be linked to an empty address.
    for (const char *empty : {"", "0x", "0X"}) {
        EXPECT_EQ(signal_of(agent_request("waiting", empty)),
                  (Signal{signal::AgentWaiting{"s", std::nullopt}}))
            << '"' << empty << '"';
        EXPECT_EQ(signal_of(agent_request("done", empty)),
                  (Signal{signal::AgentDone{"s", std::nullopt}}))
            << '"' << empty << '"';
    }
}

TEST(Ipc, ToSignalRejectsABadStateOrUrgency) {
    const std::string state = signal_error_of(agent_request("sleeping", std::nullopt));
    EXPECT_NE(state.find("sleeping"), std::string::npos) << state;
    EXPECT_NE(state.find("state"), std::string::npos) << state;
    EXPECT_FALSE(signal_error_of(agent_request("", std::nullopt)).empty());
    EXPECT_FALSE(signal_error_of(agent_request("Waiting", std::nullopt)).empty())
        << "states are lowercase";

    const std::string urgency = signal_error_of(notify_request("urgent"));
    EXPECT_NE(urgency.find("urgent"), std::string::npos) << urgency;
    EXPECT_NE(urgency.find("urgency"), std::string::npos) << urgency;
    EXPECT_FALSE(signal_error_of(notify_request("")).empty());
}

TEST(Ipc, ToSignalRejectsEmptyAndOverlongSessionIds) {
    const auto request = [](const char *state, std::string session) {
        return Request{req::Agent{state, std::move(session), std::nullopt, "claude"}};
    };
    static_assert(kMaxSessionBytes == 128);
    const std::string longest(kMaxSessionBytes, 'a');
    const std::string too_long(kMaxSessionBytes + 1, 'a');
    // Two-byte characters: 64 of them are exactly 128 bytes, 65 are 65 characters but 130 bytes.
    const auto accents = [](int n) {
        std::string s;
        for (int i = 0; i < n; ++i) {
            s += "\xC3\xA9";
        }
        return s;
    };
    const std::string wide_ok = accents(64);
    const std::string wide_too_long = accents(65);

    for (const char *state : {"waiting", "working", "done", "end"}) {
        const std::string empty_error = signal_error_of(request(state, ""));
        EXPECT_NE(empty_error.find("session"), std::string::npos) << state << ": " << empty_error;
        EXPECT_NE(empty_error.find("128"), std::string::npos) << state << ": " << empty_error;
        const std::string long_error = signal_error_of(request(state, too_long));
        EXPECT_NE(long_error.find("session"), std::string::npos) << state << ": " << long_error;
        EXPECT_FALSE(signal_error_of(request(state, wide_too_long)).empty()) << state;

        EXPECT_TRUE(signal_of(request(state, longest)).has_value()) << state;
        EXPECT_TRUE(signal_of(request(state, "s")).has_value()) << state;
        EXPECT_TRUE(signal_of(request(state, wide_ok)).has_value()) << state;
    }
    EXPECT_EQ(signal_of(request("end", longest)), (Signal{signal::AgentEnded{longest}}));
    EXPECT_EQ(signal_of(request("waiting", longest)),
              (Signal{signal::AgentWaiting{longest, std::nullopt}}));

    // An unknown state keeps its own error, whatever the session is.
    EXPECT_NE(signal_error_of(request("sleeping", "")).find("sleeping"), std::string::npos);

    // Other requests carry no session: the limit does not touch them.
    EXPECT_EQ(signal_of(req::Notify{"", "", "normal"}),
              (Signal{signal::Notified{"", "", Urgency::Normal}}));
    EXPECT_FALSE(to_signal(req::Status{}).has_value());
}

TEST(Ipc, ToSignalIsNulloptForRequestsThatAreNotSignals) {
    EXPECT_FALSE(to_signal(req::Status{}).has_value());
    EXPECT_FALSE(to_signal(req::Subscribe{}).has_value());
    EXPECT_FALSE(to_signal(req::Reload{}).has_value());
    EXPECT_FALSE(to_signal(req::Hint{"super"}).has_value());
    EXPECT_FALSE(to_signal(req::Power{req::PowerAction::Toggle}).has_value());
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

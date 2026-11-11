#include "cli/cli.hpp"
#include "cli/format.hpp"
#include "util/strings.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <sstream>
#include <string>
#include <vector>

using namespace omenkbd;

namespace {

/// Parses `omen-kbd <args...>`; returns the request or the parse error.
std::pair<std::optional<Request>, std::string> parse(std::vector<const char *> args) {
    args.insert(args.begin(), "omen-kbd");
    std::string err;
    auto r = parse_args_to_request(static_cast<int>(args.size()), args.data(), &err);
    return {std::move(r), err};
}

Status sample() {
    Status s;
    s.profile = "default";
    s.effect = "static";
    s.brightness = 70;
    s.on = true;
    s.backlight = true;
    s.zones = {"FF0000", "00FF00", "0000FF", "FFFFFF"};
    s.backend = "mock";
    s.lifetime_writes = 42;
    return s;
}

std::vector<std::string> lines_of(const std::string &text) {
    std::vector<std::string> out;
    std::istringstream in(text);
    for (std::string line; std::getline(in, line);) {
        out.push_back(line);
    }
    return out;
}

const std::string *line_with_label(const std::vector<std::string> &lines, std::string_view label) {
    for (const auto &l : lines) {
        if (l.starts_with(label) && l.size() > label.size() && l[label.size()] == ' ') {
            return &l;
        }
    }
    return nullptr;
}

} // namespace

TEST(Cli, BrightnessAcceptsNegativeDelta) {
    const auto [r, err] = parse({"brightness", "-10"});
    ASSERT_TRUE(r.has_value()) << err;
    EXPECT_EQ(*r, Request{req::Brightness{"-10"}});
    EXPECT_EQ(parse({"brightness", "+5"}).first, Request{req::Brightness{"+5"}});
    EXPECT_EQ(parse({"brightness", "40"}).first, Request{req::Brightness{"40"}});
}

TEST(Cli, SetRequiresOneOrFourColors) {
    EXPECT_EQ(parse({"set", "#ff0000"}).first, Request{req::Set{{"#ff0000"}}});
    EXPECT_EQ(parse({"set", "#ff0000", "#00ff00", "#0000ff", "theme:blue"}).first,
              (Request{req::Set{{"#ff0000", "#00ff00", "#0000ff", "theme:blue"}}}));
    const auto [two, err] = parse({"set", "#ff0000", "#00ff00"});
    EXPECT_FALSE(two.has_value());
    EXPECT_NE(err.find("1 or 4"), std::string::npos) << err;
    EXPECT_FALSE(parse({"set"}).first.has_value());
    EXPECT_FALSE(parse({"set", "a", "b", "c", "d", "e"}).first.has_value());
}

TEST(Cli, CommandsMapToRequests) {
    EXPECT_EQ(parse({"status"}).first, Request{req::Status{}});
    EXPECT_EQ(parse({"status", "--json"}).first, Request{req::Status{}});
    EXPECT_EQ(parse({"profile", "next"}).first, Request{req::Profile{"next"}});
    EXPECT_EQ(parse({"on"}).first, Request{req::Power{req::PowerAction::On}});
    EXPECT_EQ(parse({"off"}).first, Request{req::Power{req::PowerAction::Off}});
    EXPECT_EQ(parse({"toggle"}).first, Request{req::Power{req::PowerAction::Toggle}});
    EXPECT_EQ(parse({"effect", "wave"}).first, (Request{req::Effect{"wave", std::nullopt}}));
    EXPECT_EQ(parse({"effect", "breathe", "--speed", "80"}).first,
              (Request{req::Effect{"breathe", std::uint8_t{80}}}));
    EXPECT_FALSE(parse({"effect", "wave", "--speed", "0"}).first.has_value());
    EXPECT_EQ(parse({"hint", "super+shift"}).first, Request{req::Hint{"super+shift"}});
    EXPECT_EQ(parse({"idle", "on"}).first, Request{req::Idle{true}});
    EXPECT_FALSE(parse({"idle", "maybe"}).first.has_value());
    EXPECT_EQ(parse({"flag", "locked", "off"}).first, (Request{req::Flag{"locked", false}}));
    EXPECT_EQ(parse({"reload"}).first, Request{req::Reload{}});
    const auto [daemon, err] = parse({"daemon", "--backend", "mock", "--no-sources"});
    EXPECT_FALSE(daemon.has_value()) << "not a request";
    EXPECT_EQ(err, "");
    for (const char *local : {"doctor", "gen-hyprland"}) {
        const auto [r, e] = parse({local});
        EXPECT_FALSE(r.has_value()) << local << " is not a request";
        EXPECT_EQ(e, "") << local;
    }
    EXPECT_FALSE(parse({"explode"}).first.has_value());
}

// Waybar renders text and tooltip as Pango markup; user text must not break it.
TEST(Cli, WaybarEscapesPangoMarkup) {
    Status s = sample();
    s.profile = "R&D <night>";
    s.last_error = "can't write \"zones\" & <backlight>";
    const auto j = waybar_json(s);
    EXPECT_EQ(j["text"], "󰌌 R&amp;D &lt;night&gt;");
    const auto tooltip = j["tooltip"].get<std::string>();
    EXPECT_NE(tooltip.find("R&amp;D &lt;night&gt;"), std::string::npos) << tooltip;
    EXPECT_NE(tooltip.find("can't write \"zones\" &amp; &lt;backlight&gt;"), std::string::npos)
        << tooltip;
    EXPECT_EQ(tooltip.find("R&D"), std::string::npos);
    EXPECT_EQ(format_status(s).find("&amp;"), std::string::npos) << "plain text for the CLI";
}

TEST(Cli, WaybarOnOffHintError) {
    const auto on = waybar_json(sample());
    EXPECT_EQ(on["text"], "󰌌 default");
    EXPECT_EQ(on["class"], "on");
    EXPECT_EQ(on["percentage"], 70);
    EXPECT_NE(on["tooltip"].get<std::string>().find("profile"), std::string::npos);

    Status off = sample();
    off.on = false;
    EXPECT_EQ(waybar_json(off)["text"], "󰌐 off");
    EXPECT_EQ(waybar_json(off)["class"], "off");

    Status hint = sample();
    hint.hint = "super";
    EXPECT_EQ(waybar_json(hint)["class"], "hint");
    EXPECT_EQ(waybar_json(hint)["text"], "󰌌 default");

    Status effect = sample();
    effect.animated = true;
    EXPECT_EQ(waybar_json(effect)["class"], "effect");

    Status error = hint;
    error.last_error = "boom";
    EXPECT_EQ(waybar_json(error)["class"], "error");
    Status budget = sample();
    budget.budget_exhausted = true;
    EXPECT_EQ(waybar_json(budget)["class"], "error");
}

TEST(Cli, FormatStatusHasPinnedLabels) {
    Status s = sample();
    s.hint = "super";
    s.active_rules = {"gamemode", "power saver dims"};
    const auto plain = lines_of(format_status(s));
    for (const char *label : {"profile", "zones", "brightness", "hint", "rules", "backend"}) {
        EXPECT_NE(line_with_label(plain, label), nullptr) << "missing label " << label;
    }
    for (const char *label : {"error", "budget", "warnings"}) {
        EXPECT_EQ(line_with_label(plain, label), nullptr) << label << " only when present";
    }
    EXPECT_NE(line_with_label(plain, "zones")->find("L #FF0000  M #00FF00  R #0000FF  W #FFFFFF"),
              std::string::npos);
    EXPECT_NE(line_with_label(plain, "brightness")->find("70%  on  backlight on"),
              std::string::npos);
    EXPECT_NE(line_with_label(plain, "hint")->find("super"), std::string::npos);
    EXPECT_NE(line_with_label(plain, "rules")->find("gamemode, power saver dims"),
              std::string::npos);
    EXPECT_NE(line_with_label(plain, "backend")->find("mock  writes 42"), std::string::npos);

    s.last_error = "I/O error: boom";
    s.budget_exhausted = true;
    s.warnings = {"w1", "w2"};
    const auto full = lines_of(format_status(s));
    ASSERT_NE(line_with_label(full, "error"), nullptr);
    EXPECT_NE(line_with_label(full, "error")->find("boom"), std::string::npos);
    EXPECT_NE(line_with_label(full, "budget"), nullptr);
    EXPECT_NE(line_with_label(full, "warnings"), nullptr);
}

namespace {

/// Runs the real `cli_main` on `omen-kbd <args...>` and returns its exit code. Only for command
/// lines that fail to parse: a valid request would connect to the user's daemon socket. So a line
/// that does parse is not run at all: the result is -1 (never a real exit code) and `stderr_text`
/// says why. A validator that regressed then fails the test instead of reaching the real daemon.
int exit_code_of(std::vector<std::string> args, std::string *stderr_text) {
    std::vector<const char *> raw;
    for (const auto &a : args) {
        raw.push_back(a.c_str());
    }
    if (parse(raw).first.has_value()) {
        *stderr_text = "exit_code_of: these arguments parse, so cli_main would contact the daemon";
        return -1;
    }
    args.insert(args.begin(), "omen-kbd");
    std::vector<char *> argv;
    for (auto &a : args) {
        argv.push_back(a.data());
    }
    testing::internal::CaptureStderr();
    const int code = cli_main(static_cast<int>(argv.size()), argv.data());
    *stderr_text = testing::internal::GetCapturedStderr();
    return code;
}

} // namespace

TEST(Cli, NotifyParsesOptionsAndDefaults) {
    EXPECT_EQ(parse({"notify"}).first, (Request{req::Notify{"", "", "normal"}}));
    EXPECT_EQ(parse({"notify", "--app", "ZapZap", "--desktop-entry", "com.rtosta.zapzap",
                     "--urgency", "critical"})
                  .first,
              (Request{req::Notify{"ZapZap", "com.rtosta.zapzap", "critical"}}));
    EXPECT_EQ(parse({"notify", "--urgency", "low"}).first, (Request{req::Notify{"", "", "low"}}));
    EXPECT_EQ(parse({"notify", "--app", "Firefox"}).first,
              (Request{req::Notify{"Firefox", "", "normal"}}));

    // swaync passes its urgency capitalised; the request carries the canonical lowercase name.
    for (const char *capitalised : {"Low", "Normal", "Critical", "CRITICAL"}) {
        const auto [r, err] = parse({"notify", "--urgency", capitalised});
        ASSERT_TRUE(r.has_value()) << capitalised << ": " << err;
        const auto *n = std::get_if<req::Notify>(&*r);
        ASSERT_NE(n, nullptr);
        EXPECT_EQ(n->urgency, to_lower(capitalised)) << capitalised;
    }
}

// The argument form of packaging/snippets/notify-reaction.sh: swaync leaves some variables empty.
TEST(Cli, NotifyAcceptsEmptyValuesAndTheSnippetsArgumentForm) {
    EXPECT_EQ(parse({"notify", "--app", "", "--desktop-entry", "", "--urgency", "Normal"}).first,
              (Request{req::Notify{"", "", "normal"}}));
    EXPECT_EQ(parse({"notify", "--app", "Some App", "--desktop-entry", "org.some.App", "--urgency",
                     "Low"})
                  .first,
              (Request{req::Notify{"Some App", "org.some.App", "low"}}));
}

TEST(Cli, NotifyRejectsAnUnknownUrgencyWithUsage64) {
    for (const char *bad : {"urgent", "", "high", "0"}) {
        const auto [r, err] = parse({"notify", "--urgency", bad});
        EXPECT_FALSE(r.has_value()) << '"' << bad << '"';
        EXPECT_NE(err.find("--urgency"), std::string::npos) << err;
    }
    std::string text;
    EXPECT_EQ(exit_code_of({"notify", "--urgency", "urgent"}, &text), 64);
    EXPECT_NE(text.find("urgency"), std::string::npos) << text;
    EXPECT_EQ(exit_code_of({"notify", "--app", "x", "--urgency", ""}, &text), 64);
}

TEST(Cli, AgentRequiresSessionAndAValidState) {
    EXPECT_EQ(parse({"agent", "waiting", "--session", "s1"}).first,
              (Request{req::Agent{"waiting", "s1", std::nullopt, "claude"}}));

    const auto [no_session, session_err] = parse({"agent", "waiting"});
    EXPECT_FALSE(no_session.has_value());
    EXPECT_NE(session_err.find("--session"), std::string::npos) << session_err;
    const auto [no_state, state_err] = parse({"agent", "--session", "s1"});
    EXPECT_FALSE(no_state.has_value());
    EXPECT_FALSE(state_err.empty());
    EXPECT_FALSE(parse({"agent"}).first.has_value());

    for (const char *bad : {"sleeping", "Waiting", "", "wait"}) {
        const auto [r, err] = parse({"agent", bad, "--session", "s1"});
        EXPECT_FALSE(r.has_value()) << '"' << bad << '"';
        EXPECT_FALSE(err.empty()) << '"' << bad << '"';
    }

    std::string text;
    EXPECT_EQ(exit_code_of({"agent", "waiting"}, &text), 64);
    EXPECT_NE(text.find("--session"), std::string::npos) << text;
    EXPECT_EQ(exit_code_of({"agent", "sleeping", "--session", "s1"}, &text), 64);
    EXPECT_EQ(exit_code_of({"agent", "--session", "s1"}, &text), 64);
}

TEST(Cli, AgentRejectsEmptyAndOverlongSessionWithUsage64) {
    const std::string longest(kMaxSessionBytes, 'a');
    const std::string too_long(kMaxSessionBytes + 1, 'a');
    for (const std::string &bad : {std::string(), too_long}) {
        const auto [r, err] = parse({"agent", "waiting", "--session", bad.c_str()});
        EXPECT_FALSE(r.has_value()) << bad.size() << " bytes";
        EXPECT_NE(err.find("--session"), std::string::npos) << err;
        EXPECT_NE(err.find("128"), std::string::npos) << err;
    }
    std::string text;
    EXPECT_EQ(exit_code_of({"agent", "waiting", "--session", ""}, &text), 64);
    EXPECT_NE(text.find("--session"), std::string::npos) << text;
    EXPECT_EQ(exit_code_of({"agent", "end", "--session", too_long}, &text), 64);
    EXPECT_NE(text.find("--session"), std::string::npos) << text;

    // Exactly the limit is fine, for every state.
    for (const char *state : {"waiting", "working", "done", "end"}) {
        EXPECT_EQ(parse({"agent", state, "--session", longest.c_str()}).first,
                  (Request{req::Agent{state, longest, std::nullopt, "claude"}}))
            << state;
    }
    EXPECT_EQ(parse({"agent", "waiting", "--session", "x"}).first,
              (Request{req::Agent{"waiting", "x", std::nullopt, "claude"}}));
}

TEST(Cli, AgentParsesWindowAndSource) {
    EXPECT_EQ(
        parse({"agent", "waiting", "--session", "s1", "--window", "0xABC", "--source", "other"})
            .first,
        (Request{req::Agent{"waiting", "s1", "0xABC", "other"}}))
        << "the CLI sends the address as given; to_signal normalizes it";
    EXPECT_EQ(parse({"agent", "done", "--session", "s1", "--window", "0x55666f363b20"}).first,
              (Request{req::Agent{"done", "s1", "0x55666f363b20", "claude"}}))
        << "--source defaults to claude";
    EXPECT_EQ(parse({"agent", "end", "--session", "s1"}).first,
              (Request{req::Agent{"end", "s1", std::nullopt, "claude"}}));
    EXPECT_EQ(parse({"agent", "--session", "s1", "--source", "codex", "working"}).first,
              (Request{req::Agent{"working", "s1", std::nullopt, "codex"}}))
        << "options may come before the state";
}

// T14-5: the daemon refuses a window that is not an address, so the CLI says so first, as a usage
// error. An empty --window is still accepted and names no window.
TEST(Cli, AgentRejectsAnInvalidWindowWithUsage64) {
    for (const char *bad : {"zz", "0x", "w1", "0x0xab", "0xab "}) {
        const auto [r, err] = parse({"agent", "waiting", "--session", "s1", "--window", bad});
        EXPECT_FALSE(r.has_value()) << '"' << bad << '"';
        EXPECT_NE(err.find("--window"), std::string::npos) << err;
    }
    const std::string too_long(65, 'a');
    EXPECT_FALSE(parse({"agent", "done", "--session", "s1", "--window", too_long.c_str()})
                     .first.has_value());
    const std::string longest = "0x" + std::string(64, 'A');
    EXPECT_EQ(parse({"agent", "done", "--session", "s1", "--window", longest.c_str()}).first,
              (Request{req::Agent{"done", "s1", longest, "claude"}}))
        << "the CLI sends a valid address as given; to_signal normalizes it";
    EXPECT_EQ(parse({"agent", "waiting", "--session", "s1", "--window", ""}).first,
              (Request{req::Agent{"waiting", "s1", std::nullopt, "claude"}}))
        << "an empty window is no window, not an error";

    // Only once parsing is known to fail: otherwise this would reach a running daemon.
    ASSERT_FALSE(
        parse({"agent", "waiting", "--session", "s1", "--window", "zz"}).first.has_value());
    std::string text;
    EXPECT_EQ(exit_code_of({"agent", "waiting", "--session", "s1", "--window", "zz"}, &text), 64);
    EXPECT_NE(text.find("--window"), std::string::npos) << text;
}

// Review of the minors, Minor 1: `working` and `end` carry no window, so a wrapper that always adds
// `--window` (even a bad one) must still be able to clear a lamp: it parses, and the request has
// none.
TEST(Cli, AgentWorkingAndEndIgnoreAnyWindow) {
    for (const char *state : {"working", "end"}) {
        for (const char *window : {"zz", "0x", "0xABC", "w1"}) {
            EXPECT_EQ(parse({"agent", state, "--session", "s1", "--window", window}).first,
                      (Request{req::Agent{state, "s1", std::nullopt, "claude"}}))
                << state << " --window " << window;
        }
    }
}

// The argument forms packaging/agent-hook/omen-kbd-agent-hook uses, with a real address.
TEST(Cli, AgentAcceptsTheHooksArgumentForms) {
    const char *session = "0b7b3a54-1c6e-4f43-9c0e-2a8d3e1f9a10";
    EXPECT_EQ(parse({"agent", "waiting", "--session", session, "--window", "0x55666f363b20",
                     "--source", "claude"})
                  .first,
              (Request{req::Agent{"waiting", session, "0x55666f363b20", "claude"}}));
    EXPECT_EQ(parse({"agent", "waiting", "--session", session, "--source", "claude"}).first,
              (Request{req::Agent{"waiting", session, std::nullopt, "claude"}}))
        << "a session without a window";
    EXPECT_EQ(parse({"agent", "working", "--session", session}).first,
              (Request{req::Agent{"working", session, std::nullopt, "claude"}}));
    EXPECT_EQ(parse({"agent", "done", "--session", session}).first,
              (Request{req::Agent{"done", session, std::nullopt, "claude"}}));
    EXPECT_EQ(parse({"agent", "end", "--session", session}).first,
              (Request{req::Agent{"end", session, std::nullopt, "claude"}}));
    // The hook accepts ids made of [A-Za-z0-9_-], so an id may start with a dash.
    EXPECT_EQ(parse({"agent", "end", "--session", "-abc_9"}).first,
              (Request{req::Agent{"end", "-abc_9", std::nullopt, "claude"}}));
}

TEST(Format, StatusShowsReactionsOffWhenDisabled) {
    const std::string text = format_status(sample());
    EXPECT_NE(text.find("reactions   off\n"), std::string::npos) << text;

    const auto lines = lines_of(text);
    const auto rules =
        std::ranges::find_if(lines, [](const std::string &l) { return l.starts_with("rules "); });
    ASSERT_NE(rules, lines.end());
    ASSERT_NE(rules + 1, lines.end());
    EXPECT_EQ(*(rules + 1), "reactions   off") << "the line follows `rules`";
}

TEST(Format, StatusShowsLampsAndTheClipPlaying) {
    Status s = sample();
    s.reactions.enabled = true;
    s.reactions.lamps = {"agent", "urgent"};
    s.reactions.playing = "workspace";
    const std::string text = format_status(s);
    EXPECT_NE(text.find("reactions   on  lamps agent,urgent  playing workspace\n"),
              std::string::npos)
        << text;
}

TEST(Format, StatusShowsOnAloneAndEachPartOnItsOwn) {
    Status s = sample();
    s.reactions.enabled = true;
    EXPECT_NE(format_status(s).find("reactions   on\n"), std::string::npos) << format_status(s);

    s.reactions.lamps = {"agent"};
    EXPECT_NE(format_status(s).find("reactions   on  lamps agent\n"), std::string::npos)
        << format_status(s);

    s.reactions.lamps.clear();
    s.reactions.playing = "agent-done";
    EXPECT_NE(format_status(s).find("reactions   on  playing agent-done\n"), std::string::npos)
        << format_status(s);

    s.reactions.enabled = false;
    EXPECT_NE(format_status(s).find("reactions   off\n"), std::string::npos)
        << "off shows nothing else";
    EXPECT_EQ(format_status(s).find("playing"), std::string::npos);
}

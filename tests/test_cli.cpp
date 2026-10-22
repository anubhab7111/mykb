#include "cli/cli.hpp"
#include "cli/format.hpp"

#include <gtest/gtest.h>

#include <sstream>

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

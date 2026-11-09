#include "config.hpp"
#include "reactions_config.hpp"

#include <gtest/gtest.h>
#include <string>
#include <string_view>
#include <utility>

using namespace omenkbd;

namespace {

constexpr std::string_view kMinimal = "[[profiles]]\nname='a'\ncolors=['#000000']\n";

/// A full config: the given `[reactions]` text followed by the one profile a config needs.
std::expected<Config, std::string> parse(std::string_view reactions) {
    return parse_config(std::string(reactions) + "\n" + std::string(kMinimal));
}

/// The `[reactions]` table of `reactions`, which must parse.
ReactionsCfg reactions_of(std::string_view reactions) {
    const auto c = parse(reactions);
    EXPECT_TRUE(c.has_value()) << reactions << " -> " << (c ? "" : c.error());
    return c ? c->reactions : ReactionsCfg{};
}

} // namespace

TEST(ReactionsConfig, MissingTableMeansOff) {
    const auto c = parse_config(kMinimal);
    ASSERT_TRUE(c.has_value()) << c.error();
    EXPECT_FALSE(c->reactions.enabled);
    // The rest is still the spec defaults, so a Config built by hand and one parsed agree.
    ReactionsCfg expected = default_reactions();
    EXPECT_FALSE(expected.enabled);
    EXPECT_TRUE(c->reactions == expected);
    EXPECT_TRUE(Config{}.reactions == expected);
}

TEST(ReactionsConfig, EmptyTableTurnsItOnWithTheSpecDefaults) {
    const ReactionsCfg r = reactions_of("[reactions]");
    EXPECT_TRUE(r.enabled);
    EXPECT_EQ(r.step_ms, 120u);
    EXPECT_EQ(r.blink_ms, 150u);
    EXPECT_EQ(r.max_per_minute, 30u);

    EXPECT_EQ(r.workspace.color.to_string(), "theme:lavender");
    EXPECT_EQ(r.workspace.wasd_with, WasdWith::Left);

    EXPECT_DOUBLE_EQ(r.window.open, 0.5);
    EXPECT_DOUBLE_EQ(r.window.close, 0.35);
    EXPECT_EQ(r.window.cooldown_ms, 500u);

    EXPECT_EQ(r.notify.color.to_string(), "theme:blue");
    EXPECT_EQ(r.notify.critical.to_string(), "theme:red");
    EXPECT_EQ(r.notify.min_urgency, Urgency::Normal);
    EXPECT_TRUE(r.notify.skip_if_focused);
    ASSERT_EQ(r.notify.apps.size(), 2u);
    EXPECT_EQ(r.notify.apps.at("zapzap").to_string(), "theme:green");
    EXPECT_EQ(r.notify.apps.at("discord").to_string(), "#5865F2");

    EXPECT_EQ(r.urgent.zones, std::vector<Zone>{Zone::Right});
    EXPECT_EQ(r.urgent.color.to_string(), "theme:yellow");
    EXPECT_EQ(r.urgent.blinks, 2);

    EXPECT_EQ(r.agent.zones, std::vector<Zone>{Zone::Wasd});
    EXPECT_EQ(r.agent.waiting.to_string(), "theme:peach");
    EXPECT_EQ(r.agent.done.to_string(), "theme:green");
    EXPECT_EQ(r.agent.blinks, 2);
    EXPECT_EQ(r.agent.ttl_s, 3600u);

    // The same table parsed straight from toml++ gives the same result.
    const auto direct = parse_reactions(toml::parse(""));
    ASSERT_TRUE(direct.has_value()) << direct.error();
    EXPECT_TRUE(*direct == r);
}

TEST(ReactionsConfig, ExplicitValuesOverrideTheDefaults) {
    const ReactionsCfg r = reactions_of(R"(
[reactions]
enabled = false
step_ms = 80
blink_ms = 200
max_per_minute = 10
[reactions.workspace]
color = "#112233"
wasd_with = "NONE"
[reactions.window]
open = 1
close = 0
cooldown_ms = 0
[reactions.notify]
color = "theme:mauve"
critical = "#ff0000"
min_urgency = "Low"
skip_if_focused = false
[reactions.urgent]
zones = ["Left", "wasd"]
color = "#00ff00"
blinks = 0
[reactions.agent]
zones = ["middle"]
waiting = "theme:teal"
done = "#0000ff"
blinks = 5
ttl_s = 60
)");
    EXPECT_FALSE(r.enabled);
    EXPECT_EQ(r.step_ms, 80u);
    EXPECT_EQ(r.blink_ms, 200u);
    EXPECT_EQ(r.max_per_minute, 10u);
    EXPECT_EQ(r.workspace.color.to_string(), "#112233");
    EXPECT_EQ(r.workspace.wasd_with, WasdWith::None);
    EXPECT_DOUBLE_EQ(r.window.open, 1.0);
    EXPECT_DOUBLE_EQ(r.window.close, 0.0);
    EXPECT_EQ(r.window.cooldown_ms, 0u);
    EXPECT_EQ(r.notify.color.to_string(), "theme:mauve");
    EXPECT_EQ(r.notify.critical.to_string(), "#FF0000");
    EXPECT_EQ(r.notify.min_urgency, Urgency::Low);
    EXPECT_FALSE(r.notify.skip_if_focused);
    EXPECT_EQ(r.notify.apps.size(), 2u); // no [reactions.notify.apps]: the defaults stay
    EXPECT_EQ(r.urgent.zones, (std::vector<Zone>{Zone::Left, Zone::Wasd}));
    EXPECT_EQ(r.urgent.color.to_string(), "#00FF00");
    EXPECT_EQ(r.urgent.blinks, 0);
    EXPECT_EQ(r.agent.zones, std::vector<Zone>{Zone::Middle});
    EXPECT_EQ(r.agent.waiting.to_string(), "theme:teal");
    EXPECT_EQ(r.agent.done.to_string(), "#0000FF");
    EXPECT_EQ(r.agent.blinks, 5);
    EXPECT_EQ(r.agent.ttl_s, 60u);
}

TEST(ReactionsConfig, ExplicitEnabledFalseKeepsItOffButStillValidates) {
    EXPECT_FALSE(reactions_of("[reactions]\nenabled = false").enabled);
    EXPECT_TRUE(reactions_of("[reactions]\nenabled = true").enabled);
    const auto bad = parse("[reactions]\nenabled = false\nstep_ms = 5");
    ASSERT_FALSE(bad.has_value());
    EXPECT_NE(bad.error().find("step_ms"), std::string::npos) << bad.error();
    const auto wrong_type = parse("[reactions]\nenabled = 1");
    ASSERT_FALSE(wrong_type.has_value());
    EXPECT_EQ(wrong_type.error(), "reactions: `enabled` must be true or false");
}

TEST(ReactionsConfig, UnknownKeysAreErrorsNamingTheField) {
    const auto root = parse("[reactions]\nbogus = 1");
    ASSERT_FALSE(root.has_value());
    EXPECT_EQ(root.error(), "reactions: unknown field `bogus`");

    const auto window = parse("[reactions.window]\nx = 1");
    ASSERT_FALSE(window.has_value());
    EXPECT_EQ(window.error(), "reactions.window: unknown field `x`");

    const std::pair<std::string_view, std::string_view> cases[] = {
        {"[reactions.workspace]\nzones = []", "reactions.workspace: unknown field `zones`"},
        {"[reactions.notify]\ncolour = '#000000'", "reactions.notify: unknown field `colour`"},
        {"[reactions.urgent]\nttl_s = 5", "reactions.urgent: unknown field `ttl_s`"},
        {"[reactions.agent]\nzone = ['wasd']", "reactions.agent: unknown field `zone`"},
        {"[reactions.sound]\non = true", "reactions: unknown field `sound`"},
    };
    for (const auto &[snippet, expected] : cases) {
        const auto c = parse(snippet);
        ASSERT_FALSE(c.has_value()) << snippet;
        EXPECT_EQ(c.error(), expected) << snippet;
    }
}

TEST(ReactionsConfig, EveryBoundIsEnforced) {
    // Each case is out of range by one. The error names the table and the field.
    const std::pair<std::string_view, std::string_view> cases[] = {
        {"[reactions]\nstep_ms = 59", "reactions: `step_ms` must be 60-1000, got 59"},
        {"[reactions]\nstep_ms = 1001", "reactions: `step_ms` must be 60-1000, got 1001"},
        {"[reactions]\nblink_ms = 59", "reactions: `blink_ms` must be 60-1000, got 59"},
        {"[reactions]\nblink_ms = 1001", "reactions: `blink_ms` must be 60-1000, got 1001"},
        {"[reactions]\nmax_per_minute = 0", "reactions: `max_per_minute` must be 1-600, got 0"},
        {"[reactions]\nmax_per_minute = 601", "reactions: `max_per_minute` must be 1-600, got 601"},
        {"[reactions.window]\nopen = -0.1", "reactions.window: `open` must be 0-1, got -0.1"},
        {"[reactions.window]\nopen = 1.5", "reactions.window: `open` must be 0-1, got 1.5"},
        {"[reactions.window]\nclose = -0.1", "reactions.window: `close` must be 0-1, got -0.1"},
        {"[reactions.window]\nclose = 1.5", "reactions.window: `close` must be 0-1, got 1.5"},
        {"[reactions.window]\ncooldown_ms = -1",
         "reactions.window: `cooldown_ms` must be 0-10000, got -1"},
        {"[reactions.window]\ncooldown_ms = 10001",
         "reactions.window: `cooldown_ms` must be 0-10000, got 10001"},
        {"[reactions.urgent]\nblinks = 6", "reactions.urgent: `blinks` must be 0-5, got 6"},
        {"[reactions.urgent]\nblinks = -1", "reactions.urgent: `blinks` must be 0-5, got -1"},
        {"[reactions.agent]\nblinks = 6", "reactions.agent: `blinks` must be 0-5, got 6"},
        {"[reactions.agent]\nblinks = -1", "reactions.agent: `blinks` must be 0-5, got -1"},
        {"[reactions.agent]\nttl_s = 59", "reactions.agent: `ttl_s` must be 60-86400, got 59"},
        {"[reactions.agent]\nttl_s = 86401",
         "reactions.agent: `ttl_s` must be 60-86400, got 86401"},
    };
    for (const auto &[snippet, expected] : cases) {
        const auto c = parse(snippet);
        ASSERT_FALSE(c.has_value()) << snippet;
        EXPECT_EQ(c.error(), expected) << snippet;
    }
}

TEST(ReactionsConfig, BlinksCannotCarryAnUnboundedCount) {
    // `flash` loops `blinks - 1` times with no bound of its own, so these must never get through,
    // including values that would wrap if they were narrowed to int first.
    const std::pair<std::string_view, std::string_view> cases[] = {
        {"[reactions.urgent]\nblinks = 2147483648", "reactions.urgent: `blinks` must be 0-5"},
        {"[reactions.urgent]\nblinks = 4294967297", "reactions.urgent: `blinks` must be 0-5"},
        {"[reactions.agent]\nblinks = 9223372036854775807",
         "reactions.agent: `blinks` must be 0-5"},
        {"[reactions.agent]\nblinks = -9223372036854775808",
         "reactions.agent: `blinks` must be 0-5"},
        {"[reactions]\nstep_ms = 4294967356", "reactions: `step_ms` must be 60-1000"},
        {"[reactions]\nblink_ms = 4294967356", "reactions: `blink_ms` must be 60-1000"},
    };
    for (const auto &[snippet, expected] : cases) {
        const auto c = parse(snippet);
        ASSERT_FALSE(c.has_value()) << snippet;
        EXPECT_TRUE(c.error().starts_with(expected)) << snippet << " -> " << c.error();
    }
}

TEST(ReactionsConfig, BoundsAreInclusiveAndNonNumbersAreRejected) {
    const ReactionsCfg low = reactions_of(R"(
[reactions]
step_ms = 60
blink_ms = 60
max_per_minute = 1
[reactions.window]
open = 0
close = 0
cooldown_ms = 0
[reactions.urgent]
blinks = 0
[reactions.agent]
blinks = 0
ttl_s = 60
)");
    EXPECT_EQ(low.step_ms, 60u);
    EXPECT_EQ(low.blink_ms, 60u);
    EXPECT_EQ(low.max_per_minute, 1u);
    EXPECT_DOUBLE_EQ(low.window.open, 0.0);
    EXPECT_EQ(low.urgent.blinks, 0);
    EXPECT_EQ(low.agent.ttl_s, 60u);

    const ReactionsCfg high = reactions_of(R"(
[reactions]
step_ms = 1000
blink_ms = 1000
max_per_minute = 600
[reactions.window]
open = 1.0
close = 1
cooldown_ms = 10000
[reactions.urgent]
blinks = 5
[reactions.agent]
blinks = 5
ttl_s = 86400
)");
    EXPECT_EQ(high.step_ms, 1000u);
    EXPECT_EQ(high.blink_ms, 1000u);
    EXPECT_EQ(high.max_per_minute, 600u);
    EXPECT_DOUBLE_EQ(high.window.open, 1.0);
    EXPECT_DOUBLE_EQ(high.window.close, 1.0);
    EXPECT_EQ(high.window.cooldown_ms, 10000u);
    EXPECT_EQ(high.urgent.blinks, 5);
    EXPECT_EQ(high.agent.blinks, 5);
    EXPECT_EQ(high.agent.ttl_s, 86400u);

    // NaN fails both comparisons of a naive `< lo || > hi` check, so it needs its own case.
    for (const std::string_view snippet :
         {"[reactions.window]\nopen = nan", "[reactions.window]\nclose = inf",
          "[reactions.window]\nclose = -inf"}) {
        const auto c = parse(snippet);
        ASSERT_FALSE(c.has_value()) << snippet;
        EXPECT_TRUE(c.error().starts_with("reactions.window: `")) << snippet << " -> " << c.error();
        EXPECT_NE(c.error().find("must be 0-1"), std::string::npos) << c.error();
    }

    // Wrong types are errors too: a float where an integer is needed, a string for a number.
    const std::pair<std::string_view, std::string_view> wrong[] = {
        {"[reactions]\nstep_ms = 120.0", "reactions: `step_ms` must be an integer"},
        {"[reactions.urgent]\nblinks = '2'", "reactions.urgent: `blinks` must be an integer"},
        {"[reactions.window]\nopen = 'half'", "reactions.window: `open` must be a number"},
        {"[reactions.notify]\nskip_if_focused = 'yes'",
         "reactions.notify: `skip_if_focused` must be true or false"},
    };
    for (const auto &[snippet, expected] : wrong) {
        const auto c = parse(snippet);
        ASSERT_FALSE(c.has_value()) << snippet;
        EXPECT_EQ(c.error(), expected) << snippet;
    }
}

TEST(ReactionsConfig, ZonesColorsAndUrgencyAreValidated) {
    const std::pair<std::string_view, std::string_view> cases[] = {
        {"[reactions.urgent]\nzones = ['top']", "reactions.urgent: unknown zone \"top\""},
        {"[reactions.agent]\nzones = ['wasd', 'bottom']",
         "reactions.agent: unknown zone \"bottom\""},
        {"[reactions.urgent]\nzones = [1]", "reactions.urgent: unknown zone value"},
        {"[reactions.agent]\nzones = 'wasd'", "reactions.agent: `zones` must be a list"},
        {"[reactions.workspace]\ncolor = 'nope'", "reactions.workspace: `color`: invalid color"},
        {"[reactions.workspace]\ncolor = 7", "reactions.workspace: `color` must be a string"},
        {"[reactions.notify]\ncolor = 'nope'", "reactions.notify: `color`: invalid color"},
        {"[reactions.notify]\ncritical = '#12'", "reactions.notify: `critical`: invalid color"},
        {"[reactions.urgent]\ncolor = 'theme:'", "reactions.urgent: `color`: invalid color"},
        {"[reactions.agent]\nwaiting = 'red'", "reactions.agent: `waiting`: invalid color"},
        {"[reactions.agent]\ndone = ''", "reactions.agent: `done`: invalid color"},
        {"[reactions.notify]\nmin_urgency = 'loud'",
         "reactions.notify: `min_urgency` must be \"low\", \"normal\" or \"critical\", got "
         "\"loud\""},
        {"[reactions.workspace]\nwasd_with = 'up'",
         "reactions.workspace: `wasd_with` must be \"left\", \"middle\" or \"none\", got \"up\""},
        {"[reactions.notify.apps]\nzapzap = 'nope'", "reactions.notify.apps.zapzap: invalid color"},
        {"[reactions.notify.apps]\nzapzap = 3",
         "reactions.notify.apps.zapzap must be a color string"},
        {"[reactions]\nworkspace = 1", "reactions: `workspace` must be a table"},
        {"[reactions.notify]\napps = 'zapzap'", "reactions.notify: `apps` must be a table"},
    };
    for (const auto &[snippet, expected] : cases) {
        const auto c = parse(snippet);
        ASSERT_FALSE(c.has_value()) << snippet;
        EXPECT_TRUE(c.error().starts_with(expected)) << snippet << " -> " << c.error();
    }

    // An empty zone list is allowed: it disables that lamp (spec 5.11).
    const ReactionsCfg off = reactions_of("[reactions.urgent]\nzones = []\n[reactions.agent]\n"
                                          "zones = []");
    EXPECT_TRUE(off.enabled);
    EXPECT_TRUE(off.urgent.zones.empty());
    EXPECT_TRUE(off.agent.zones.empty());

    // Zone names are case-insensitive and `center` means `middle`, as everywhere else.
    const ReactionsCfg many =
        reactions_of("[reactions.urgent]\nzones = ['RIGHT', 'Center', 'wasd']");
    EXPECT_EQ(many.urgent.zones, (std::vector<Zone>{Zone::Right, Zone::Middle, Zone::Wasd}));

    for (const auto &[name, want] : {std::pair<std::string_view, Urgency>{"low", Urgency::Low},
                                     {"Normal", Urgency::Normal},
                                     {"CRITICAL", Urgency::Critical}}) {
        EXPECT_EQ(reactions_of(std::string("[reactions.notify]\nmin_urgency = '") +
                               std::string(name) + "'")
                      .notify.min_urgency,
                  want)
            << name;
    }
    EXPECT_EQ(reactions_of("[reactions.workspace]\nwasd_with = 'middle'").workspace.wasd_with,
              WasdWith::Middle);
}

TEST(ReactionsConfig, NotifyAppKeysAreLowercased) {
    const ReactionsCfg r = reactions_of(
        "[reactions.notify]\napps = { ZapZap = \"theme:green\", \"Com.Example.App\" = '#010203' }");
    EXPECT_TRUE(r.notify.apps.contains("zapzap"));
    EXPECT_FALSE(r.notify.apps.contains("ZapZap"));
    EXPECT_TRUE(r.notify.apps.contains("com.example.app"));
    EXPECT_EQ(r.notify.apps.at("zapzap").to_string(), "theme:green");
    EXPECT_EQ(r.notify.apps.at("com.example.app").to_string(), "#010203");
    // A table that is present replaces the defaults, so `discord` is gone and the user can drop it.
    EXPECT_EQ(r.notify.apps.size(), 2u);
    EXPECT_FALSE(r.notify.apps.contains("discord"));

    // An empty table clears them all.
    EXPECT_TRUE(reactions_of("[reactions.notify.apps]").notify.apps.empty());
}

TEST(ReactionsConfig, AppKeysThatCollideAfterLowercasingAreRejected) {
    const auto c = parse("[reactions.notify.apps]\nZapZap = 'theme:green'\nzapzap = '#000000'");
    ASSERT_FALSE(c.has_value());
    EXPECT_EQ(c.error(), "reactions.notify.apps: duplicate app `zapzap`");
}

TEST(ReactionsConfig, TheShippedExampleBlockMatchesTheDefaults) {
    // packaging/config.example.toml is the embedded default and the documented block: it must say
    // what default_reactions() says, or one of the two drifted.
    ReactionsCfg expected = default_reactions();
    expected.enabled = true;
    EXPECT_TRUE(embedded_default().reactions == expected);
}

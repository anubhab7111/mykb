#include "config.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdlib>
#include <fstream>
#include <gtest/gtest.h>
#include <iterator>
#include <string>
#include <vector>

using namespace omenkbd;

TEST(Config, TheExampleConfigShipsOnlyTheGameProfile) {
    const Config c = embedded_default();
    ASSERT_EQ(c.profiles.size(), 1u);
    EXPECT_EQ(c.profiles[0].name, "game");
    EXPECT_EQ(c.profiles[0].effect, Effect::Static);
}

TEST(Config, TheOldEffectNamesAreGone) {
    for (const char *old : {"breathe", "wave", "cycle", "gradient"}) {
        const auto r =
            parse_config(std::string("[[profiles]]\nname = \"x\"\ncolors = [\"#ffffff\"]\n"
                                     "effect = \"") +
                         old + "\"\n");
        ASSERT_FALSE(r.has_value()) << old;
        EXPECT_NE(r.error().find("static, drift, glow, tide or dusk"), std::string::npos)
            << r.error();
    }
}

TEST(Config, ThemeColorsAndTheOldTablesAreRejectedWithAClearError) {
    const auto theme_color = parse_config("[[profiles]]\nname='a'\ncolors=['theme:mauve']");
    ASSERT_FALSE(theme_color.has_value());
    EXPECT_NE(theme_color.error().find("theme colors were removed"), std::string::npos)
        << theme_color.error();
    for (const char *table : {"[theme]\ncolors = 'vivid'", "[hints]\ntimeout_s = 5"}) {
        const auto r =
            parse_config(std::string(table) + "\n[[profiles]]\nname='a'\ncolors=['#000000']");
        ASSERT_FALSE(r.has_value()) << table;
        EXPECT_NE(r.error().find("unknown"), std::string::npos) << r.error();
    }
}

TEST(Config, ExampleConfigParses) {
    const Config c = embedded_default();
    EXPECT_EQ(c.profiles.size(), 1u);
    EXPECT_FALSE(c.effects.enabled);
    EXPECT_EQ(c.gates.on_idle, GateAction::Dim);
    EXPECT_EQ(c.rules.size(), 3u);
}

TEST(Config, EmbeddedDefaultEnablesReactions) {
    const Config c = embedded_default();
    EXPECT_TRUE(c.reactions.enabled);
    EXPECT_EQ(c.reactions.step_ms, 120u);
    EXPECT_FALSE(c.reactions.workspace.color) << "no color is built in";
    EXPECT_FALSE(c.reactions.notify.color);
    EXPECT_FALSE(c.reactions.agent.waiting);
    EXPECT_TRUE(c.reactions.notify.apps.empty());
}

TEST(Config, MissingFileGivesDefault) {
    const auto c = load_config("/nonexistent/x.toml");
    ASSERT_TRUE(c.has_value()) << c.error();
    EXPECT_EQ(c->profiles[0].name, "game");
}

TEST(Config, RejectsInvalid) {
    const std::pair<const char *, const char *> cases[] = {
        {"[[profiles]]\nname='a'\ncolors=['#000000','#000000']", "colors"},
        {"[effects]\nfps=30\n[[profiles]]\nname='a'\ncolors=['#000000']", "fps"},
        {"[[profiles]]\nname='a'\ncolors=['#000000']\n[[profiles]]\nname='a'\ncolors=['#000000']",
         "duplicate"},
        {"[[profiles]]\nname='custom'\ncolors=['#000000']", "custom"},
        {"[[profiles]]\nname='a'\ncolors=['#000000']\n[[rules]]\nname='r'\nwhen={}\nprofile='zzz'",
         "zzz"},
        {"[[profiles]]\nname='a'\ncolors=['#000000']\nspeed=0", "speed"},
        {"[bogus]\nx=1\n[[profiles]]\nname='a'\ncolors=['#000000']", "bogus"},
        {"", "profile"},
    };
    for (const auto &[snippet, needle] : cases) {
        const auto c = parse_config(snippet);
        ASSERT_FALSE(c.has_value()) << snippet;
        EXPECT_NE(c.error().find(needle), std::string::npos) << snippet << " -> " << c.error();
    }
}

TEST(Config, ASectionOrSubTableThatIsNotATableIsRejectedNamingItsKey) {
    // The root sections name only the key; the sub-tables of `device` lead with the
    // section, like the other errors of those sections.
    const std::pair<const char *, const char *> cases[] = {
        {"device = 1", "`device` must be a table"},
        {"effects = true", "`effects` must be a table"},
        {"gates = []", "`gates` must be a table"},
        {"[device]\nzone_map = 1", "device: `zone_map` must be a table"},
    };
    for (const auto &[snippet, expected] : cases) {
        const auto c = parse_config(std::string(snippet) + "\n[[profiles]]\nname='a'\n"
                                                           "colors=['#000000']");
        ASSERT_FALSE(c.has_value()) << snippet;
        EXPECT_EQ(c.error(), expected) << snippet;
    }
}

TEST(Config, NeedsCpuTempOnlyWithATemperatureRule) {
    EXPECT_FALSE(embedded_default().needs_cpu_temp());
    const auto c = parse_config("[[profiles]]\nname='a'\ncolors=['#000000']\n[[rules]]\n"
                                "name='hot'\nwhen={ cpu_temp_above = 90 }\nbrightness_cap = 10");
    ASSERT_TRUE(c.has_value()) << c.error();
    EXPECT_TRUE(c->needs_cpu_temp());
    ASSERT_NE(c->profile("a"), nullptr);
    EXPECT_EQ(c->profile("a")->speed, 50);
    EXPECT_EQ(c->profile("b"), nullptr);
}

TEST(Config, NeedsFullscreenOnlyWhenARuleUsesIt) {
    const auto without =
        parse_config("[[profiles]]\nname='a'\ncolors=['#000000']\n[[rules]]\n"
                     "name='hot'\nwhen={ cpu_temp_above = 90 }\nbrightness_cap = 10");
    ASSERT_TRUE(without.has_value()) << without.error();
    EXPECT_FALSE(without->needs_fullscreen());
    const auto none = parse_config("[[profiles]]\nname='a'\ncolors=['#000000']");
    ASSERT_TRUE(none.has_value()) << none.error();
    EXPECT_FALSE(none->needs_fullscreen());
    const auto with = parse_config("[[profiles]]\nname='a'\ncolors=['#000000']\n[[rules]]\n"
                                   "name='hot'\nwhen={ cpu_temp_above = 90 }\nbrightness_cap = 10\n"
                                   "[[rules]]\nname='fs'\nwhen={ fullscreen_app = ['*'] }\n"
                                   "brightness_cap = 15");
    ASSERT_TRUE(with.has_value()) << with.error();
    EXPECT_TRUE(with->needs_fullscreen());
    EXPECT_TRUE(embedded_default().needs_fullscreen()) << "the shipped config has such a rule";
}

TEST(Config, EmbeddedDefaultDimsFullscreenVideoBeforePowerSaver) {
    const Config c = embedded_default();
    const auto index = [&c](std::string_view name) {
        return std::ranges::find(c.rules, name, &Rule::name) - c.rules.begin();
    };
    const auto count = static_cast<std::ptrdiff_t>(c.rules.size());
    const auto video = index("fullscreen video dims");
    ASSERT_LT(video, count) << "the rule is missing";
    const Rule &rule = c.rules[static_cast<std::size_t>(video)];
    EXPECT_EQ(rule.brightness_cap, 15);
    ASSERT_TRUE(rule.when.fullscreen_app.has_value());
    EXPECT_EQ(*rule.when.fullscreen_app,
              (std::vector<std::string>{"mpv", "vlc", "firefox", "chromium"}));
    EXPECT_GT(video, index("gamemode"));
    ASSERT_LT(index("power saver dims"), count);
    EXPECT_LT(video, index("power saver dims")) << "rules are first-match per category";
}

TEST(Config, ZoneMapFromToml) {
    const auto c = parse_config("[device]\nzone_map = { left = 0, middle = 1, right = 2, wasd = 3 }"
                                "\n[[profiles]]\nname='a'\ncolors=['#000000']");
    ASSERT_TRUE(c.has_value()) << c.error();
    constexpr Rgb R{255, 0, 0}, G{0, 255, 0}, B{0, 0, 255};
    EXPECT_EQ(c->device.zone_map.to_hw({R, G, B, kWhite}), (std::array<Rgb, 4>{R, G, B, kWhite}));
    EXPECT_FALSE(parse_config("[device]\nzone_map = { left = 0, middle = 0, right = 2, wasd = 3 }"
                              "\n[[profiles]]\nname='a'\ncolors=['#000000']")
                     .has_value());
}

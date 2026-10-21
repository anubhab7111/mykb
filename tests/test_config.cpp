#include "config.hpp"

#include <cstdlib>
#include <gtest/gtest.h>

using namespace omenkbd;

TEST(Config, ExampleConfigParses) {
    const Config c = embedded_default();
    EXPECT_EQ(c.profiles.size(), 3u);
    EXPECT_FALSE(c.effects.enabled);
    EXPECT_TRUE(c.hints.modifiers.contains("super+shift"));
    EXPECT_EQ(c.hints.timeout_s, 15u);
    EXPECT_EQ(c.gates.on_idle, GateAction::Dim);
    EXPECT_EQ(c.rules.size(), 3u);
}

TEST(Config, MissingFileGivesDefault) {
    const auto c = load_config("/nonexistent/x.toml");
    ASSERT_TRUE(c.has_value()) << c.error();
    EXPECT_EQ(c->profiles[0].name, "default");
}

TEST(Config, NormalizesModifierKeys) {
    EXPECT_EQ(normalize_mods("Shift+SUPER").value(), "super+shift");
    EXPECT_EQ(normalize_mods("alt+ctrl+alt").value(), "ctrl+alt");
    EXPECT_FALSE(normalize_mods("hyper").has_value());
    EXPECT_FALSE(normalize_mods("").has_value());
}

TEST(Config, ModifierKeysInConfigAreNormalized) {
    const auto c = parse_config("[hints.modifiers]\n\"Shift+Super\" = '#0000ff'\n[[profiles]]\n"
                                "name='a'\ncolors=['#000000']");
    ASSERT_TRUE(c.has_value()) << c.error();
    EXPECT_TRUE(c->hints.modifiers.contains("super+shift"));
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
        {"[hints.modifiers]\nhyper='#000000'\n[[profiles]]\nname='a'\ncolors=['#000000']", "hyper"},
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

TEST(Config, ThemeDirExpandsHome) {
    const char *home = std::getenv("HOME");
    ASSERT_NE(home, nullptr);
    const auto c =
        parse_config("[theme]\ndir = '~/x/y'\n[[profiles]]\nname='a'\ncolors=['#000000']");
    ASSERT_TRUE(c.has_value()) << c.error();
    EXPECT_EQ(c->theme.dir, std::filesystem::path(home) / "x/y");
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

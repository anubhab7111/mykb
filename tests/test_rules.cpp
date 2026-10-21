#include "core/rules.hpp"

#include <gtest/gtest.h>
#include <toml++/toml.hpp>

using namespace omenkbd;

namespace {

std::expected<std::vector<Rule>, std::string> try_rules(std::string_view src) {
    toml::table root;
    try {
        root = toml::parse(src);
    } catch (const toml::parse_error &e) {
        return std::unexpected(std::string(e.description()));
    }
    std::vector<Rule> out;
    const toml::array *arr = root["rules"].as_array();
    if (arr == nullptr) {
        return out;
    }
    for (const toml::node &n : *arr) {
        auto r = parse_rule(*n.as_table());
        if (!r) {
            return std::unexpected(r.error());
        }
        out.push_back(std::move(*r));
    }
    return out;
}

std::vector<Rule> rules(std::string_view src) {
    auto r = try_rules(src);
    EXPECT_TRUE(r.has_value()) << r.error();
    return r.value_or(std::vector<Rule>{});
}

} // namespace

TEST(Rules, FirstMatchPerCategory) {
    const auto r = rules("[[rules]]\nname = 'a'\nwhen = {}\nprofile = 'game'\n"
                         "[[rules]]\nname = 'b'\nwhen = {}\nprofile = 'white'\n"
                         "[[rules]]\nname = 'c'\nwhen = {}\nbrightness_cap = 40\n");
    const auto out = evaluate(r, Facts{});
    EXPECT_EQ(out.profile, "game");
    EXPECT_EQ(out.brightness_cap, 40);
}

TEST(Rules, AllConditionsMustHold) {
    const auto r = rules("[[rules]]\nname = 'low'\nwhen = { on_battery = true, battery_below = 15 }"
                         "\nprofile = 'low'\n");
    Facts f;
    f.on_battery = true;
    f.battery_pct = 20.0;
    EXPECT_EQ(evaluate(r, f).profile, std::nullopt);
    f.battery_pct = 10.0;
    EXPECT_EQ(evaluate(r, f).profile, "low");
    f.on_battery = false;
    EXPECT_EQ(evaluate(r, f).profile, std::nullopt);
}

TEST(Rules, UnknownFactIsFalse) {
    const auto r = rules("[[rules]]\nname = 'ps'\nwhen = { power_profile = 'power-saver' }\n"
                         "brightness_cap = 40\n");
    EXPECT_EQ(evaluate(r, Facts{}).brightness_cap, std::nullopt);
    Facts f;
    f.power_profile = "power-saver";
    EXPECT_EQ(evaluate(r, f).brightness_cap, 40);
}

TEST(Rules, EmptyConditionAlwaysMatches) {
    const auto r = rules("[[rules]]\nname = 'always'\nwhen = {}\nbrightness_cap = 10\n");
    EXPECT_EQ(evaluate(r, Facts{}).brightness_cap, 10);
}

TEST(Rules, BoolAndThresholdConditions) {
    const auto r = rules("[[rules]]\nname = 'hot'\nwhen = { cpu_temp_above = 90 }\n"
                         "tint = { color = '#ff4000', strength = 0.5 }\n"
                         "[[rules]]\nname = 'gm'\nwhen = { gamemode = true, locked = false, idle = "
                         "false }\nprofile = 'game'\n");
    Facts f;
    f.cpu_temp_c = 95.0;
    f.gamemode = true;
    auto out = evaluate(r, f);
    ASSERT_TRUE(out.overlay.has_value());
    EXPECT_EQ(*out.overlay, (Overlay{TintOverlay{ColorRef::hex({255, 0x40, 0}), 0.5}}));
    EXPECT_EQ(out.profile, "game");

    Facts cool_locked;
    cool_locked.cpu_temp_c = 90.0;
    cool_locked.gamemode = true;
    cool_locked.locked = true;
    out = evaluate(r, cool_locked);
    EXPECT_FALSE(out.overlay.has_value()) << "90 is not above 90";
    EXPECT_EQ(out.profile, std::nullopt) << "locked = false does not hold";
}

TEST(Rules, ActiveListsContributors) {
    const auto r = rules(
        "[[rules]]\nname = 'r1'\nwhen = {}\nprofile = 'game'\n"
        "[[rules]]\nname = 'r2'\nwhen = {}\nprofile = 'white'\n"
        "[[rules]]\nname = 'r3'\nwhen = {}\noverlay = { zones = ['right'], color = '#ff0000', "
        "pulse = true }\n");
    const auto out = evaluate(r, Facts{});
    EXPECT_EQ(out.active, (std::vector<std::string>{"r1", "r3"}));
    ASSERT_TRUE(out.overlay.has_value());
    EXPECT_EQ(*out.overlay,
              (Overlay{ZonesOverlay{{Zone::Right}, ColorRef::hex({255, 0, 0}), true}}));
}

TEST(Rules, OverlayAndTintBothIsInvalid) {
    const auto both = rules("[[rules]]\nname = 'x'\nwhen = {}\noverlay = { zones = ['left'], color "
                            "= '#ff0000' }\ntint = { color = '#00ff00', strength = 0.5 }\n");
    EXPECT_FALSE(both[0].validate().has_value());
    const auto none = rules("[[rules]]\nname = 'x'\nwhen = {}\n");
    EXPECT_FALSE(none[0].validate().has_value());
    const auto strong =
        rules("[[rules]]\nname = 'x'\nwhen = {}\ntint = { color = '#00ff00', strength = 1.5 }\n");
    EXPECT_FALSE(strong[0].validate().has_value());
    const auto ok = rules("[[rules]]\nname = 'x'\nwhen = {}\nbrightness_cap = 30\n");
    EXPECT_TRUE(ok[0].validate().has_value());
}

TEST(Rules, UnknownConditionFieldIsRejected) {
    const auto r = try_rules("[[rules]]\nname = 'x'\nwhen = { foo = 1 }\nprofile = 'a'\n");
    ASSERT_FALSE(r.has_value());
    EXPECT_NE(r.error().find("foo"), std::string::npos) << r.error();
}

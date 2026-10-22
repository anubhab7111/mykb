#include "helpers.hpp"

#include "state.hpp"

#include <gtest/gtest.h>

using namespace omenkbd;
using namespace omenkbd::test;

TEST(State, MissingFileGivesDefault) {
    TempDir d;
    const auto [s, warning] = PersistedState::load(d.path() / "state.json", "default");
    EXPECT_EQ(warning, std::nullopt);
    PersistedState expected;
    expected.active_profile = "default";
    EXPECT_EQ(s, expected);
    EXPECT_EQ(s.brightness, 100);
    EXPECT_TRUE(s.on);
    EXPECT_EQ(s.lifetime_writes, 0u);
}

TEST(State, CorruptFileIsMovedAside) {
    TempDir d;
    const auto p = d.path() / "state.json";
    write_file(p, "{not json");
    const auto [s, warning] = PersistedState::load(p, "default");
    EXPECT_EQ(s, PersistedState::default_for("default"));
    EXPECT_TRUE(warning.has_value());
    EXPECT_EQ(read_file(d.path() / "state.json.bad"), "{not json");
    EXPECT_FALSE(std::filesystem::exists(p));
}

TEST(State, SaveRoundtripsAndLeavesNoTmp) {
    TempDir d;
    const auto p = d.path() / "a/b/state.json";
    PersistedState s;
    s.active_profile = "custom";
    s.custom = std::vector<std::string>{"#ff0000", "theme:blue"};
    s.effect_override = EffectOverride{Effect::Breathe, 30};
    s.brightness = 40;
    s.on = false;
    s.lifetime_writes = 12345;
    ASSERT_TRUE(s.save(p).has_value());
    const auto [loaded, warning] = PersistedState::load(p, "default");
    EXPECT_EQ(warning, std::nullopt);
    EXPECT_EQ(loaded, s);
    EXPECT_FALSE(std::filesystem::exists(d.path() / "a/b/state.json.tmp"));
    EXPECT_NE(read_file(p).find("\"effect\": \"breathe\""), std::string::npos)
        << "same JSON shape as the Rust prototype";
}

// save() runs in a daemon timer and must never throw, whatever the strings hold (today they come
// only through parsers that reject invalid UTF-8; see Ipc.DecodingRejectsIllFormedUtf8).
TEST(State, SaveReplacesInvalidUtf8) {
    TempDir d;
    const auto p = d.path() / "state.json";
    PersistedState s = PersistedState::default_for("caf\xe9");
    ASSERT_TRUE(s.save(p).has_value());
    const auto [loaded, warning] = PersistedState::load(p, "default");
    EXPECT_EQ(warning, std::nullopt);
    EXPECT_EQ(loaded.active_profile, "caf\xef\xbf\xbd");
}

TEST(State, UnknownAndMissingFieldsAreTolerated) {
    TempDir d;
    const auto p = d.path() / "state.json";
    write_file(p, R"({"active_profile":"game","future_field":1})");
    const auto [s, warning] = PersistedState::load(p, "default");
    EXPECT_EQ(warning, std::nullopt);
    EXPECT_EQ(s.active_profile, "game");
    EXPECT_EQ(s.brightness, 100);
    EXPECT_TRUE(s.on);
}

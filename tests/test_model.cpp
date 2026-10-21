#include "daemon/model.hpp"

#include <algorithm>
#include <gtest/gtest.h>

using namespace omenkbd;
using namespace std::chrono_literals;

namespace {

constexpr Rgb R{255, 0, 0};
constexpr Rgb G{0, 255, 0};
constexpr Rgb B{0, 0, 255};
constexpr Rgb W = kWhite;

Palette palette() {
    return Palette::from_waybar_css(
        "@define-color mauve #cba6f7;\n@define-color blue #89b4fa;\n@define-color teal "
        "#94e2d5;\n@define-color peach #fab387;\n@define-color red #f38ba8;\n");
}

Model model() {
    return Model(embedded_default(), palette(), PersistedState::default_for("default"), "mock");
}

/// Explicit hex colors so expectations are literals.
constexpr std::string_view kHexCfg = R"(
[effects]
enabled = true
[hints]
timeout_s = 15
[hints.modifiers]
super = "#0000ff"
[hints.submaps]
resize = "#00ff00"
[[profiles]]
name = "rgbw"
colors = ["#ff0000", "#00ff00", "#0000ff", "#ffffff"]
effect = "wave"
[[profiles]]
name = "game"
colors = ["#202020", "#202020", "#202020", "#ff0000"]
[[rules]]
name = "gamemode"
when = { gamemode = true }
profile = "game"
[[rules]]
name = "power saver dims"
when = { power_profile = "power-saver" }
brightness_cap = 40
)";

Model hex_model() {
    return Model(parse_config(kHexCfg).value(), palette(), PersistedState::default_for("rgbw"),
                 "mock");
}

std::expected<Change, std::string> req_(Model &m, Request r) { return m.handle(r, Clock::now()); }

std::expected<Change, std::string> set(Model &m, std::vector<std::string> colors) {
    return req_(m, req::Set{std::move(colors)});
}

Inputs in(Model &m) { return m.inputs(Clock::now()).value(); }

bool contains(const std::string &s, std::string_view needle) {
    return s.find(needle) != std::string::npos;
}

} // namespace

TEST(Model, SetCustomColorsPersistsAndOutputs) {
    Model m = model();
    EXPECT_EQ(set(m, {"#ff0000", "#00ff00", "#0000ff", "#ffffff"}), Change::OutputAndPersist);
    EXPECT_EQ(m.persisted().active_profile, "custom");
    EXPECT_TRUE(m.persisted().on);
    EXPECT_EQ(in(m).base, (Frame{R, G, B, W}));
    EXPECT_FALSE(set(m, {"#ff0000", "#00ff00"}).has_value());
    EXPECT_FALSE(set(m, {"red"}).has_value());
    EXPECT_EQ(set(m, {"#00ff00"}), Change::OutputAndPersist);
    EXPECT_EQ(in(m).base, (Frame{G, G, G, G}));
}

TEST(Model, ProfileNextCyclesConfigOrderSkippingCustom) {
    Model m = model();
    const auto next = [](Model &mm) {
        EXPECT_TRUE(req_(mm, req::Profile{"next"}).has_value());
        return mm.persisted().active_profile;
    };
    EXPECT_EQ(next(m), "game");
    EXPECT_EQ(next(m), "white");
    EXPECT_EQ(next(m), "default");
    ASSERT_TRUE(req_(m, req::Profile{"prev"}).has_value());
    EXPECT_EQ(m.persisted().active_profile, "white");
    ASSERT_TRUE(set(m, {"#123456"}).has_value());
    EXPECT_EQ(next(m), "default") << "from custom, next is the first";
    EXPECT_TRUE(req_(m, req::Profile{"custom"}).has_value());
    const auto e = req_(m, req::Profile{"nope"});
    ASSERT_FALSE(e.has_value());
    EXPECT_TRUE(contains(e.error(), "unknown profile: nope")) << e.error();
}

TEST(Model, BrightnessRelativeClamps) {
    Model m = model();
    const auto b = [](Model &mm, std::string v) -> std::optional<int> {
        if (!req_(mm, req::Brightness{std::move(v)})) {
            return std::nullopt;
        }
        return mm.persisted().brightness;
    };
    EXPECT_EQ(b(m, "90"), 90);
    EXPECT_EQ(b(m, "+30"), 100);
    EXPECT_EQ(b(m, "-150"), 0);
    EXPECT_EQ(b(m, "101"), std::nullopt);
    EXPECT_EQ(b(m, "abc"), std::nullopt);
}

TEST(Model, LidAndLockGatesTurnBacklightOff) {
    Model m = model();
    EXPECT_TRUE(in(m).backlight);
    m.apply_fact(fact::LidClosed{true});
    EXPECT_FALSE(in(m).backlight);
    m.apply_fact(fact::LidClosed{false});
    m.apply_fact(fact::Locked{true});
    EXPECT_FALSE(in(m).backlight) << "on_lock = off";
    m.apply_fact(fact::Locked{false});
    ASSERT_TRUE(req_(m, req::Power{req::PowerAction::Off}).has_value());
    EXPECT_FALSE(in(m).backlight);
    ASSERT_TRUE(req_(m, req::Power{req::PowerAction::Toggle}).has_value());
    EXPECT_TRUE(in(m).backlight);
}

TEST(Model, IdleDimLowersBrightness) {
    Model m = model();
    ASSERT_TRUE(req_(m, req::Idle{true}).has_value());
    const Inputs i = in(m);
    EXPECT_EQ(i.brightness, 20);
    EXPECT_TRUE(i.backlight);
    EXPECT_EQ(m.persisted().brightness, 100) << "user brightness unchanged";
}

TEST(Model, HintExpiresAfterTimeout) {
    Model m = hex_model();
    const TimePoint t0 = Clock::now();
    ASSERT_TRUE(m.handle(req::Hint{"SUPER"}, t0).has_value());
    EXPECT_EQ(m.inputs(t0)->hint, B);
    EXPECT_EQ(m.hint_deadline(), t0 + 15s);
    EXPECT_EQ(m.inputs(t0 + 16s)->hint, std::nullopt);
}

TEST(Model, LockClearsHint) {
    Model m = hex_model();
    ASSERT_TRUE(req_(m, req::Hint{"super"}).has_value());
    m.apply_fact(fact::Locked{true});
    EXPECT_EQ(in(m).hint, std::nullopt);
}

TEST(Model, ModifierHintBeatsSubmap) {
    Model m = hex_model();
    m.set_submap("resize");
    EXPECT_EQ(in(m).hint, G);
    ASSERT_TRUE(req_(m, req::Hint{"super"}).has_value());
    EXPECT_EQ(in(m).hint, B);
    ASSERT_TRUE(req_(m, req::Hint{"clear"}).has_value());
    EXPECT_EQ(in(m).hint, G);
    m.set_submap(std::nullopt);
    EXPECT_EQ(in(m).hint, std::nullopt);
}

TEST(Model, UnconfiguredHintIsAnError) {
    Model m = hex_model();
    const auto e = req_(m, req::Hint{"ctrl+alt"});
    ASSERT_FALSE(e.has_value());
    EXPECT_TRUE(contains(e.error(), "no color configured for ctrl+alt")) << e.error();
}

TEST(Model, EffectRequestErrorsWhenDisabled) {
    Model m = model();
    const auto e = req_(m, req::Effect{"wave", std::nullopt});
    ASSERT_FALSE(e.has_value());
    EXPECT_TRUE(contains(e.error(), "enabled = true")) << e.error();
    Model h = hex_model();
    EXPECT_EQ(req_(h, req::Effect{"breathe", std::uint8_t{80}}), Change::OutputAndPersist);
    const Inputs i = in(h);
    EXPECT_EQ(i.effect, Effect::Breathe);
    EXPECT_EQ(i.speed, 80);
}

TEST(Model, BatteryPausesEffects) {
    Model m = hex_model();
    EXPECT_TRUE(in(m).animate);
    m.apply_fact(fact::OnBattery{true});
    EXPECT_FALSE(in(m).animate);
    EXPECT_FALSE(m.wants_animation());
}

TEST(Model, FnKeyOffPausesButKeepsProbing) {
    Model m = hex_model();
    m.set_hw_backlight(false);
    EXPECT_FALSE(in(m).animate);
    EXPECT_TRUE(m.wants_animation()) << "keep probing so the Fn key can resume it";
}

TEST(Model, BudgetErrorDisablesAnimation) {
    Model m = hex_model();
    m.record_write(std::unexpected(BackendError{BackendError::Kind::Budget, ""}));
    EXPECT_FALSE(in(m).animate);
    const Status s = m.status(nullptr);
    EXPECT_TRUE(s.budget_exhausted);
    ASSERT_TRUE(s.last_error.has_value());
    EXPECT_TRUE(contains(*s.last_error, "budget"));
    m.record_write(true);
    EXPECT_TRUE(in(m).animate);
    EXPECT_EQ(m.persisted().lifetime_writes, 1u);
    EXPECT_EQ(m.status(nullptr).last_error, std::nullopt);
}

TEST(Model, RuleProfileOverridesUserSelection) {
    Model m = hex_model();
    ASSERT_TRUE(req_(m, req::Flag{"gamemode", true}).has_value());
    constexpr Rgb g{0x20, 0x20, 0x20};
    EXPECT_EQ(in(m).base, (Frame{g, g, g, R}));
    const Status s = m.status(nullptr);
    EXPECT_EQ(s.profile, "game");
    EXPECT_EQ(s.active_rules, std::vector<std::string>{"gamemode"});
    EXPECT_EQ(m.persisted().active_profile, "rgbw") << "user selection is kept";
}

TEST(Model, UnknownFlagIsAnError) {
    Model m = model();
    const auto e = req_(m, req::Flag{"turbo", true});
    ASSERT_FALSE(e.has_value());
    EXPECT_TRUE(contains(e.error(), "unknown flag: turbo")) << e.error();
}

TEST(Model, BrightnessCapFromRule) {
    Model m = hex_model();
    m.apply_fact(fact::PowerProfile{"power-saver"});
    EXPECT_EQ(in(m).brightness, 40);
}

TEST(Model, UnknownThemeColorWarnsAndUsesWhite) {
    Config cfg = parse_config("[[profiles]]\nname = 'x'\ncolors = ['theme:nope', '#000000', "
                              "'#000000', '#000000']\n")
                     .value();
    Model m(std::move(cfg), palette(), PersistedState::default_for("x"), "mock");
    EXPECT_EQ(in(m).base[0], W);
    const auto warnings = m.status(nullptr).warnings;
    EXPECT_TRUE(std::ranges::any_of(
        warnings, [](const std::string &w) { return contains(w, "unknown theme color: nope"); }));
}

TEST(Model, SuspendedYieldsNoInputs) {
    Model m = hex_model();
    ASSERT_TRUE(req_(m, req::Hint{"super"}).has_value());
    m.set_suspended(true);
    EXPECT_FALSE(m.inputs(Clock::now()).has_value());
    m.set_suspended(false);
    EXPECT_EQ(in(m).hint, std::nullopt) << "suspend cleared the hint";
}

TEST(Model, StatusReportsOutputZones) {
    Model m = hex_model();
    ASSERT_TRUE(set(m, {"#ff0000", "#00ff00", "#0000ff", "#ffffff"}).has_value());
    const Output out = compose(in(m), 0.0);
    const Status s = m.status(&out);
    EXPECT_EQ(s.zones, (std::array<std::string, 4>{"FF0000", "00FF00", "0000FF", "FFFFFF"}));
    EXPECT_EQ(s.profile, "custom");
    EXPECT_EQ(s.backend, "mock");
    EXPECT_TRUE(s.backlight);
}

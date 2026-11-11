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

constexpr Rgb kSweep{0x0a, 0x14, 0x1e};
constexpr Rgb kUrgent{0xe6, 0x28, 0x28};
constexpr Rgb kPeach{0xfa, 0xb3, 0x87}; // theme:peach in palette()

/// Reactions on, over a static profile. The lamp's color comes from the theme, as is.
constexpr std::string_view kReactCfg = R"(
[theme]
colors = "exact"
[reactions]
[reactions.workspace]
color = "#0a141e"
[reactions.urgent]
color = "#e62828"
[reactions.agent]
waiting = "theme:peach"
ttl_s = 3600
[[profiles]]
name = "plain"
colors = ["#101010", "#202020", "#303030", "#404040"]
)";

Model react_model(std::string_view cfg = kReactCfg) {
    return Model(parse_config(cfg).value(), palette(), PersistedState::default_for("plain"),
                 "mock");
}

/// A layer that paints `color` on `zones` only.
LayerOps lit(Rgb color, std::initializer_list<Zone> zones) {
    LayerOps ops;
    for (Zone z : zones) {
        ops[index(z)] = Paint{color};
    }
    return ops;
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
    m.set_hw_backlight(false, Clock::now());
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

// hyprlock never sets logind's LockedHint and unlocking it sends no Unlock signal, so the lock
// state comes from hypridle's on_lock_cmd/on_unlock_cmd: `omen-kbd flag locked on|off`.
TEST(Model, LockedFlagGatesBacklightAndClearsHint) {
    Model m = hex_model();
    ASSERT_TRUE(req_(m, req::Hint{"super"}).has_value());
    EXPECT_EQ(req_(m, req::Flag{"locked", true}), Change::Output);
    Inputs i = in(m);
    EXPECT_FALSE(i.backlight) << "on_lock = off";
    EXPECT_EQ(i.hint, std::nullopt) << "locking clears the hint";
    EXPECT_TRUE(m.status(nullptr).facts.locked);
    EXPECT_EQ(req_(m, req::Flag{"locked", false}), Change::Output);
    EXPECT_TRUE(in(m).backlight);
}

TEST(Model, BrightnessCapFromRule) {
    Model m = hex_model();
    m.apply_fact(fact::PowerProfile{"power-saver"});
    EXPECT_EQ(in(m).brightness, 40);
}

TEST(Model, FullscreenFactSelectsTheCap) {
    Model m = model();
    EXPECT_EQ(in(m).brightness, 100);
    EXPECT_EQ(m.status(nullptr).facts.fullscreen_app, std::nullopt);

    m.apply_fact(fact::FullscreenApp{"mpv"});
    EXPECT_EQ(in(m).brightness, 15);
    EXPECT_EQ(m.status(nullptr).facts.fullscreen_app, "mpv");
    EXPECT_EQ(m.status(nullptr).active_rules, std::vector<std::string>{"fullscreen video dims"});

    m.apply_fact(fact::FullscreenApp{"kitty"});
    EXPECT_EQ(in(m).brightness, 100) << "kitty is not a listed video app";

    m.apply_fact(fact::FullscreenApp{"mpv"});
    m.apply_fact(fact::PowerProfile{"power-saver"});
    EXPECT_EQ(in(m).brightness, 15) << "the fullscreen rule is listed before power saver dims";

    m.apply_fact(fact::FullscreenApp{std::nullopt});
    EXPECT_EQ(in(m).brightness, 40) << "back to the power saver cap";
    EXPECT_EQ(m.status(nullptr).facts.fullscreen_app, std::nullopt);
    m.apply_fact(fact::PowerProfile{"balanced"});
    EXPECT_EQ(in(m).brightness, 100);
}

// The literal sequence mpv, then no fullscreen window, then full brightness again, with no other
// cap in play (the test above ends with power saver and a battery value mixed in).
TEST(Model, FullscreenFactReleasesTheCapWhenNothingElseCaps) {
    Model m = model();
    EXPECT_EQ(in(m).brightness, 100);
    m.apply_fact(fact::FullscreenApp{"mpv"});
    EXPECT_EQ(in(m).brightness, 15);
    m.apply_fact(fact::FullscreenApp{std::nullopt});
    EXPECT_EQ(in(m).brightness, 100) << "no other cap applies, so back to full";
    EXPECT_EQ(m.status(nullptr).facts.fullscreen_app, std::nullopt);
    EXPECT_TRUE(m.status(nullptr).active_rules.empty());
}

// `Facts::fullscreen_app` is documented as lowercase and the rules compare it with the lowercased
// classes of a rule, so the model enforces that at the one place a fact enters.
TEST(Model, FullscreenFactIsLowercasedWhereItEntersTheModel) {
    Model m = model();
    m.apply_fact(fact::FullscreenApp{"MPV"});
    EXPECT_EQ(in(m).brightness, 15) << "the rule lists mpv";
    EXPECT_EQ(m.status(nullptr).facts.fullscreen_app, "mpv");
    EXPECT_EQ(m.status(nullptr).active_rules, std::vector<std::string>{"fullscreen video dims"});

    m.apply_fact(fact::FullscreenApp{"FireFox"});
    EXPECT_EQ(in(m).brightness, 15) << "firefox is listed too";
    EXPECT_EQ(m.status(nullptr).facts.fullscreen_app, "firefox");

    m.apply_fact(fact::FullscreenApp{"Kitty"});
    EXPECT_EQ(in(m).brightness, 100) << "a mixed-case class that is not listed";
    EXPECT_EQ(m.status(nullptr).facts.fullscreen_app, "kitty");
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

// A hint can still arrive while suspended (a key event racing PrepareForSleep) and must expire all
// the same: the daemon wakes at hint_deadline(), and a deadline in the past wakes it in a loop.
TEST(Model, HintExpiresWhileSuspended) {
    Model m = hex_model();
    const TimePoint t0 = Clock::now();
    m.set_suspended(true);
    ASSERT_TRUE(m.handle(req::Hint{"super"}, t0).has_value());
    EXPECT_FALSE(m.inputs(t0 + 16s).has_value());
    EXPECT_EQ(m.hint_deadline(), std::nullopt);
}

// Spec: "idle/lock/Fn-off/suspend → stop ticking", whatever the gate does to the backlight
// (here on_idle = dim, on_lock = off).
TEST(Model, IdleAndLockStopEffectsUntilTheyEnd) {
    Model m = hex_model();
    ASSERT_TRUE(in(m).animate);
    ASSERT_TRUE(req_(m, req::Idle{true}).has_value());
    EXPECT_FALSE(in(m).animate) << "idle";
    EXPECT_FALSE(m.wants_animation()) << "idle";
    ASSERT_TRUE(req_(m, req::Idle{false}).has_value());
    EXPECT_TRUE(in(m).animate) << "after idle";
    EXPECT_TRUE(m.wants_animation()) << "after idle";
    m.apply_fact(fact::Locked{true});
    EXPECT_FALSE(in(m).animate) << "locked";
    EXPECT_FALSE(m.wants_animation()) << "locked";
    m.apply_fact(fact::Locked{false});
    EXPECT_TRUE(in(m).animate) << "after unlock";
    EXPECT_TRUE(m.wants_animation()) << "after unlock";
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

TEST(Model, InputsCarryTheReactionLayersAndTheDeadline) {
    Model m = react_model();
    const TimePoint t0 = Clock::now();
    ASSERT_TRUE(m.inputs(t0).has_value());
    EXPECT_EQ(m.reaction_deadline(), std::nullopt) << "nothing is lit or playing";
    m.apply_signal(signal::WorkspaceSwitched{1, 2}, t0);
    m.apply_signal(signal::UrgentRaised{"a1"}, t0);
    EXPECT_EQ(m.reaction_deadline(), t0 + 120ms);

    const Inputs at0 = m.inputs(t0).value();
    EXPECT_EQ(at0.transient, lit(kSweep, {Zone::Left, Zone::Wasd}));
    EXPECT_EQ(at0.lamps, lit(kUrgent, {Zone::Right}));

    const Inputs later = m.inputs(t0 + 150ms).value();
    EXPECT_EQ(later.transient, lit(kSweep, {Zone::Middle}));
    EXPECT_TRUE(empty(later.lamps)) << "the intro's dark phase";
    EXPECT_EQ(m.reaction_deadline(), t0 + 240ms);
}

TEST(Model, ReactionsOffChangeNothing) {
    Model m = hex_model(); // no [reactions] table
    const TimePoint t0 = Clock::now();
    const Inputs before = m.inputs(t0).value();
    m.apply_signal(signal::WorkspaceSwitched{1, 2}, t0);
    m.apply_signal(signal::UrgentRaised{"a1"}, t0);
    m.apply_signal(signal::AgentWaiting{"s1", std::nullopt}, t0);
    EXPECT_EQ(m.inputs(t0).value(), before);
    EXPECT_EQ(m.reaction_deadline(), std::nullopt);
    EXPECT_EQ(m.status(nullptr).reactions, ReactionsStatus{});
}

TEST(Model, SuspendClearsClipsButKeepsLamps) {
    Model m = react_model();
    const TimePoint t0 = Clock::now();
    m.apply_signal(signal::AgentWaiting{"s1", std::nullopt}, t0);
    ASSERT_TRUE(m.inputs(t0 + 1s).has_value()); // the intro is over
    m.apply_signal(signal::WorkspaceSwitched{1, 2}, t0 + 1s);
    ASSERT_EQ(m.status(nullptr).reactions.playing, "workspace");
    ASSERT_EQ(m.reaction_deadline(), t0 + 1s + 120ms);

    m.set_suspended(true);
    EXPECT_EQ(m.reaction_deadline(), std::nullopt) << "nothing to wake for while suspended";
    EXPECT_FALSE(m.inputs(t0 + 1s + 10ms).has_value());
    EXPECT_EQ(m.status(nullptr).reactions, (ReactionsStatus{true, {"agent"}, std::nullopt}));

    m.set_suspended(false);
    const Inputs in = m.inputs(t0 + 1s + 20ms).value();
    EXPECT_TRUE(empty(in.transient)) << "the sweep is gone, not resumed";
    EXPECT_EQ(in.lamps, lit(kPeach, {Zone::Wasd})) << "the lamp holds";
    EXPECT_EQ(m.reaction_deadline(), t0 + 3600s) << "only the TTL is left";
}

TEST(Model, PaletteChangeRestylesLamps) {
    Model m = react_model();
    const TimePoint t0 = Clock::now();
    m.apply_signal(signal::AgentWaiting{"s1", std::nullopt}, t0);
    EXPECT_EQ(m.inputs(t0 + 1s)->lamps, lit(kPeach, {Zone::Wasd}));

    m.set_palette(Palette::from_waybar_css("@define-color peach #102030;\n"));
    EXPECT_EQ(m.inputs(t0 + 2s)->lamps, lit(Rgb{0x10, 0x20, 0x30}, {Zone::Wasd}));
    EXPECT_EQ(m.status(nullptr).reactions.lamps, std::vector<std::string>{"agent"});
}

TEST(Model, HwBacklightReadingIsFreshFor5s) {
    Model m = react_model();
    const TimePoint t0 = Clock::now();
    EXPECT_FALSE(m.backlight_reading_fresh(t0, 5s)) << "never read";
    m.set_hw_backlight(true, t0);
    EXPECT_TRUE(m.backlight_reading_fresh(t0, 5s));
    EXPECT_TRUE(m.backlight_reading_fresh(t0 + 5s, 5s));
    EXPECT_FALSE(m.backlight_reading_fresh(t0 + 5s + 1ms, 5s));

    m.set_hw_backlight(false, t0 + 10s);
    EXPECT_TRUE(m.backlight_reading_fresh(t0 + 12s, 5s)) << "a reading of off is a reading too";
    m.forget_hw_backlight();
    EXPECT_FALSE(m.backlight_reading_fresh(t0 + 12s, 5s))
        << "forgetting the reading forgets its time";
}

TEST(Model, StatusReportsReactions) {
    Model m = react_model();
    const TimePoint t0 = Clock::now();
    EXPECT_EQ(m.status(nullptr).reactions, (ReactionsStatus{true, {}, std::nullopt}));
    m.apply_signal(signal::UrgentRaised{"a1"}, t0);
    m.apply_signal(signal::AgentWaiting{"s1", std::nullopt}, t0);
    m.apply_signal(signal::WorkspaceSwitched{1, 2}, t0);
    EXPECT_EQ(m.status(nullptr).reactions,
              (ReactionsStatus{true, {"agent", "urgent"}, "workspace"}));
    ASSERT_TRUE(m.inputs(t0 + 1s).has_value());
    EXPECT_EQ(m.status(nullptr).reactions,
              (ReactionsStatus{true, {"agent", "urgent"}, std::nullopt}));

    EXPECT_EQ(hex_model().status(nullptr).reactions, ReactionsStatus{}) << "no [reactions] table";
}

// The keyboard counts as visible only while the backlight is on, the frame is not dark, the
// firmware did not report the backlight off, and the write budget is not spent.
TEST(Model, ReactionsNeedAVisibleKeyboard) {
    const TimePoint t0 = Clock::now();
    Model fn = react_model();
    fn.set_hw_backlight(false, t0); // the Fn key
    fn.apply_signal(signal::WorkspaceSwitched{1, 2}, t0);
    fn.apply_signal(signal::UrgentRaised{"a1"}, t0);
    Inputs in = fn.inputs(t0).value();
    EXPECT_TRUE(empty(in.transient));
    EXPECT_TRUE(empty(in.lamps));
    fn.set_hw_backlight(true, t0 + 1s);
    EXPECT_EQ(fn.inputs(t0 + 1s)->lamps, lit(kUrgent, {Zone::Right})) << "steady, no intro";
    EXPECT_EQ(fn.reaction_deadline(), std::nullopt);

    Model budget = react_model();
    budget.record_write(std::unexpected(BackendError{BackendError::Kind::Budget, ""}));
    budget.apply_signal(signal::WorkspaceSwitched{1, 2}, t0);
    budget.apply_signal(signal::UrgentRaised{"a1"}, t0);
    in = budget.inputs(t0).value();
    EXPECT_TRUE(empty(in.transient));
    EXPECT_TRUE(empty(in.lamps));

    Model dark = react_model();
    ASSERT_TRUE(dark.handle(req::Brightness{"0"}, t0).has_value());
    dark.apply_signal(signal::WorkspaceSwitched{1, 2}, t0);
    EXPECT_TRUE(empty(dark.inputs(t0)->transient));

    // A dimmed keyboard is still visible: a lamp shows while the user is idle, a clip does not.
    Model idle = react_model();
    ASSERT_TRUE(idle.handle(req::Idle{true}, t0).has_value()); // on_idle = dim
    idle.apply_signal(signal::WorkspaceSwitched{1, 2}, t0);
    idle.apply_signal(signal::UrgentRaised{"a1"}, t0);
    in = idle.inputs(t0).value();
    EXPECT_TRUE(empty(in.transient));
    EXPECT_EQ(in.lamps, lit(kUrgent, {Zone::Right}));
}

// Ruling P14 (b): a reload can shorten the TTL or the blinks, so the deadline is refreshed with
// it, before any new frame.
TEST(Model, ReloadRefreshesTheReactionDeadline) {
    Model m = react_model();
    const TimePoint t0 = Clock::now();
    m.apply_signal(signal::AgentWaiting{"s1", std::nullopt}, t0);
    ASSERT_TRUE(m.inputs(t0 + 1s).has_value());
    ASSERT_EQ(m.reaction_deadline(), t0 + 3600s);

    std::string shorter(kReactCfg);
    shorter.replace(shorter.find("ttl_s = 3600"), 12, "ttl_s = 60");
    m.replace_config(parse_config(shorter).value(), palette());
    EXPECT_EQ(m.reaction_deadline(), t0 + 60s);
    EXPECT_EQ(m.status(nullptr).reactions.lamps, std::vector<std::string>{"agent"});
}

TEST(Model, ReactionColorWarningsShowOnlyWhileEnabled) {
    std::string cfg(kReactCfg);
    cfg.replace(cfg.find("#0a141e"), 7, "theme:nope");
    const auto warned = [](const Model &m) {
        const auto w = m.status(nullptr).warnings;
        return std::ranges::any_of(
            w, [](const std::string &s) { return contains(s, "unknown theme color: nope"); });
    };
    Model on = react_model(cfg);
    EXPECT_TRUE(warned(on));

    // Fixed by a reload, the warning goes.
    on.replace_config(parse_config(kReactCfg).value(), palette());
    EXPECT_FALSE(warned(on));

    // Reactions off resolve nothing anyone would see, so they warn about nothing.
    cfg.replace(cfg.find("[reactions]\n"), 12, "[reactions]\nenabled = false\n");
    EXPECT_FALSE(warned(react_model(cfg)));
}

// Ruling P17: when the visibility changes without a frame after it (the daemon forgets its
// backlight reading after composing a forced write; a spent budget clears in `record_write`), the
// deadline says a frame is due at once. One frame takes the new visibility, and it is not due
// again.
TEST(Model, AVisibilityChangeIsDueOnceThenTheDeadlineMovesOn) {
    const TimePoint t0 = Clock::now();
    const auto due = [](const Model &m, TimePoint now) {
        const std::optional<TimePoint> d = m.reaction_deadline();
        return d && *d <= now;
    };
    Model m = react_model();
    EXPECT_TRUE(due(m, t0)) << "the first frame takes the visibility";
    m.apply_signal(signal::AgentWaiting{"s1", std::nullopt}, t0);
    ASSERT_TRUE(m.inputs(t0 + 1s).has_value()); // the intro is over
    ASSERT_EQ(m.reaction_deadline(), t0 + 3600s);

    m.set_hw_backlight(false, t0 + 2s); // the Fn key, read before a toggle
    EXPECT_TRUE(due(m, t0 + 2s));
    EXPECT_TRUE(empty(m.inputs(t0 + 2s)->lamps));
    EXPECT_EQ(m.reaction_deadline(), t0 + 3600s) << "taken: not due again";

    m.forget_hw_backlight(); // the forced write turns the backlight on
    EXPECT_TRUE(due(m, t0 + 2s));
    EXPECT_EQ(m.inputs(t0 + 2s)->lamps, lit(kPeach, {Zone::Wasd})) << "steady at once";
    EXPECT_EQ(m.reaction_deadline(), t0 + 3600s);

    m.record_write(std::unexpected(BackendError{BackendError::Kind::Budget, ""}));
    EXPECT_TRUE(due(m, t0 + 3s));
    EXPECT_TRUE(empty(m.inputs(t0 + 3s)->lamps));
    m.record_write(true);
    EXPECT_TRUE(due(m, t0 + 4s));
    EXPECT_EQ(m.inputs(t0 + 4s)->lamps, lit(kPeach, {Zone::Wasd}));
    EXPECT_EQ(m.reaction_deadline(), t0 + 3600s);

    // With reactions off nothing is ever due.
    Model off = hex_model();
    EXPECT_EQ(off.reaction_deadline(), std::nullopt);
    off.set_hw_backlight(false, t0);
    EXPECT_EQ(off.reaction_deadline(), std::nullopt);
    ASSERT_TRUE(off.inputs(t0).has_value());
    off.forget_hw_backlight();
    EXPECT_EQ(off.reaction_deadline(), std::nullopt);
}

#include "core/lookbook.hpp"

#include "core/palette.hpp"
#include "reactions_config.hpp"

#include <chrono>
#include <cstddef>
#include <gtest/gtest.h>
#include <initializer_list>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

using namespace omenkbd;
using namespace std::chrono_literals;

namespace {

/// Every theme name the defaults use, each a different color so precedence shows in the result.
constexpr std::string_view kThemeCss = R"(
@define-color lavender #b4befe;
@define-color blue #89b4fa;
@define-color red #f38ba8;
@define-color green #a6e3a1;
@define-color yellow #f9e2af;
@define-color peach #fab387;
)";

constexpr Rgb kLavender{0xb4, 0xbe, 0xfe};
constexpr Rgb kBlue{0x89, 0xb4, 0xfa};
constexpr Rgb kRed{0xf3, 0x8b, 0xa8};
constexpr Rgb kGreen{0xa6, 0xe3, 0xa1};
constexpr Rgb kYellow{0xf9, 0xe2, 0xaf};
constexpr Rgb kPeach{0xfa, 0xb3, 0x87};
constexpr Rgb kDiscord{0x58, 0x65, 0xf2};

Resolver palette_resolver() {
    const Palette palette = Palette::from_waybar_css(kThemeCss);
    return [palette](const ColorRef &c) { return c.resolve(palette, ThemeColors::Exact).value(); };
}

LookBook look_of(const ReactionsCfg &cfg) { return LookBook(cfg, palette_resolver()); }

/// The spec's defaults.
LookBook default_look() { return look_of(default_reactions()); }

/// A layer that paints `color` on `zones` and leaves the others alone.
LayerOps lit(Rgb color, std::initializer_list<Zone> zones) {
    LayerOps ops;
    for (Zone z : zones) {
        ops[index(z)] = Paint{color};
    }
    return ops;
}

/// A layer that applies `op` to all four zones.
LayerOps everywhere(const ZoneOp &op) {
    LayerOps ops;
    ops.fill(op);
    return ops;
}

signal::Notified note(std::string app, std::string entry, Urgency urgency = Urgency::Normal) {
    return signal::Notified{std::move(app), std::move(entry), urgency};
}

/// The clip, which a test expects to exist. A missing one is a failure, and the empty clip it
/// returns then fails the test's own step checks as well.
Clip got(const std::optional<Clip> &clip) {
    EXPECT_TRUE(clip.has_value());
    return clip.value_or(Clip{});
}

/// The color of the notification's flash: its Left zone, which every flash paints.
std::optional<Rgb> flash_color(const std::optional<Clip> &clip) {
    if (!clip || clip->steps.empty()) {
        return std::nullopt;
    }
    const std::optional<ZoneOp> &op = clip->steps[0].ops[index(Zone::Left)];
    if (!op || !std::holds_alternative<Paint>(*op)) {
        return std::nullopt;
    }
    return std::get<Paint>(*op).color;
}

} // namespace

TEST(LookBook, WorkspaceSweepDirectionAndStep) {
    const LookBook look = default_look();

    const Clip up = got(look.workspace(1, 2));
    ASSERT_EQ(up.steps.size(), 3u); // Up: Left+Wasd, Middle, Right
    EXPECT_EQ(up.steps[0].ops, lit(kLavender, {Zone::Left, Zone::Wasd}));
    EXPECT_EQ(up.steps[1].ops, lit(kLavender, {Zone::Middle}));
    EXPECT_EQ(up.steps[2].ops, lit(kLavender, {Zone::Right}));
    for (const ClipStep &step : up.steps) {
        EXPECT_EQ(step.hold, 120ms);
    }

    const Clip down = got(look.workspace(3, 1));
    ASSERT_EQ(down.steps.size(), 3u); // Down: Right, Middle, Left (WASD stays with Left)
    EXPECT_EQ(down.steps[0].ops, lit(kLavender, {Zone::Right}));
    EXPECT_EQ(down.steps[1].ops, lit(kLavender, {Zone::Middle}));
    EXPECT_EQ(down.steps[2].ops, lit(kLavender, {Zone::Left, Zone::Wasd}));
    for (const ClipStep &step : down.steps) {
        EXPECT_EQ(step.hold, 120ms);
    }

    EXPECT_FALSE(look.workspace(2, 2).has_value());
}

TEST(LookBook, WorkspaceSweepFollowsTheConfiguredColorWasdAndStep) {
    ReactionsCfg cfg = default_reactions();
    cfg.step_ms = 200;
    cfg.workspace.color = ColorRef::theme("peach");
    cfg.workspace.wasd_with = WasdWith::Middle;
    const LookBook look = look_of(cfg);

    const Clip up = got(look.workspace(4, 9));
    ASSERT_EQ(up.steps.size(), 3u);
    EXPECT_EQ(up.steps[0].ops, lit(kPeach, {Zone::Left}));
    EXPECT_EQ(up.steps[1].ops, lit(kPeach, {Zone::Middle, Zone::Wasd}));
    EXPECT_EQ(up.steps[2].ops, lit(kPeach, {Zone::Right}));
    for (const ClipStep &step : up.steps) {
        EXPECT_EQ(step.hold, 200ms);
    }
}

TEST(LookBook, WindowPulsesUseTheConfiguredAmountsAndCanBeSwitchedOff) {
    const LookBook defaults = default_look();
    const Clip open = got(defaults.window_opened());
    ASSERT_EQ(open.steps.size(), 1u);
    EXPECT_EQ(open.steps[0].ops, everywhere(Mix{kWhite, 0.5}));
    EXPECT_EQ(open.steps[0].hold, 120ms);
    const Clip close = got(defaults.window_closed());
    ASSERT_EQ(close.steps.size(), 1u);
    EXPECT_EQ(close.steps[0].ops, everywhere(Scale{0.35}));
    EXPECT_EQ(close.steps[0].hold, 120ms);

    ReactionsCfg cfg = default_reactions();
    cfg.step_ms = 90;
    cfg.window.open = 0.2;
    cfg.window.close = 0.6;
    const LookBook tuned = look_of(cfg);
    const Clip tuned_open = got(tuned.window_opened());
    ASSERT_EQ(tuned_open.steps.size(), 1u);
    EXPECT_EQ(tuned_open.steps[0].ops, everywhere(Mix{kWhite, 0.2}));
    EXPECT_EQ(tuned_open.steps[0].hold, 90ms);
    const Clip tuned_close = got(tuned.window_closed());
    ASSERT_EQ(tuned_close.steps.size(), 1u);
    EXPECT_EQ(tuned_close.steps[0].ops, everywhere(Scale{0.6}));
    EXPECT_EQ(tuned_close.steps[0].hold, 90ms);

    cfg.window.open = 0.0;  // off
    cfg.window.close = 1.0; // off
    const LookBook off = look_of(cfg);
    EXPECT_FALSE(off.window_opened().has_value());
    EXPECT_FALSE(off.window_closed().has_value());

    // A dip to 0 is a real dip: only 1 means off.
    cfg.window.close = 0.0;
    const Clip dark = got(look_of(cfg).window_closed());
    ASSERT_EQ(dark.steps.size(), 1u);
    EXPECT_EQ(dark.steps[0].ops, everywhere(Scale{0.0}));
}

TEST(LookBook, NotificationColorPrecedence) {
    const LookBook look = default_look();

    // No match: the default color.
    EXPECT_EQ(flash_color(look.notified(note("Firefox", "firefox"), "")), kBlue);

    // Keys match the app name, case-insensitively.
    EXPECT_EQ(flash_color(look.notified(note("ZapZap", ""), "")), kGreen);
    EXPECT_EQ(flash_color(look.notified(note("DISCORD", ""), "")), kDiscord);

    // ... the desktop entry's last segment, case-insensitively.
    EXPECT_EQ(flash_color(look.notified(note("", "com.rtosta.zapzap"), "")), kGreen);
    EXPECT_EQ(flash_color(look.notified(note("", "com.rtosta.ZapZap"), "")), kGreen);

    // ... and the whole desktop entry.
    ReactionsCfg cfg = default_reactions();
    cfg.notify.apps.emplace("org.example.tool", ColorRef::theme("peach"));
    const LookBook with_entry = look_of(cfg);
    EXPECT_EQ(flash_color(with_entry.notified(note("", "Org.Example.Tool"), "")), kPeach);

    // The first identity found wins: the app name before the entry.
    EXPECT_EQ(flash_color(look.notified(note("ZapZap", "discord"), "")), kGreen);
    EXPECT_EQ(flash_color(look.notified(note("Chat", "discord"), "")), kDiscord);

    // Critical beats the app table and the default.
    EXPECT_EQ(
        flash_color(look.notified(note("ZapZap", "com.rtosta.zapzap", Urgency::Critical), "")),
        kRed);
    EXPECT_EQ(flash_color(look.notified(note("Firefox", "", Urgency::Critical), "")), kRed);

    // A configured default replaces the spec's.
    cfg.notify.color = ColorRef::hex(Rgb{1, 2, 3});
    const LookBook other = look_of(cfg);
    EXPECT_EQ(flash_color(other.notified(note("Firefox", ""), "")), (Rgb{1, 2, 3}));
}

TEST(LookBook, LowUrgencyIsSkippedByDefault) {
    const LookBook look = default_look();
    EXPECT_FALSE(look.notified(note("Firefox", "", Urgency::Low), "").has_value());
    EXPECT_TRUE(look.notified(note("Firefox", "", Urgency::Normal), "").has_value());
    EXPECT_TRUE(look.notified(note("Firefox", "", Urgency::Critical), "").has_value());

    ReactionsCfg cfg = default_reactions();
    cfg.notify.min_urgency = Urgency::Low;
    const LookBook everything = look_of(cfg);
    EXPECT_TRUE(everything.notified(note("Firefox", "", Urgency::Low), "").has_value());

    cfg.notify.min_urgency = Urgency::Critical;
    const LookBook only_critical = look_of(cfg);
    EXPECT_FALSE(only_critical.notified(note("Firefox", "", Urgency::Normal), "").has_value());
    EXPECT_TRUE(only_critical.notified(note("Firefox", "", Urgency::Critical), "").has_value());
}

TEST(LookBook, SkipsTheFocusedAppUnlessTurnedOff) {
    ReactionsCfg cfg = default_reactions();
    const LookBook look = look_of(cfg);
    const signal::Notified zap = note("ZapZap", "com.rtosta.zapzap");

    // The focused class equals the desktop entry, Hyprland's class for ZapZap.
    EXPECT_FALSE(look.notified(zap, "com.rtosta.zapzap").has_value());
    // It equals the app name or the entry's last segment as well, whatever the case.
    EXPECT_FALSE(look.notified(zap, "ZapZap").has_value());
    EXPECT_FALSE(look.notified(zap, "zapzap").has_value());
    EXPECT_FALSE(look.notified(zap, "COM.RTOSTA.ZAPZAP").has_value());
    // Another window is focused, or the match is only a part of an identity.
    EXPECT_TRUE(look.notified(zap, "firefox").has_value());
    EXPECT_TRUE(look.notified(zap, "com.rtosta").has_value());
    // Skipping comes before color: a critical notification of the focused app is skipped too.
    EXPECT_FALSE(look.notified(note("ZapZap", "com.rtosta.zapzap", Urgency::Critical), "ZapZap")
                     .has_value());

    cfg.notify.skip_if_focused = false;
    const LookBook always = look_of(cfg);
    EXPECT_TRUE(always.notified(zap, "com.rtosta.zapzap").has_value());
    EXPECT_TRUE(always.notified(zap, "ZapZap").has_value());
}

TEST(LookBook, EmptyIdentitiesNeverMatch) {
    ReactionsCfg cfg = default_reactions();
    cfg.notify.apps.emplace("", ColorRef::theme("peach")); // a hostile table
    const LookBook look = look_of(cfg);

    // `omen-kbd notify` with no --app and no --desktop-entry, before the first focus event: the
    // empty app is not "focused", and the empty key of the table is not matched.
    EXPECT_TRUE(look.notified(note("", ""), "").has_value());
    EXPECT_EQ(flash_color(look.notified(note("", ""), "")), kBlue);

    // An empty app does not match a focused class either.
    EXPECT_EQ(flash_color(look.notified(note("", ""), "alacritty")), kBlue);
    EXPECT_TRUE(look.notified(note("", "org.example.tool"), "alacritty").has_value());

    // An entry with no dot has no last segment, and one ending in a dot has an empty one.
    EXPECT_EQ(flash_color(look.notified(note("", "nodots"), "")), kBlue);
    EXPECT_EQ(flash_color(look.notified(note("", "com.rtosta."), "")), kBlue);
    EXPECT_TRUE(look.notified(note("", "com.rtosta."), "").has_value());

    // An entry with no dot is still an identity of its own.
    EXPECT_FALSE(look.notified(note("", "zapzap"), "zapzap").has_value());
    EXPECT_EQ(flash_color(look.notified(note("", "zapzap"), "")), kGreen);
}

TEST(LookBook, NotifyFlashIsOneBlinkOnAllZones) {
    ReactionsCfg cfg = default_reactions();
    cfg.blink_ms = 200;
    cfg.step_ms = 90; // the flash follows blink_ms, not step_ms
    const LookBook look = look_of(cfg);

    const Clip clip = got(look.notified(note("ZapZap", ""), ""));
    ASSERT_EQ(clip.steps.size(), 1u);
    EXPECT_EQ(clip.steps[0].ops, lit(kGreen, {Zone::Left, Zone::Middle, Zone::Right, Zone::Wasd}));
    EXPECT_EQ(clip.steps[0].hold, 200ms);
    EXPECT_EQ(clip.total(), 200ms);
}

TEST(LookBook, AgentDoneUsesAgentZonesAndTheDoneColor) {
    const LookBook defaults = default_look();
    const Clip done = got(defaults.agent_done());
    ASSERT_EQ(done.steps.size(), 1u);
    EXPECT_EQ(done.steps[0].ops, lit(kGreen, {Zone::Wasd}));
    EXPECT_EQ(done.steps[0].hold, 150ms);

    // The configured zones, the done color (not the waiting one) and blink_ms.
    ReactionsCfg cfg = default_reactions();
    cfg.blink_ms = 250;
    cfg.agent.zones = {Zone::Left, Zone::Right};
    cfg.agent.waiting = ColorRef::theme("peach");
    cfg.agent.done = ColorRef::theme("yellow");
    const LookBook tuned = look_of(cfg);
    const Clip custom = got(tuned.agent_done());
    ASSERT_EQ(custom.steps.size(), 1u);
    EXPECT_EQ(custom.steps[0].ops, lit(kYellow, {Zone::Left, Zone::Right}));
    EXPECT_EQ(custom.steps[0].hold, 250ms);

    // No agent zones, no flash.
    cfg.agent.zones.clear();
    EXPECT_FALSE(look_of(cfg).agent_done().has_value());
}

TEST(LookBook, StylesCarryZonesColorBlinksAndTtl) {
    const LookBook defaults = default_look();

    const LampStyle urgent = defaults.urgent_style();
    EXPECT_EQ(urgent.zones, std::vector<Zone>{Zone::Right});
    EXPECT_EQ(urgent.color, kYellow);
    EXPECT_EQ(urgent.blinks, 2);
    EXPECT_EQ(urgent.blink, 150ms);
    EXPECT_FALSE(urgent.ttl.has_value());

    // The agent lamp is the waiting light, and it expires.
    const LampStyle agent = defaults.agent_style();
    EXPECT_EQ(agent.zones, std::vector<Zone>{Zone::Wasd});
    EXPECT_EQ(agent.color, kPeach);
    EXPECT_EQ(agent.blinks, 2);
    EXPECT_EQ(agent.blink, 150ms);
    EXPECT_EQ(agent.ttl, 3600s);

    ReactionsCfg cfg = default_reactions();
    cfg.blink_ms = 300;
    cfg.urgent.zones = {Zone::Left, Zone::Middle};
    cfg.urgent.color = ColorRef::theme("red");
    cfg.urgent.blinks = 0; // passed through: LampSet treats 0 and 1 alike
    cfg.agent.zones = {};  // passed through: an empty list disables the lamp
    cfg.agent.waiting = ColorRef::theme("green");
    cfg.agent.blinks = 5;
    cfg.agent.ttl_s = 60;
    const LookBook tuned = look_of(cfg);

    const LampStyle u = tuned.urgent_style();
    EXPECT_EQ(u.zones, (std::vector<Zone>{Zone::Left, Zone::Middle}));
    EXPECT_EQ(u.color, kRed);
    EXPECT_EQ(u.blinks, 0);
    EXPECT_EQ(u.blink, 300ms);
    EXPECT_FALSE(u.ttl.has_value());

    const LampStyle a = tuned.agent_style();
    EXPECT_TRUE(a.zones.empty());
    EXPECT_EQ(a.color, kGreen);
    EXPECT_EQ(a.blinks, 5);
    EXPECT_EQ(a.blink, 300ms);
    EXPECT_EQ(a.ttl, 60s);
}

TEST(LookBook, LimitsCarryTheRateTheWindowCooldownAndTheGap) {
    const PlayerLimits defaults = default_look().limits();
    EXPECT_EQ(defaults.max_per_minute, 30u);
    EXPECT_EQ(defaults.window_cooldown, 500ms);
    EXPECT_EQ(defaults.gap, 150ms);

    ReactionsCfg cfg = default_reactions();
    cfg.max_per_minute = 7;
    cfg.window.cooldown_ms = 1200;
    cfg.blink_ms = 320;
    cfg.step_ms = 90; // the gap follows blink_ms, not step_ms
    const PlayerLimits tuned = look_of(cfg).limits();
    EXPECT_EQ(tuned.max_per_minute, 7u);
    EXPECT_EQ(tuned.window_cooldown, 1200ms);
    EXPECT_EQ(tuned.gap, 320ms);
}

TEST(LookBook, EveryColorIsResolvedOnceAtConstruction) {
    // One distinct color per setting, so a skipped or repeated resolve shows in the counts.
    ReactionsCfg cfg = default_reactions();
    cfg.workspace.color = ColorRef::hex(Rgb{1, 0, 0});
    cfg.notify.color = ColorRef::hex(Rgb{2, 0, 0});
    cfg.notify.critical = ColorRef::hex(Rgb{3, 0, 0});
    cfg.notify.apps.clear();
    cfg.notify.apps.emplace("one", ColorRef::hex(Rgb{4, 0, 0}));
    cfg.notify.apps.emplace("two", ColorRef::hex(Rgb{5, 0, 0}));
    cfg.urgent.color = ColorRef::hex(Rgb{6, 0, 0});
    cfg.agent.waiting = ColorRef::hex(Rgb{7, 0, 0});
    cfg.agent.done = ColorRef::hex(Rgb{8, 0, 0});

    std::map<std::string, int> asked;
    int calls = 0;
    const Resolver counting = [&](const ColorRef &c) {
        ++calls;
        ++asked[c.to_string()];
        return Rgb{9, 9, 9};
    };
    const LookBook look(cfg, counting);

    // Six single settings and two apps.
    EXPECT_EQ(calls, 8);
    EXPECT_EQ(asked.size(), 8u);
    for (const auto &[color, count] : asked) {
        EXPECT_EQ(count, 1) << color;
    }

    // Using the look resolves nothing more.
    EXPECT_TRUE(look.workspace(1, 2).has_value());
    EXPECT_TRUE(look.workspace(2, 1).has_value());
    EXPECT_TRUE(look.window_opened().has_value());
    EXPECT_TRUE(look.window_closed().has_value());
    EXPECT_TRUE(look.notified(note("one", ""), "").has_value());
    EXPECT_TRUE(look.notified(note("other", "", Urgency::Critical), "").has_value());
    EXPECT_TRUE(look.notified(note("other", ""), "").has_value());
    EXPECT_TRUE(look.agent_done().has_value());
    EXPECT_EQ(look.urgent_style().color, (Rgb{9, 9, 9}));
    EXPECT_EQ(look.agent_style().color, (Rgb{9, 9, 9}));
    EXPECT_EQ(look.limits().max_per_minute, 30u);
    EXPECT_EQ(calls, 8);
}

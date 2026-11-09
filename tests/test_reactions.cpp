#include "daemon/reactions.hpp"

#include "core/palette.hpp"

#include <chrono>
#include <gtest/gtest.h>
#include <initializer_list>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

using namespace omenkbd;
using namespace std::chrono_literals;

namespace {

// Every color the reactions use, each different. A black one would still be a Paint, so a test
// could pass without proving the right color got there.
constexpr Rgb kSweep{10, 20, 30};
constexpr Rgb kNote{40, 50, 60};
constexpr Rgb kCritical{70, 80, 90};
constexpr Rgb kUrgent{230, 40, 40};
constexpr Rgb kWaiting{10, 200, 90};
constexpr Rgb kDone{200, 200, 10};

constexpr Visibility kShown{true, true};
/// The keyboard is lit but nobody is at the machine.
constexpr Visibility kAway{true, false};
/// Somebody is there but the keyboard is dark (the Fn key, brightness 0, a rule).
constexpr Visibility kDark{false, true};

const TimePoint t0 = Clock::now();

/// Maps hex references to themselves and theme references through `css` (waybar color lines).
Resolver resolver(std::string_view css = {}) {
    const Palette palette = Palette::from_waybar_css(css);
    return [palette](const ColorRef &c) { return c.resolve(palette, ThemeColors::Exact).value(); };
}

/// Enabled, with the spec's timings (120 ms steps, 150 ms blinks, the attention light on Right, the
/// Claude lamp on WASD for an hour) and a distinct color for each reaction.
ReactionsCfg make_cfg() {
    ReactionsCfg cfg;
    cfg.enabled = true;
    cfg.workspace.color = ColorRef::hex(kSweep);
    cfg.notify.color = ColorRef::hex(kNote);
    cfg.notify.critical = ColorRef::hex(kCritical);
    cfg.urgent.color = ColorRef::hex(kUrgent);
    cfg.agent.waiting = ColorRef::hex(kWaiting);
    cfg.agent.done = ColorRef::hex(kDone);
    return cfg;
}

Reactions started(const ReactionsCfg &cfg = make_cfg()) {
    Reactions r;
    r.reconfigure(cfg, resolver());
    return r;
}

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

std::vector<std::string> names(std::initializer_list<std::string> list) { return list; }

signal::Notified zapzap() {
    return signal::Notified{"ZapZap", "com.rtosta.zapzap", Urgency::Normal};
}

} // namespace

TEST(Reactions, DisabledDoesNothing) {
    ReactionsCfg off_cfg = make_cfg();
    off_cfg.enabled = false;
    Reactions never_configured;
    Reactions configured_off;
    configured_off.reconfigure(off_cfg, resolver());

    for (Reactions *r : {&never_configured, &configured_off}) {
        const std::vector<Signal> all{
            signal::WorkspaceSwitched{1, 2},
            signal::WindowOpened{},
            signal::WindowClosed{},
            signal::Notified{"ZapZap", "com.rtosta.zapzap", Urgency::Critical},
            signal::UrgentRaised{"a"},
            signal::AgentWaiting{"s", "b"},
            signal::AgentWaiting{"t", std::nullopt},
            signal::AgentDone{"u", std::nullopt},
            signal::FocusChanged{"c", "kitty"}};
        for (const Signal &sig : all) {
            r->on_signal(sig, t0, kShown);
        }
        r->update(t0 + 10ms, kShown);
        EXPECT_TRUE(empty(r->layers(t0 + 10ms).lamps));
        EXPECT_TRUE(empty(r->layers(t0 + 10ms).transient));
        EXPECT_FALSE(r->next_change().has_value());
        EXPECT_EQ(r->status(), ReactionsStatus{});
    }

    // Nothing was kept for later: switching it on starts from a blank slate.
    configured_off.reconfigure(make_cfg(), resolver());
    configured_off.update(t0 + 20ms, kShown);
    EXPECT_TRUE(empty(configured_off.layers(t0 + 20ms).lamps));
    EXPECT_TRUE(empty(configured_off.layers(t0 + 20ms).transient));
    EXPECT_EQ(configured_off.status(), (ReactionsStatus{true, {}, std::nullopt}));
}

TEST(Reactions, WorkspaceSwitchPlaysASweepThenEmptiesTheLayer) {
    Reactions r = started();
    r.on_signal(signal::WorkspaceSwitched{1, 2}, t0, kShown);
    EXPECT_EQ(r.layers(t0).transient, lit(kSweep, {Zone::Left, Zone::Wasd}));
    EXPECT_TRUE(empty(r.layers(t0).lamps));
    EXPECT_EQ(r.next_change(), t0 + 120ms);

    r.update(t0 + 120ms, kShown);
    EXPECT_EQ(r.layers(t0 + 120ms).transient, lit(kSweep, {Zone::Middle}));
    EXPECT_EQ(r.next_change(), t0 + 240ms);

    r.update(t0 + 240ms, kShown);
    EXPECT_EQ(r.layers(t0 + 240ms).transient, lit(kSweep, {Zone::Right}));
    EXPECT_EQ(r.next_change(), t0 + 360ms);

    r.update(t0 + 360ms, kShown);
    EXPECT_TRUE(empty(r.layers(t0 + 360ms).transient));
    EXPECT_FALSE(r.next_change().has_value());
    EXPECT_FALSE(r.status().playing.has_value());

    // Going down sweeps the other way: `from` and `to` are not swapped on the way in.
    Reactions down = started();
    down.on_signal(signal::WorkspaceSwitched{3, 1}, t0, kShown);
    EXPECT_EQ(down.layers(t0).transient, lit(kSweep, {Zone::Right}));
}

TEST(Reactions, ShortReactionsNeedAVisiblePresentKeyboard) {
    for (const Visibility vis : {kAway, kDark}) {
        Reactions r = started();
        r.on_signal(signal::WorkspaceSwitched{1, 2}, t0, vis);
        r.on_signal(signal::WindowOpened{}, t0, vis);
        r.on_signal(zapzap(), t0, vis);
        r.on_signal(signal::AgentDone{"s", std::nullopt}, t0, vis);
        EXPECT_TRUE(empty(r.layers(t0).transient));
        EXPECT_FALSE(r.status().playing.has_value());
        EXPECT_FALSE(r.next_change().has_value());

        // A lamp raised now lights steadily at once. An intro would be dark at 150 ms.
        r.on_signal(signal::UrgentRaised{"a"}, t0, vis);
        EXPECT_FALSE(r.next_change().has_value()); // no intro boundaries, and no TTL
        r.update(t0 + 150ms, kShown);
        EXPECT_EQ(r.layers(t0 + 150ms).lamps, lit(kUrgent, {Zone::Right}));
        EXPECT_EQ(r.layers(t0 + 160ms).lamps, lit(kUrgent, {Zone::Right}));
        EXPECT_FALSE(r.next_change().has_value());
    }

    // Once the keyboard is lit and somebody is there, the same signals play.
    Reactions shown = started();
    shown.on_signal(signal::WorkspaceSwitched{1, 2}, t0, kShown);
    EXPECT_FALSE(empty(shown.layers(t0).transient));
}

TEST(Reactions, ShortClipsAreDroppedWhenTheUserLeavesMidClip) {
    for (const Visibility gone : {kAway, kDark}) {
        Reactions r = started();
        r.on_signal(signal::WorkspaceSwitched{1, 2}, t0, kShown);
        r.on_signal(zapzap(), t0 + 10ms, kShown); // queued behind the sweep
        EXPECT_EQ(r.status().playing, "workspace");

        r.update(t0 + 130ms, gone); // the sweep would be showing its Middle step
        EXPECT_TRUE(empty(r.layers(t0 + 130ms).transient));
        EXPECT_FALSE(r.status().playing.has_value());
        EXPECT_FALSE(r.next_change().has_value());

        // Coming back resumes nothing: the sweep is gone, and so is the queued flash.
        r.update(t0 + 1s, kShown);
        EXPECT_TRUE(empty(r.layers(t0 + 1s).transient));
        EXPECT_FALSE(r.status().playing.has_value());
        EXPECT_FALSE(r.next_change().has_value());
    }
}

TEST(Reactions, WaitingAgentLightsTheLampAndFocusingItsWindowClearsIt) {
    Reactions r = started();
    const LayerOps on = lit(kWaiting, {Zone::Wasd});
    r.on_signal(signal::FocusChanged{"w0", "kitty"}, t0, kShown);
    r.on_signal(signal::AgentWaiting{"s1", "w1"}, t0, kShown);
    EXPECT_EQ(r.layers(t0).lamps, on);
    EXPECT_TRUE(empty(r.layers(t0 + 150ms).lamps)); // the intro blinks: on, off, on
    EXPECT_EQ(r.layers(t0 + 300ms).lamps, on);
    EXPECT_TRUE(empty(r.layers(t0).transient));
    EXPECT_EQ(r.status().lamps, names({"agent"}));
    EXPECT_EQ(r.next_change(), t0 + 150ms);

    r.update(t0 + 150ms, kShown);
    EXPECT_EQ(r.next_change(), t0 + 300ms);
    r.update(t0 + 300ms, kShown);
    EXPECT_EQ(r.next_change(), t0 + 3600s); // only the TTL is left
    EXPECT_EQ(r.layers(t0 + 10s).lamps, on);

    r.on_signal(signal::FocusChanged{"w1", "kitty"}, t0 + 10s, kShown);
    EXPECT_TRUE(empty(r.layers(t0 + 10s).lamps));
    EXPECT_TRUE(r.status().lamps.empty());
    EXPECT_FALSE(r.next_change().has_value());
}

TEST(Reactions, WaitingAgentWhoseWindowIsFocusedDoesNotLight) {
    Reactions r = started();
    r.on_signal(signal::FocusChanged{"w1", "kitty"}, t0, kShown);
    r.on_signal(signal::AgentWaiting{"s1", "w1"}, t0, kShown);
    EXPECT_TRUE(empty(r.layers(t0).lamps));
    EXPECT_TRUE(r.status().lamps.empty());
    EXPECT_FALSE(r.next_change().has_value());

    // A session without a window is never the focused one.
    r.on_signal(signal::AgentWaiting{"s2", std::nullopt}, t0, kShown);
    EXPECT_EQ(r.layers(t0).lamps, lit(kWaiting, {Zone::Wasd}));
    EXPECT_EQ(r.status().lamps, names({"agent"}));

    // Before the first focus event nothing counts as focused (R10), not even an empty address.
    Reactions fresh = started();
    fresh.on_signal(signal::AgentWaiting{"s3", "w1"}, t0, kShown);
    EXPECT_EQ(fresh.status().lamps, names({"agent"}));
    Reactions blank = started();
    blank.on_signal(signal::AgentWaiting{"s4", ""}, t0, kShown);
    EXPECT_EQ(blank.status().lamps, names({"agent"}));
}

TEST(Reactions, AgentDoneClearsTheLampAndFlashesUnlessItsWindowIsFocused) {
    Reactions r = started();
    r.on_signal(signal::FocusChanged{"w0", "kitty"}, t0, kShown);
    r.on_signal(signal::AgentWaiting{"s1", "w1"}, t0, kShown);
    r.update(t0 + 400ms, kShown); // the intro is over

    r.on_signal(signal::AgentDone{"s1", "w1"}, t0 + 400ms, kShown);
    EXPECT_TRUE(r.status().lamps.empty());
    EXPECT_TRUE(empty(r.layers(t0 + 400ms).lamps));
    EXPECT_EQ(r.layers(t0 + 400ms).transient, lit(kDone, {Zone::Wasd}));
    EXPECT_EQ(r.status().playing, "agent-done");
    EXPECT_EQ(r.next_change(), t0 + 550ms);
    r.update(t0 + 550ms, kShown);
    EXPECT_TRUE(empty(r.layers(t0 + 550ms).transient)); // one blink, then it is over
    EXPECT_FALSE(r.status().playing.has_value());

    // A session without a window flashes too.
    r.on_signal(signal::AgentDone{"s2", std::nullopt}, t0 + 1s, kShown);
    EXPECT_EQ(r.layers(t0 + 1s).transient, lit(kDone, {Zone::Wasd}));

    // The window of the finished session has focus: the lamp still goes, the flash does not.
    Reactions focused = started();
    focused.on_signal(signal::AgentWaiting{"s1", std::nullopt}, t0, kShown);
    focused.on_signal(signal::FocusChanged{"w1", "kitty"}, t0 + 1s, kShown);
    EXPECT_EQ(focused.status().lamps, names({"agent"})); // the key carries no window to clear
    focused.on_signal(signal::AgentDone{"s1", "w1"}, t0 + 2s, kShown);
    EXPECT_TRUE(focused.status().lamps.empty());
    EXPECT_TRUE(empty(focused.layers(t0 + 2s).transient));
    EXPECT_FALSE(focused.status().playing.has_value());
}

TEST(Reactions, UrgentOnTheFocusedWindowIsIgnoredAndFocusClearsIt) {
    Reactions r = started();
    r.on_signal(signal::FocusChanged{"a", "kitty"}, t0, kShown);
    r.on_signal(signal::UrgentRaised{"a"}, t0, kShown);
    EXPECT_TRUE(empty(r.layers(t0).lamps));
    EXPECT_TRUE(r.status().lamps.empty());
    EXPECT_FALSE(r.next_change().has_value());

    r.on_signal(signal::UrgentRaised{"b"}, t0, kShown);
    EXPECT_EQ(r.layers(t0).lamps, lit(kUrgent, {Zone::Right}));
    EXPECT_TRUE(empty(r.layers(t0 + 150ms).lamps)); // an intro blink
    EXPECT_EQ(r.status().lamps, names({"urgent"}));

    r.on_signal(signal::FocusChanged{"b", "kitty"}, t0 + 1s, kShown);
    EXPECT_TRUE(empty(r.layers(t0 + 1s).lamps));
    EXPECT_TRUE(r.status().lamps.empty());

    // Hyprland clearing the flag clears the light as well; clearing an unknown window is harmless.
    r.on_signal(signal::UrgentRaised{"c"}, t0 + 2s, kShown);
    EXPECT_EQ(r.status().lamps, names({"urgent"}));
    r.on_signal(signal::UrgentCleared{"zzz"}, t0 + 2s, kShown);
    EXPECT_EQ(r.status().lamps, names({"urgent"}));
    r.on_signal(signal::UrgentCleared{"c"}, t0 + 2s, kShown);
    EXPECT_TRUE(r.status().lamps.empty());
    EXPECT_TRUE(empty(r.layers(t0 + 2s).lamps));
}

TEST(Reactions, UrgentFromAnAgentsWindowIsAbsorbed) {
    Reactions r = started();
    r.on_signal(signal::AgentWaiting{"s1", "w1"}, t0, kShown);
    r.on_signal(signal::UrgentRaised{"w1"}, t0, kShown); // the agent light already covers w1
    EXPECT_EQ(r.status().lamps, names({"agent"}));
    EXPECT_EQ(r.layers(t0).lamps, lit(kWaiting, {Zone::Wasd})); // the urgent zone, Right, is dark
    r.update(t0 + 400ms, kShown);
    EXPECT_EQ(r.layers(t0 + 400ms).lamps, lit(kWaiting, {Zone::Wasd}));

    // The urgent light was dropped, not parked: once the agent is working, w1 shows nothing.
    r.on_signal(signal::AgentWorking{"s1"}, t0 + 500ms, kShown);
    EXPECT_TRUE(r.status().lamps.empty());
    EXPECT_TRUE(empty(r.layers(t0 + 500ms).lamps));

    // Another window's urgent light is not absorbed.
    r.on_signal(signal::AgentWaiting{"s2", "w2"}, t0 + 1s, kShown);
    r.on_signal(signal::UrgentRaised{"w3"}, t0 + 1s, kShown);
    EXPECT_EQ(r.status().lamps, names({"agent", "urgent"}));
}

TEST(Reactions, AnAgentRaiseDropsAnUrgentLightOnItsWindow) {
    Reactions r = started();
    r.on_signal(signal::UrgentRaised{"w1"}, t0, kShown);
    EXPECT_EQ(r.status().lamps, names({"urgent"}));
    EXPECT_EQ(r.layers(t0).lamps, lit(kUrgent, {Zone::Right}));
    r.update(t0 + 400ms, kShown);
    EXPECT_EQ(r.layers(t0 + 400ms).lamps, lit(kUrgent, {Zone::Right}));

    r.on_signal(signal::AgentWaiting{"s1", "w1"}, t0 + 1s, kShown);
    EXPECT_EQ(r.status().lamps, names({"agent"})); // only the Agent kind is active
    r.update(t0 + 2s, kShown);
    EXPECT_EQ(r.layers(t0 + 2s).lamps, lit(kWaiting, {Zone::Wasd})); // steady; Right is dark

    // The window now shows no light, even if Hyprland still reports it urgent. That is the
    // extended spec section 5.5: a window shows one light, the agent's. The tracker's urgent set is
    // separate state, and it does not come back by itself.
    r.on_signal(signal::AgentWorking{"s1"}, t0 + 3s, kShown);
    EXPECT_TRUE(r.status().lamps.empty());
    EXPECT_TRUE(empty(r.layers(t0 + 3s).lamps));
}

TEST(Reactions, UrgentResetClearsTheAttentionLight) {
    Reactions r = started();
    r.on_signal(signal::UrgentRaised{"a"}, t0, kShown);
    r.on_signal(signal::UrgentRaised{"b"}, t0, kShown);
    r.on_signal(signal::AgentWaiting{"s1", "w9"}, t0, kShown);
    r.update(t0 + 1s, kShown);
    EXPECT_EQ(r.status().lamps, names({"agent", "urgent"}));

    r.on_signal(signal::UrgentReset{}, t0 + 1s, kShown); // Hyprland went away
    EXPECT_EQ(r.status().lamps, names({"agent"}));
    EXPECT_EQ(r.layers(t0 + 1s).lamps, lit(kWaiting, {Zone::Wasd})); // the agent light stays
}

TEST(Reactions, ReconfigureKeepsLampKeysAndLetsAPlayingClipFinish) {
    Reactions r = started();
    r.on_signal(signal::AgentWaiting{"s1", "w1"}, t0, kShown);
    r.update(t0 + 1s, kShown);
    r.on_signal(signal::WorkspaceSwitched{1, 2}, t0 + 1s, kShown);

    constexpr Rgb kSweep2{1, 2, 3};
    constexpr Rgb kWaiting2{4, 5, 6};
    ReactionsCfg cfg2 = make_cfg();
    cfg2.step_ms = 200;
    cfg2.workspace.color = ColorRef::hex(kSweep2);
    cfg2.agent.waiting = ColorRef::hex(kWaiting2);
    cfg2.agent.ttl_s = 60;
    r.reconfigure(cfg2, resolver());

    // The key stays and the lamp takes the new look at once.
    EXPECT_EQ(r.status().lamps, names({"agent"}));
    EXPECT_EQ(r.layers(t0 + 1s + 50ms).lamps, lit(kWaiting2, {Zone::Wasd}));

    // The playing sweep finishes with its old color and its old 120 ms steps.
    EXPECT_EQ(r.layers(t0 + 1s + 50ms).transient, lit(kSweep, {Zone::Left, Zone::Wasd}));
    r.update(t0 + 1s + 130ms, kShown);
    EXPECT_EQ(r.layers(t0 + 1s + 130ms).transient, lit(kSweep, {Zone::Middle}));
    r.update(t0 + 1s + 250ms, kShown);
    EXPECT_EQ(r.layers(t0 + 1s + 250ms).transient, lit(kSweep, {Zone::Right}));
    r.update(t0 + 1s + 360ms, kShown);
    EXPECT_TRUE(empty(r.layers(t0 + 1s + 360ms).transient));

    // The next sweep has the new look: the new color, and steps of 200 ms.
    r.on_signal(signal::WorkspaceSwitched{1, 2}, t0 + 2s, kShown);
    EXPECT_EQ(r.layers(t0 + 2s + 130ms).transient, lit(kSweep2, {Zone::Left, Zone::Wasd}));
    EXPECT_EQ(r.layers(t0 + 2s + 200ms).transient, lit(kSweep2, {Zone::Middle}));

    // The new TTL counts from the raise before the reload.
    r.update(t0 + 59s, kShown);
    EXPECT_EQ(r.status().lamps, names({"agent"}));
    r.update(t0 + 60s, kShown);
    EXPECT_TRUE(r.status().lamps.empty());
}

TEST(Reactions, ReconfigureAppliesTheNewRateLimit) {
    Reactions r = started();
    ReactionsCfg cfg = make_cfg();
    cfg.max_per_minute = 1;
    r.reconfigure(cfg, resolver());

    r.on_signal(signal::WorkspaceSwitched{1, 2}, t0, kShown);
    EXPECT_FALSE(empty(r.layers(t0).transient));
    r.update(t0 + 1s, kShown);
    r.on_signal(signal::WorkspaceSwitched{1, 2}, t0 + 1s, kShown); // the minute's one is spent
    EXPECT_TRUE(empty(r.layers(t0 + 1s).transient));
    r.on_signal(signal::WorkspaceSwitched{1, 2}, t0 + 61s, kShown);
    EXPECT_FALSE(empty(r.layers(t0 + 61s).transient));
}

TEST(Reactions, ReconfigureToDisabledClearsLampsAndClips) {
    Reactions r = started();
    r.on_signal(signal::AgentWaiting{"s1", "w1"}, t0, kShown);
    r.on_signal(signal::UrgentRaised{"w2"}, t0, kShown);
    r.on_signal(signal::WorkspaceSwitched{1, 2}, t0, kShown);
    ASSERT_EQ(r.status().lamps, names({"agent", "urgent"}));
    ASSERT_EQ(r.status().playing, "workspace");
    ASSERT_TRUE(r.next_change().has_value());

    ReactionsCfg off = make_cfg();
    off.enabled = false;
    r.reconfigure(off, resolver());
    EXPECT_TRUE(empty(r.layers(t0 + 10ms).lamps));
    EXPECT_TRUE(empty(r.layers(t0 + 10ms).transient));
    EXPECT_FALSE(r.next_change().has_value());
    EXPECT_EQ(r.status(), ReactionsStatus{});

    // Turning it back on does not bring anything back.
    r.reconfigure(make_cfg(), resolver());
    r.update(t0 + 20ms, kShown);
    EXPECT_TRUE(empty(r.layers(t0 + 20ms).lamps));
    EXPECT_TRUE(empty(r.layers(t0 + 20ms).transient));
    EXPECT_EQ(r.status(), (ReactionsStatus{true, {}, std::nullopt}));
}

TEST(Reactions, RestyleChangesTheLampColorWithoutLosingKeys) {
    constexpr Rgb kAlertA{0x0a, 0xa0, 0x5a};
    constexpr Rgb kAlertB{0xc0, 0x30, 0x70};
    constexpr Rgb kSweepB{0x30, 0x40, 0x50};
    ReactionsCfg cfg = make_cfg();
    cfg.agent.waiting = ColorRef::theme("alert");
    cfg.workspace.color = ColorRef::theme("sweep");
    Reactions r;
    r.reconfigure(cfg, resolver("@define-color alert #0aa05a;\n@define-color sweep #0a141e;\n"));

    r.on_signal(signal::AgentWaiting{"s1", "w1"}, t0, kShown);
    EXPECT_EQ(r.layers(t0).lamps, lit(kAlertA, {Zone::Wasd}));

    r.restyle(resolver("@define-color alert #c03070;\n@define-color sweep #304050;\n"));
    EXPECT_EQ(r.status().lamps, names({"agent"}));
    EXPECT_EQ(r.layers(t0).lamps, lit(kAlertB, {Zone::Wasd}));
    EXPECT_TRUE(empty(r.layers(t0 + 150ms).lamps)); // the intro goes on where it was
    EXPECT_EQ(r.layers(t0 + 300ms).lamps, lit(kAlertB, {Zone::Wasd}));

    r.on_signal(signal::WorkspaceSwitched{1, 2}, t0 + 1s, kShown);
    EXPECT_EQ(r.layers(t0 + 1s).transient, lit(kSweepB, {Zone::Left, Zone::Wasd}));
}

TEST(Reactions, LampsAreHiddenWhileTheKeyboardIsNotVisible) {
    Reactions r = started();
    r.on_signal(signal::UrgentRaised{"a"}, t0, kDark);
    EXPECT_TRUE(empty(r.layers(t0).lamps));         // nobody sees the zones, so nothing is drawn
    EXPECT_EQ(r.status().lamps, names({"urgent"})); // but the key is held
    r.update(t0 + 100ms, kDark);
    EXPECT_TRUE(empty(r.layers(t0 + 100ms).lamps));

    // The keyboard comes back: the lamp shows steady, with no blink (150 ms would be dark in an
    // intro).
    r.update(t0 + 150ms, kShown);
    EXPECT_EQ(r.layers(t0 + 150ms).lamps, lit(kUrgent, {Zone::Right}));
    EXPECT_EQ(r.layers(t0 + 160ms).lamps, lit(kUrgent, {Zone::Right}));
    EXPECT_FALSE(r.next_change().has_value());

    // And an absent user does not hide it: only the keyboard does.
    r.update(t0 + 1s, kAway);
    EXPECT_EQ(r.layers(t0 + 1s).lamps, lit(kUrgent, {Zone::Right}));
    r.update(t0 + 2s, kDark);
    EXPECT_TRUE(empty(r.layers(t0 + 2s).lamps));
}

TEST(Reactions, SuppressDropsClipsButKeepsLamps) {
    Reactions r = started();
    r.on_signal(signal::AgentWaiting{"s1", "w1"}, t0, kShown);
    r.update(t0 + 1s, kShown);
    r.on_signal(signal::WorkspaceSwitched{1, 2}, t0 + 1s, kShown);
    r.on_signal(zapzap(), t0 + 1s + 10ms, kShown); // queued behind the sweep
    ASSERT_EQ(r.status().playing, "workspace");

    r.suppress();
    EXPECT_TRUE(empty(r.layers(t0 + 1s + 20ms).transient));
    EXPECT_FALSE(r.status().playing.has_value());
    EXPECT_EQ(r.status().lamps, names({"agent"}));
    EXPECT_EQ(r.layers(t0 + 1s + 20ms).lamps, lit(kWaiting, {Zone::Wasd}));

    // The queued flash is gone too, and only the lamp's TTL is left to wait for.
    r.update(t0 + 2s, kShown);
    EXPECT_TRUE(empty(r.layers(t0 + 2s).transient));
    EXPECT_FALSE(r.status().playing.has_value());
    EXPECT_EQ(r.next_change(), t0 + 3600s);

    // Resuming is not a mode: the next signal plays as usual.
    r.on_signal(signal::WorkspaceSwitched{1, 2}, t0 + 3s, kShown);
    EXPECT_EQ(r.layers(t0 + 3s).transient, lit(kSweep, {Zone::Left, Zone::Wasd}));
}

TEST(Reactions, NextChangeCoversIntroClipAndTtl) {
    Reactions r = started();
    r.on_signal(signal::AgentWaiting{"s1", "w1"}, t0, kShown); // intro changes at 150, 300 ms
    EXPECT_EQ(r.next_change(), t0 + 150ms);

    // A sweep from 50 ms changes at 170, 290 and 410 ms. The nearest of the two sources wins.
    r.on_signal(signal::WorkspaceSwitched{1, 2}, t0 + 50ms, kShown);
    EXPECT_EQ(r.next_change(), t0 + 150ms);
    r.update(t0 + 150ms, kShown);
    EXPECT_EQ(r.next_change(), t0 + 170ms);
    r.update(t0 + 170ms, kShown);
    EXPECT_EQ(r.next_change(), t0 + 290ms);
    r.update(t0 + 290ms, kShown);
    EXPECT_EQ(r.next_change(), t0 + 300ms);
    r.update(t0 + 300ms, kShown);
    EXPECT_EQ(r.next_change(), t0 + 410ms);
    r.update(t0 + 410ms, kShown);
    EXPECT_EQ(r.next_change(), t0 + 3600s); // the TTL

    // Raising the key again moves its expiry.
    r.on_signal(signal::AgentWaiting{"s1", "w1"}, t0 + 1000s, kShown);
    EXPECT_EQ(r.next_change(), t0 + 4600s);

    // The lamp expires before the boundary is cached, so a signal at the expiry time does not
    // leave a boundary that is already past (R7).
    r.on_signal(signal::WorkspaceSwitched{1, 2}, t0 + 4600s, kShown);
    EXPECT_TRUE(r.status().lamps.empty());
    EXPECT_EQ(r.next_change(), t0 + 4600s + 120ms);
    r.update(t0 + 4600s + 120ms, kShown);
    EXPECT_EQ(r.next_change(), t0 + 4600s + 240ms);
    r.update(t0 + 4600s + 360ms, kShown);
    EXPECT_FALSE(r.next_change().has_value());

    // The same in `update`.
    Reactions u = started();
    u.on_signal(signal::AgentWaiting{"s1", "w1"}, t0, kShown);
    u.update(t0 + 3600s, kShown);
    EXPECT_TRUE(u.status().lamps.empty());
    EXPECT_FALSE(u.next_change().has_value());
}

TEST(Reactions, NotificationsFlashUnlessTheirAppHasFocus) {
    Reactions r = started();
    r.on_signal(zapzap(), t0, kShown);
    EXPECT_EQ(r.layers(t0).transient, everywhere(Paint{kNote}));
    EXPECT_EQ(r.status().playing, "notify");

    r.on_signal(signal::FocusChanged{"w1", "com.rtosta.zapzap"}, t0 + 1s, kShown);
    r.update(t0 + 1s, kShown);
    r.on_signal(zapzap(), t0 + 1s, kShown);
    EXPECT_TRUE(empty(r.layers(t0 + 1s).transient));
    EXPECT_FALSE(r.status().playing.has_value());

    // Critical ones have their own color.
    r.on_signal(signal::Notified{"other", "org.other.app", Urgency::Critical}, t0 + 1s, kShown);
    EXPECT_EQ(r.layers(t0 + 1s).transient, everywhere(Paint{kCritical}));
}

TEST(Reactions, WindowsLiftAndDip) {
    Reactions r = started();
    r.on_signal(signal::WindowOpened{}, t0, kShown);
    EXPECT_EQ(r.layers(t0).transient, everywhere(Mix{kWhite, 0.5}));
    EXPECT_EQ(r.status().playing, "window");
    r.update(t0 + 120ms, kShown);
    EXPECT_TRUE(empty(r.layers(t0 + 120ms).transient));

    r.on_signal(signal::WindowClosed{}, t0 + 1s, kShown);
    EXPECT_EQ(r.layers(t0 + 1s).transient, everywhere(Scale{0.35}));
}

TEST(Reactions, StatusListsLampsAndThePlayingChannel) {
    Reactions r = started();
    EXPECT_EQ(r.status(), (ReactionsStatus{true, {}, std::nullopt}));

    r.on_signal(signal::UrgentRaised{"a"}, t0, kShown);
    r.on_signal(signal::AgentWaiting{"s1", "b"}, t0, kShown);
    r.on_signal(signal::WorkspaceSwitched{1, 2}, t0, kShown);
    EXPECT_EQ(r.status(), (ReactionsStatus{true, {"agent", "urgent"}, "workspace"})); // Agent first

    r.update(t0 + 400ms, kShown);
    EXPECT_EQ(r.status(), (ReactionsStatus{true, {"agent", "urgent"}, std::nullopt}));

    r.on_signal(zapzap(), t0 + 1s, kShown);
    EXPECT_EQ(r.status().playing, "notify");
    r.update(t0 + 2s, kShown);
    r.on_signal(signal::WindowOpened{}, t0 + 3s, kShown);
    EXPECT_EQ(r.status().playing, "window");
    r.update(t0 + 4s, kShown);
    r.on_signal(signal::AgentDone{"s2", std::nullopt}, t0 + 5s, kShown);
    EXPECT_EQ(r.status().playing, "agent-done");
}

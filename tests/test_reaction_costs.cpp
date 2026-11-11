// The write-count contract (spec section 2's table, section 6): what each reaction costs in
// keyboard writes, counted the way the writer counts them. Each test drives a fresh rig on the
// static profile `plain` unless it says otherwise.

#include "reaction_rig.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

using namespace omenkbd;
using namespace omenkbd::test;
using namespace std::chrono_literals;

namespace {

// The colors of kRigConfig.
constexpr Rgb kL{0x10, 0x10, 0x10};
constexpr Rgb kM{0x20, 0x20, 0x20};
constexpr Rgb kR{0x30, 0x30, 0x30};
constexpr Rgb kW{0x40, 0x40, 0x40};
constexpr Frame kBase{kL, kM, kR, kW};
constexpr Rgb kSweep{0xa0, 0xa0, 0xff};
constexpr Rgb kZapZap{0x20, 0xff, 0x40};
constexpr Rgb kUrgent{0xff, 0xe0, 0x20};
constexpr Rgb kWaiting{0xff, 0x90, 0x40};
constexpr Rgb kDone{0x40, 0xff, 0x90};

/// Window addresses as Hyprland gives them after normalizing (R8).
constexpr std::string_view kTerm = "5a5a01";
constexpr std::string_view kOther = "5a5a02";

/// `frame` with `zones` painted `color`.
Frame paint(Frame frame, std::initializer_list<Zone> zones, Rgb color) {
    for (Zone z : zones) {
        frame[index(z)] = color;
    }
    return frame;
}

Frame all(Rgb c) { return {c, c, c, c}; }

std::vector<Frame> frames(std::initializer_list<Frame> list) { return list; }

std::vector<Duration> times(std::initializer_list<Duration> list) { return list; }

signal::Notified zapzap() {
    return signal::Notified{"ZapZap", "com.rtosta.zapzap", Urgency::Normal};
}

std::string str(std::string_view s) { return std::string(s); }

/// kRigConfig with a `[gates]` table appended.
std::string with_gates(std::string_view gates) {
    return std::string(kRigConfig) + "\n[gates]\n" + std::string(gates);
}

/// `text` with its one `from` replaced by `to`.
std::string replaced(std::string_view text, std::string_view from, std::string_view to) {
    std::string out(text);
    const std::size_t at = out.find(from);
    EXPECT_NE(at, std::string::npos) << from;
    if (at != std::string::npos) {
        out.replace(at, from.size(), to);
    }
    return out;
}

/// `frame` with every zone moved half way toward white, as a window opening lifts it.
Frame lifted(Frame frame) {
    for (Rgb &z : frame) {
        z = Rgb::lerp(z, kWhite, 0.5);
    }
    return frame;
}

/// The frames of one upward sweep over `kBase`: left with wasd, middle, right, then the base.
std::vector<Frame> sweep_frames() {
    return frames({paint(kBase, {Zone::Left, Zone::Wasd}, kSweep),
                   paint(kBase, {Zone::Middle}, kSweep), paint(kBase, {Zone::Right}, kSweep),
                   kBase});
}

} // namespace

TEST(ReactionCosts, WorkspaceSweepCostsFour) {
    ReactionRig rig;
    rig.signal(signal::WorkspaceSwitched{1, 2});
    rig.run_for(2s);
    EXPECT_EQ(rig.writes(), 4u);
    EXPECT_EQ(rig.frames(), sweep_frames());
    EXPECT_EQ(rig.frame_times(), times({0ms, 120ms, 240ms, 360ms}));
}

TEST(ReactionCosts, FiveFastSwitchesCostFour) {
    ReactionRig rig;
    for (int i = 0; i < 5; ++i) { // at 0, 20, 40, 60 and 80 ms
        rig.signal(signal::WorkspaceSwitched{i + 1, i + 2});
        rig.run_for(20ms);
    }
    rig.run_for(2s);
    EXPECT_EQ(rig.writes(), 4u) << "switches during a sweep are absorbed";
    EXPECT_EQ(rig.frames(), sweep_frames());
    EXPECT_EQ(rig.frame_times(), times({0ms, 120ms, 240ms, 360ms}));
}

TEST(ReactionCosts, WindowOpenOrCloseCostsTwo) {
    ReactionRig open;
    open.signal(signal::WindowOpened{});
    open.run_for(2s);
    EXPECT_EQ(open.writes(), 2u);
    EXPECT_EQ(open.frames(), frames({lifted(kBase), kBase})) << "50% toward white, then back";
    EXPECT_EQ(open.frame_times(), times({0ms, 120ms}));

    ReactionRig close;
    close.signal(signal::WindowClosed{});
    close.run_for(2s);
    EXPECT_EQ(close.writes(), 2u);
    const Frame dipped{multiply(kL, 0.35), multiply(kM, 0.35), multiply(kR, 0.35),
                       multiply(kW, 0.35)};
    EXPECT_EQ(close.frames(), frames({dipped, kBase})) << "down to 35%, then back";
    EXPECT_EQ(close.frame_times(), times({0ms, 120ms}));
}

TEST(ReactionCosts, WindowStormCostsAtMostFour) {
    ReactionRig rig;
    for (int i = 0; i < 20; ++i) { // 20 opens in 1 s
        rig.signal(signal::WindowOpened{});
        rig.run_for(50ms);
    }
    rig.run_for(2s);
    // One pulse per 500 ms: the opens at 0 and 500 ms play, the rest are dropped.
    EXPECT_EQ(rig.writes(), 4u);
    EXPECT_EQ(rig.frame_times(), times({0ms, 120ms, 500ms, 620ms}));
    EXPECT_EQ(rig.frames().back(), kBase);
}

TEST(ReactionCosts, NotifyFlashCostsTwo) {
    ReactionRig rig;
    rig.signal(zapzap());
    rig.run_for(2s);
    EXPECT_EQ(rig.writes(), 2u);
    EXPECT_EQ(rig.frames(), frames({all(kZapZap), kBase}));
    EXPECT_EQ(rig.frame_times(), times({0ms, 150ms}));
}

TEST(ReactionCosts, NotifyBurstCostsFour) {
    ReactionRig rig;
    for (int i = 0; i < 10; ++i) { // 10 in 200 ms
        rig.signal(zapzap());
        rig.run_for(20ms);
    }
    rig.run_for(2s);
    // The first plays, the second waits in the queue and starts a blink after the first ends,
    // the other eight are dropped: on, off, on, off.
    EXPECT_EQ(rig.writes(), 4u);
    EXPECT_EQ(rig.frames(), frames({all(kZapZap), kBase, all(kZapZap), kBase}));
    EXPECT_EQ(rig.frame_times(), times({0ms, 150ms, 300ms, 450ms}));
}

TEST(ReactionCosts, AttentionLightCostsFourPerEpisode) {
    ReactionRig rig;
    const Frame lit = paint(kBase, {Zone::Right}, kUrgent);
    rig.signal(signal::UrgentRaised{str(kOther)});
    rig.run_for(2s);
    EXPECT_EQ(rig.writes(), 3u) << "on, off, on";
    EXPECT_EQ(rig.frames(), frames({lit, kBase, lit}));
    EXPECT_EQ(rig.frame_times(), times({0ms, 150ms, 300ms}));

    rig.signal(signal::FocusChanged{str(kOther), "kitty"});
    rig.run_for(2s);
    EXPECT_EQ(rig.writes(), 4u);
    EXPECT_EQ(rig.frames().back(), kBase);
}

TEST(ReactionCosts, MoreUrgentWindowsAddNothingWhileLit) {
    ReactionRig rig;
    rig.signal(signal::UrgentRaised{"a1"});
    rig.run_for(2s);
    EXPECT_EQ(rig.writes(), 3u);

    rig.signal(signal::UrgentRaised{"b2"});
    rig.run_for(2s);
    EXPECT_EQ(rig.writes(), 3u) << "the light is already on";

    rig.signal(signal::FocusChanged{"a1", "kitty"});
    rig.run_for(2s);
    EXPECT_EQ(rig.writes(), 3u) << "b2 still asks for attention";

    rig.signal(signal::FocusChanged{"b2", "kitty"});
    rig.run_for(2s);
    EXPECT_EQ(rig.writes(), 4u);
    EXPECT_EQ(rig.frames().back(), kBase);
    EXPECT_EQ(rig.frame_times(), times({0ms, 150ms, 300ms, 6000ms}));
}

TEST(ReactionCosts, LampHeldForSixtySecondsAddsNothing) {
    // Both lamps: the attention light has no TTL, the Claude lamp's is an hour. Each kind blinks
    // its own intro.
    ReactionRig rig;
    rig.signal(signal::UrgentRaised{"a1"});
    rig.run_for(2s);
    rig.signal(signal::AgentWaiting{"s1", std::nullopt});
    rig.run_for(2s);
    const Frame lit = paint(kBase, {Zone::Right}, kUrgent);
    const Frame both = paint(lit, {Zone::Wasd}, kWaiting);
    ASSERT_EQ(rig.frames(), frames({lit, kBase, lit, both, lit, both}));
    ASSERT_EQ(rig.frame_times(), times({0ms, 150ms, 300ms, 2000ms, 2150ms, 2300ms}));

    rig.run_for(60s);
    EXPECT_EQ(rig.writes(), 6u);
    EXPECT_EQ(rig.frames().back(), both);
    // `run_for` took no wake in those 60 s, so sample once: a focus on an unrelated window
    // recomputes, and the frame still shows both lamps (a lamp that had expired would write).
    rig.signal(signal::FocusChanged{"5a5a99", "kitty"});
    EXPECT_EQ(rig.writes(), 6u) << "both lamps still lit after 60 s";
}

TEST(ReactionCosts, ClaudeWaitCostsFourAndFinishCostsTwo) {
    const Frame waiting = paint(kBase, {Zone::Wasd}, kWaiting);
    const Frame done = paint(kBase, {Zone::Wasd}, kDone);

    ReactionRig wait;
    wait.signal(signal::AgentWaiting{"s1", std::nullopt});
    wait.run_for(2s);
    EXPECT_EQ(wait.writes(), 3u);
    EXPECT_EQ(wait.frames(), frames({waiting, kBase, waiting}));
    wait.signal(signal::AgentWorking{"s1"});
    wait.run_for(2s);
    EXPECT_EQ(wait.writes(), 4u);
    EXPECT_EQ(wait.frames().back(), kBase);
    EXPECT_EQ(wait.frame_times(), times({0ms, 150ms, 300ms, 2000ms}));

    ReactionRig finish;
    finish.signal(signal::AgentDone{"s1", std::nullopt});
    finish.run_for(2s);
    EXPECT_EQ(finish.writes(), 2u);
    EXPECT_EQ(finish.frames(), frames({done, kBase}));
    EXPECT_EQ(finish.frame_times(), times({0ms, 150ms}));

    // A finish that ends a wait costs two as well: the flash replaces the lamp in one frame.
    ReactionRig both;
    both.signal(signal::AgentWaiting{"s1", std::nullopt});
    both.run_for(2s);
    both.signal(signal::AgentDone{"s1", std::nullopt});
    both.run_for(2s);
    EXPECT_EQ(both.writes(), 5u);
    EXPECT_EQ(both.frames(), frames({waiting, kBase, waiting, done, kBase}));
    EXPECT_EQ(both.frame_times(), times({0ms, 150ms, 300ms, 2000ms, 2150ms}));
}

TEST(ReactionCosts, ClaudeLampWindowRules) {
    ReactionRig rig;
    const auto step = [&rig](const Signal &s) {
        rig.signal(s);
        rig.run_for(2s);
        return rig.writes();
    };
    // Waiting while its terminal has focus: nothing to show.
    EXPECT_EQ(step(signal::FocusChanged{str(kTerm), "alacritty"}), 0u);
    EXPECT_EQ(step(signal::AgentWaiting{"s1", str(kTerm)}), 0u);

    // Waiting in another window lights (on, off, on); focusing that window clears it.
    EXPECT_EQ(step(signal::AgentWaiting{"s2", str(kOther)}), 3u);
    EXPECT_EQ(step(signal::FocusChanged{str(kOther), "alacritty"}), 4u);
    EXPECT_EQ(rig.frames().back(), kBase);

    // An agent light on kOther absorbs an urgent raise from the same window.
    EXPECT_EQ(step(signal::FocusChanged{str(kTerm), "alacritty"}), 4u);
    EXPECT_EQ(step(signal::AgentWaiting{"s3", str(kOther)}), 7u);
    EXPECT_EQ(step(signal::UrgentRaised{str(kOther)}), 7u);
    EXPECT_EQ(rig.frames().back(), paint(kBase, {Zone::Wasd}, kWaiting)) << "Right stays dark";

    // A session finishing in the focused terminal does not flash.
    EXPECT_EQ(step(signal::AgentDone{"s1", str(kTerm)}), 7u);

    // Each step is 2 s: s2 lights at 4 s, its window's focus clears it at 6 s, s3 lights at 10 s.
    EXPECT_EQ(rig.frame_times(),
              times({4000ms, 4150ms, 4300ms, 6000ms, 10000ms, 10150ms, 10300ms}));
}

// Task 11 minor 4: an empty window string on AgentWaiting or AgentDone is no window, even while
// an empty workspace (focus "") has focus: the lamp lights and stays, and the flash plays.
TEST(ReactionCosts, AnEmptyAgentWindowIsNoWindow) {
    const Frame waiting = paint(kBase, {Zone::Wasd}, kWaiting);
    const Frame done = paint(kBase, {Zone::Wasd}, kDone);
    ReactionRig rig;
    rig.signal(signal::FocusChanged{"", ""});
    rig.signal(signal::AgentWaiting{"s1", ""});
    rig.run_for(2s);
    EXPECT_EQ(rig.frames(), frames({waiting, kBase, waiting}));
    rig.signal(signal::FocusChanged{"", ""});
    rig.run_for(2s);
    EXPECT_EQ(rig.writes(), 3u) << "focusing an empty workspace leaves the lamp lit";

    rig.signal(signal::AgentDone{"s1", ""});
    rig.run_for(2s);
    EXPECT_EQ(rig.frames(), frames({waiting, kBase, waiting, done, kBase}));
    EXPECT_EQ(rig.frame_times(), times({0ms, 150ms, 300ms, 4000ms, 4150ms}));
}

// Ruling P7: an agent raise drops the urgent light of its window in the frame that lights WASD.
TEST(ReactionCosts, UrgentThenAgentWaitingForTheSameWindowCostsSeven) {
    const Frame urgent = paint(kBase, {Zone::Right}, kUrgent);
    const Frame waiting = paint(kBase, {Zone::Wasd}, kWaiting);

    ReactionRig rig;
    rig.signal(signal::UrgentRaised{str(kOther)});
    rig.run_for(6s);
    EXPECT_EQ(rig.writes(), 3u);
    rig.signal(signal::AgentWaiting{"s1", str(kOther)});
    rig.run_for(2s);
    EXPECT_EQ(rig.writes(), 6u);
    rig.signal(signal::FocusChanged{str(kOther), "alacritty"});
    rig.run_for(2s);
    EXPECT_EQ(rig.writes(), 7u);
    EXPECT_EQ(rig.frames(), frames({urgent, kBase, urgent, waiting, kBase, waiting, kBase}));
    EXPECT_EQ(rig.frame_times(), times({0ms, 150ms, 300ms, 6000ms, 6150ms, 6300ms, 8000ms}));

    // The session goes back to work instead: the urgent light was dropped, not parked, so only the
    // WASD light goes out.
    ReactionRig working;
    working.signal(signal::UrgentRaised{str(kOther)});
    working.run_for(6s);
    working.signal(signal::AgentWaiting{"s1", str(kOther)});
    working.run_for(2s);
    working.signal(signal::AgentWorking{"s1"});
    working.run_for(2s);
    EXPECT_EQ(working.writes(), 7u);
    EXPECT_EQ(working.frames(), frames({urgent, kBase, urgent, waiting, kBase, waiting, kBase}));
}

TEST(ReactionCosts, FullscreenInAndOutCostTwo) {
    ReactionRig rig;
    rig.fact(fact::FullscreenApp{"mpv"});
    EXPECT_EQ(rig.writes(), 1u);
    EXPECT_EQ(rig.frames().back(), (Frame{kL.scale(15), kM.scale(15), kR.scale(15), kW.scale(15)}));
    rig.fact(fact::FullscreenApp{"mpv"}); // Hyprland repeats its fullscreen events
    rig.run_for(2s);
    EXPECT_EQ(rig.writes(), 1u);
    rig.fact(fact::FullscreenApp{std::nullopt});
    rig.run_for(2s);
    EXPECT_EQ(rig.writes(), 2u);
    EXPECT_EQ(rig.frames().back(), kBase);
}

TEST(ReactionCosts, ReactionsAreSilentWhileLockedIdleOrOff) {
    const auto short_reactions = [](ReactionRig &rig) {
        rig.signal(signal::WorkspaceSwitched{1, 2});
        rig.signal(signal::WindowOpened{});
        rig.signal(signal::WindowClosed{});
        rig.signal(zapzap());
        rig.signal(signal::AgentDone{"s9", std::nullopt});
        rig.run_for(2s);
    };

    // The gates keep the keyboard lit, but nobody is there: short reactions are dropped, and a lamp
    // raised now lights steady.
    for (const bool idle : {false, true}) {
        ReactionRig rig(with_gates("on_lock = \"keep\"\non_idle = \"keep\"\n"));
        if (idle) {
            rig.request(req::Idle{true});
        } else {
            rig.fact(fact::Locked{true});
        }
        EXPECT_EQ(rig.writes(), 0u) << "idle " << idle << ": keep changes nothing";
        short_reactions(rig);
        EXPECT_EQ(rig.writes(), 0u) << "idle " << idle;
        rig.signal(signal::UrgentRaised{"a1"});
        rig.run_for(2s);
        EXPECT_EQ(rig.writes(), 1u) << "idle " << idle << ": steady, no blink";
        EXPECT_EQ(rig.frames(), frames({paint(kBase, {Zone::Right}, kUrgent)})) << "idle " << idle;
    }

    // The keyboard goes dark: one write for that, then nothing.
    const auto silent_after = [&short_reactions](ReactionRig &rig, std::string_view what) {
        EXPECT_EQ(rig.writes(), 1u) << what << ": the transition";
        short_reactions(rig);
        rig.signal(signal::UrgentRaised{"a1"});
        rig.signal(signal::AgentWaiting{"s1", std::nullopt});
        rig.run_for(2s);
        EXPECT_EQ(rig.writes(), 1u) << what;
    };
    ReactionRig locked;
    locked.fact(fact::Locked{true});
    silent_after(locked, "locked, on_lock = off");
    ReactionRig idle(with_gates("on_idle = \"off\"\n"));
    idle.request(req::Idle{true});
    silent_after(idle, "idle, on_idle = off");
    ReactionRig off;
    off.request(req::Power{req::PowerAction::Off});
    silent_after(off, "turned off");
    ReactionRig zero;
    zero.request(req::Brightness{"0"});
    silent_after(zero, "brightness 0");
}

TEST(ReactionCosts, ReactionsOnAnAnimatedBaseAddNoWrites) {
    ReactionRig plain;
    plain.request(req::Profile{"wavy"});
    ReactionRig swept;
    swept.request(req::Profile{"wavy"});
    // Ruling P16: while the effect ticks, a signal is applied without a frame of its own (before,
    // it was delivered with one, the spec's "± 1" of section 6). The next tick samples it.
    swept.signal(signal::WorkspaceSwitched{1, 2});
    plain.run_for(1s);
    swept.run_for(1s);

    // The switch to `wavy` at 0, then a tick every 100 ms: a cycle frame changes every tick.
    EXPECT_EQ(plain.writes(), 11u);
    EXPECT_EQ(swept.writes(), plain.writes());
    EXPECT_EQ(swept.frame_times(), plain.frame_times());

    // And the ticks show the sweep, each step up to one tick late: left with wasd at the 100 ms
    // tick, middle at 200, right at 300, gone at 400.
    const std::vector<Frame> &f = swept.frames();
    ASSERT_EQ(f.size(), 11u);
    EXPECT_EQ(f[1][index(Zone::Left)], kSweep);
    EXPECT_EQ(f[1][index(Zone::Wasd)], kSweep);
    EXPECT_EQ(f[2][index(Zone::Middle)], kSweep);
    EXPECT_EQ(f[3][index(Zone::Right)], kSweep);
    EXPECT_EQ(f[4], plain.frames()[4]);
}

TEST(ReactionCosts, SignalsBetweenTicksAddNoWrites) {
    ReactionRig plain;
    plain.request(req::Profile{"wavy"});
    plain.run_for(1s);
    ASSERT_EQ(plain.writes(), 11u) << "the switch to `wavy` at 0, then a tick every 100 ms";

    // 20 window opens 50 ms apart, half of them between two ticks.
    ReactionRig storm;
    storm.request(req::Profile{"wavy"});
    for (int i = 0; i < 20; ++i) {
        storm.signal(signal::WindowOpened{});
        storm.run_for(50ms);
    }
    EXPECT_EQ(storm.writes(), plain.writes());
    EXPECT_EQ(storm.frame_times(), plain.frame_times());
    // The opens at 0 and 500 ms play (the cooldown drops the rest), each seen at the next tick.
    ASSERT_EQ(storm.frames().size(), 11u);
    for (std::size_t k = 0; k < 11; ++k) {
        const bool pulse = k == 1 || k == 6;
        EXPECT_EQ(storm.frames()[k], pulse ? lifted(plain.frames()[k]) : plain.frames()[k])
            << "frame " << k;
    }

    // 20 focus changes 50 ms apart change nothing anyone sees, and add no write.
    ReactionRig focus;
    focus.request(req::Profile{"wavy"});
    for (int i = 0; i < 20; ++i) {
        focus.signal(signal::FocusChanged{str(i % 2 == 0 ? kTerm : kOther), "alacritty"});
        focus.run_for(50ms);
    }
    EXPECT_EQ(focus.frames(), plain.frames());
    EXPECT_EQ(focus.frame_times(), plain.frame_times());
}

TEST(ReactionCosts, RateLimitCapsAStormOverAMinute) {
    ReactionRig rig;
    int clips = 0;
    for (int i = 0; i < 100; ++i) { // every 600 ms, past the 500 ms window cooldown
        if (i % 2 == 0) {
            rig.signal(signal::WindowOpened{});
        } else {
            rig.signal(signal::WindowClosed{});
        }
        if (rig.model().status(nullptr).reactions.playing == "window") {
            ++clips;
        }
        rig.run_for(600ms);
    }
    EXPECT_LE(clips, 30);
    EXPECT_LE(rig.writes(), 60u);
    // Exactly at the bound: the rate limit is what stops the storm.
    EXPECT_EQ(clips, 30);
    EXPECT_EQ(rig.writes(), 60u);
}

TEST(ReactionCosts, ADarkKeyboardCostsNothing) {
    ReactionRig rig;
    rig.set_hw_backlight(false); // the Fn key, as last read
    EXPECT_EQ(rig.writes(), 0u);
    rig.signal(signal::WindowOpened{});
    rig.run_for(2s);
    EXPECT_EQ(rig.writes(), 0u) << "WindowOpened";
    rig.signal(signal::UrgentRaised{"a1"});
    rig.run_for(2s);
    EXPECT_EQ(rig.writes(), 0u) << "UrgentRaised";
    rig.signal(signal::AgentWaiting{"s1", std::nullopt});
    rig.run_for(2s);
    EXPECT_EQ(rig.writes(), 0u) << "AgentWaiting";

    // The backlight is back: both lamps show steady, with no blink.
    rig.set_hw_backlight(true);
    rig.run_for(2s);
    EXPECT_EQ(rig.writes(), 1u);
    EXPECT_EQ(rig.frames(),
              frames({paint(paint(kBase, {Zone::Right}, kUrgent), {Zone::Wasd}, kWaiting)}));
}

// Ruling P14 (b): a reload that shortens the lamp's TTL moves the deadline the loop sleeps until.
TEST(ReactionCosts, AReloadThatShortensTheTtlClearsTheLampAtTheNewExpiry) {
    ReactionRig rig;
    const TimePoint raised = rig.now();
    rig.signal(signal::AgentWaiting{"s1", std::nullopt});
    rig.run_for(2s);
    ASSERT_EQ(rig.writes(), 3u);
    ASSERT_EQ(rig.model().reaction_deadline(), raised + 3600s);

    rig.reload(replaced(kRigConfig, "ttl_s = 3600", "ttl_s = 60"));
    EXPECT_EQ(rig.writes(), 3u) << "the lamp keeps its key and its look";
    EXPECT_EQ(rig.model().reaction_deadline(), raised + 60s);
    rig.run_for(10min);
    EXPECT_EQ(rig.writes(), 4u);
    EXPECT_EQ(rig.frames().back(), kBase);
    EXPECT_EQ(rig.frame_times().back(), 60s) << "cleared at the new expiry, not at the end";
}

// Ruling P17: a lamp is held while the Fn key has the backlight off; `toggle` or `on` brings the
// backlight back, and the lamp shows steady in one write, with no other event.
TEST(ReactionCosts, TheFnKeyThenPowerOnShowsTheHeldLampInOneWrite) {
    const Frame lit = paint(kBase, {Zone::Right}, kUrgent);
    for (const req::PowerAction action : {req::PowerAction::Toggle, req::PowerAction::On}) {
        const bool on = action == req::PowerAction::On;
        ReactionRig rig;
        rig.signal(signal::UrgentRaised{"a1"});
        rig.run_for(2s);
        ASSERT_EQ(rig.frames(), frames({lit, kBase, lit})) << "on " << on;

        // `toggle` reads the backlight first (`needs_backlight_read`); `on` reads nothing, so for
        // it this stands for an earlier read such as `status` (a static profile has no periodic
        // probe: those run only while an effect animates). Either way the reading is off, after
        // the Fn key, and from then on the lamp is not drawn.
        rig.set_hw_backlight(false);
        EXPECT_EQ(rig.frames().back(), kBase) << "on " << on;
        const std::size_t dark = rig.writes();

        // The forced write is composed with that reading, which the daemon forgets only after it.
        // The deadline says a frame is due, and that frame shows the lamp.
        rig.request(req::Power{action});
        rig.run_for(2s);
        EXPECT_EQ(rig.writes(), dark + 1) << "on " << on;
        EXPECT_EQ(rig.frames().back(), lit) << "on " << on;
        EXPECT_EQ(rig.frame_times().back(), 2000ms) << "on " << on << ": at once, no blink";
    }
}

// Ruling P17: the lamps are hidden while the write budget is spent, and come back when a write
// succeeds again. The daemon recomputes on neither writer outcome: the deadline wakes it.
TEST(ReactionCosts, ASpentBudgetHidesTheLampsUntilAWriteSucceeds) {
    const Frame lit = paint(kBase, {Zone::Right}, kUrgent);
    ReactionRig rig;
    rig.signal(signal::UrgentRaised{"a1"});
    rig.run_for(2s);
    ASSERT_EQ(rig.writes(), 3u);

    rig.model().record_write(std::unexpected(BackendError{BackendError::Kind::Budget, ""}));
    rig.run_for(1s);
    // The frame without the lamp is sent at once (the backend refuses it while the budget lasts).
    EXPECT_EQ(rig.frames().back(), kBase);
    EXPECT_EQ(rig.frame_times().back(), 2000ms);

    rig.model().record_write(true);
    rig.run_for(1s);
    EXPECT_EQ(rig.writes(), 5u);
    EXPECT_EQ(rig.frames().back(), lit);
    EXPECT_EQ(rig.frame_times().back(), 3000ms) << "at once, with no other event";
}

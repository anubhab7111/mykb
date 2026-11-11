#include "core/lamps.hpp"

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <format>
#include <gtest/gtest.h>
#include <initializer_list>
#include <optional>
#include <string>
#include <utility>
#include <vector>

using namespace omenkbd;
using namespace std::chrono_literals;

namespace {

constexpr Rgb kAgent{10, 200, 90};
constexpr Rgb kUrgent{230, 40, 40};

const TimePoint t0 = Clock::now();

/// Agent: WASD, two lit phases of 150 ms, gone an hour after the last raise of its key.
LampStyle agent_style() { return LampStyle{{Zone::Wasd}, kAgent, 2, 150ms, 1h}; }

/// Urgent: the three big zones, two lit phases of 150 ms, never expires.
LampStyle urgent_style() {
    return LampStyle{{Zone::Left, Zone::Middle, Zone::Right}, kUrgent, 2, 150ms, std::nullopt};
}

LampSet styled() {
    LampSet lamps;
    lamps.set_style(LampKind::Agent, agent_style());
    lamps.set_style(LampKind::Urgent, urgent_style());
    return lamps;
}

/// A layer that paints `color` on `zones` and leaves the others alone.
LayerOps lit(Rgb color, std::initializer_list<Zone> zones) {
    LayerOps ops;
    for (Zone z : zones) {
        ops[index(z)] = Paint{color};
    }
    return ops;
}

std::vector<LampKind> kinds(std::initializer_list<LampKind> list) { return list; }

} // namespace

TEST(LampSet, KindNames) {
    EXPECT_EQ(name(LampKind::Agent), "agent");
    EXPECT_EQ(name(LampKind::Urgent), "urgent");
}

TEST(LampSet, StartsEmpty) {
    const LampSet lamps = styled();
    EXPECT_TRUE(lamps.empty());
    EXPECT_TRUE(lamps.active().empty());
    EXPECT_TRUE(empty(lamps.layer(t0)));
    EXPECT_FALSE(lamps.next_change(t0).has_value());
}

TEST(LampSet, IntroIsOnOffOnThenSteady) {
    LampSet lamps = styled();
    const LayerOps on = lit(kAgent, {Zone::Wasd});
    lamps.raise(LampKind::Agent, "s1", std::nullopt, t0, true);
    EXPECT_EQ(lamps.layer(t0), on);
    EXPECT_EQ(lamps.layer(t0 + 149ms), on);
    EXPECT_TRUE(empty(lamps.layer(t0 + 150ms)));
    EXPECT_TRUE(empty(lamps.layer(t0 + 299ms)));
    EXPECT_EQ(lamps.layer(t0 + 300ms), on);
    EXPECT_EQ(lamps.layer(t0 + 449ms), on);
    EXPECT_EQ(lamps.layer(t0 + 450ms), on);
    EXPECT_EQ(lamps.layer(t0 + 10s), on);
    EXPECT_EQ(lamps.next_change(t0), t0 + 150ms);
    EXPECT_EQ(lamps.next_change(t0 + 150ms), t0 + 300ms);
    // The end of the last lit phase changes nothing: it is lit before and after. Only the TTL is
    // left.
    EXPECT_EQ(lamps.next_change(t0 + 300ms), t0 + 1h);
    EXPECT_EQ(lamps.next_change(t0 + 450ms), t0 + 1h);
}

TEST(LampSet, BlinksCountTheLitPhases) {
    LampSet lamps = styled();
    LampStyle style = agent_style();
    style.blinks = 3;
    lamps.set_style(LampKind::Agent, style);
    const LayerOps on = lit(kAgent, {Zone::Wasd});
    lamps.raise(LampKind::Agent, "s1", std::nullopt, t0, true);
    // ON OFF ON OFF ON, 150 ms each.
    EXPECT_EQ(lamps.layer(t0), on);
    EXPECT_TRUE(empty(lamps.layer(t0 + 150ms)));
    EXPECT_EQ(lamps.layer(t0 + 300ms), on);
    EXPECT_TRUE(empty(lamps.layer(t0 + 450ms)));
    EXPECT_EQ(lamps.layer(t0 + 600ms), on);
    EXPECT_EQ(lamps.layer(t0 + 750ms), on);
    EXPECT_EQ(lamps.next_change(t0 + 450ms), t0 + 600ms);
    EXPECT_EQ(lamps.next_change(t0 + 600ms), t0 + 1h);
}

TEST(LampSet, ALateSampleAfterTheIntroShowsSteady) {
    LampSet lamps = styled();
    const LayerOps on = lit(kAgent, {Zone::Wasd});
    lamps.raise(LampKind::Agent, "s1", std::nullopt, t0, true);
    // Nobody sampled for ten minutes (suspend, a stalled loop). The first sample is steady, and so
    // is every one after it: the blink is never replayed.
    const TimePoint late = t0 + 10min;
    EXPECT_EQ(lamps.layer(late), on);
    EXPECT_EQ(lamps.layer(late + 150ms), on);
    EXPECT_EQ(lamps.layer(late + 300ms), on);
    EXPECT_EQ(lamps.next_change(late), t0 + 1h);
    EXPECT_EQ(lamps.next_change(late + 150ms), t0 + 1h);
}

TEST(LampSet, ATimeBeforeTheRaiseShowsTheFirstStepOfTheIntro) {
    LampSet lamps = styled();
    lamps.raise(LampKind::Agent, "s1", std::nullopt, t0, true);
    // Never a negative time into the clip: a `now` earlier than the raise reads the first step.
    EXPECT_EQ(lamps.layer(t0 - 50ms), lit(kAgent, {Zone::Wasd}));
    EXPECT_EQ(lamps.next_change(t0 - 50ms), t0 + 150ms);
}

TEST(LampSet, OneBlinkOrZeroLightsSteadilyAtOnce) {
    for (int blinks : {0, 1}) {
        SCOPED_TRACE(blinks);
        LampSet lamps = styled();
        LampStyle style = agent_style();
        style.blinks = blinks;
        lamps.set_style(LampKind::Agent, style);
        const LayerOps on = lit(kAgent, {Zone::Wasd});
        lamps.raise(LampKind::Agent, "s1", std::nullopt, t0, true);
        EXPECT_EQ(lamps.layer(t0), on);
        EXPECT_EQ(lamps.layer(t0 + 75ms), on);
        EXPECT_EQ(lamps.layer(t0 + 150ms), on);
        EXPECT_EQ(lamps.layer(t0 + 10s), on);
        // No intro boundary: only the TTL is left.
        EXPECT_EQ(lamps.next_change(t0), t0 + 1h);
    }
}

TEST(LampSet, NoIntroWhenNotVisibleOrAlreadyLit) {
    const LayerOps on = lit(kAgent, {Zone::Wasd});
    {
        // Not visible: the caller asks for no intro.
        LampSet lamps = styled();
        lamps.raise(LampKind::Agent, "s1", std::nullopt, t0, false);
        EXPECT_EQ(lamps.layer(t0), on);
        EXPECT_EQ(lamps.layer(t0 + 150ms), on);
        EXPECT_EQ(lamps.next_change(t0), t0 + 1h);
        // A later raise with an intro does not start one: the lamp is already lit.
        lamps.raise(LampKind::Agent, "s2", std::nullopt, t0 + 1s, true);
        EXPECT_EQ(lamps.layer(t0 + 1s), on);
        EXPECT_EQ(lamps.layer(t0 + 1s + 150ms), on);
    }
    {
        // Already lit, in the middle of the intro: a second key neither restarts it nor ends it.
        LampSet lamps = styled();
        lamps.raise(LampKind::Agent, "s1", std::nullopt, t0, true);
        lamps.raise(LampKind::Agent, "s2", std::nullopt, t0 + 100ms, true);
        EXPECT_EQ(lamps.layer(t0 + 100ms), on);
        EXPECT_TRUE(empty(lamps.layer(t0 + 150ms)));
        EXPECT_EQ(lamps.layer(t0 + 300ms), on);
        EXPECT_EQ(lamps.next_change(t0 + 100ms), t0 + 150ms);
    }
}

TEST(LampSet, LampStaysLitUntilTheLastKeyIsCleared) {
    LampSet lamps = styled();
    const LayerOps on = lit(kAgent, {Zone::Wasd});
    lamps.raise(LampKind::Agent, "s1", std::nullopt, t0, false);
    lamps.raise(LampKind::Agent, "s2", std::nullopt, t0, false);
    EXPECT_FALSE(lamps.empty());

    lamps.clear(LampKind::Agent, "s1");
    EXPECT_EQ(lamps.layer(t0 + 1min), on);
    EXPECT_EQ(lamps.active(), kinds({LampKind::Agent}));
    EXPECT_FALSE(lamps.empty());

    // A key that was never raised, or one of the other kind, changes nothing.
    lamps.clear(LampKind::Agent, "nope");
    lamps.clear(LampKind::Urgent, "s2");
    EXPECT_EQ(lamps.layer(t0 + 1min), on);

    lamps.clear(LampKind::Agent, "s2");
    EXPECT_TRUE(empty(lamps.layer(t0 + 1min)));
    EXPECT_TRUE(lamps.active().empty());
    EXPECT_TRUE(lamps.empty());
    EXPECT_FALSE(lamps.next_change(t0 + 1min).has_value());

    // Lit again from dark, the kind plays its intro again.
    const TimePoint again = t0 + 2min;
    lamps.raise(LampKind::Agent, "s3", std::nullopt, again, true);
    EXPECT_EQ(lamps.layer(again), on);
    EXPECT_TRUE(empty(lamps.layer(again + 150ms)));
    EXPECT_EQ(lamps.layer(again + 300ms), on);
}

TEST(LampSet, ClearKindDarkensOnlyThatKind) {
    LampSet lamps = styled();
    lamps.raise(LampKind::Agent, "a1", std::nullopt, t0, false);
    lamps.raise(LampKind::Agent, "a2", "w1", t0, false);
    lamps.raise(LampKind::Urgent, "u1", std::nullopt, t0, false);
    lamps.clear_kind(LampKind::Agent);
    EXPECT_EQ(lamps.active(), kinds({LampKind::Urgent}));
    EXPECT_EQ(lamps.layer(t0), lit(kUrgent, {Zone::Left, Zone::Middle, Zone::Right}));
    lamps.clear_kind(LampKind::Urgent);
    EXPECT_TRUE(lamps.empty());
    EXPECT_TRUE(empty(lamps.layer(t0)));
}

TEST(LampSet, RaisingAnExistingKeyRefreshesTtlWithoutReplayingTheIntro) {
    LampSet lamps = styled();
    const LayerOps on = lit(kAgent, {Zone::Wasd});
    lamps.raise(LampKind::Agent, "s1", std::nullopt, t0, true);

    // Mid-intro: the intro goes on, it does not restart.
    lamps.raise(LampKind::Agent, "s1", std::nullopt, t0 + 100ms, true);
    EXPECT_TRUE(empty(lamps.layer(t0 + 150ms)));
    EXPECT_EQ(lamps.layer(t0 + 300ms), on);

    // Long after: no replay either, and the TTL counts from the new raise.
    const TimePoint later = t0 + 30min;
    lamps.raise(LampKind::Agent, "s1", std::nullopt, later, true);
    EXPECT_EQ(lamps.layer(later), on);
    EXPECT_EQ(lamps.layer(later + 150ms), on);
    EXPECT_EQ(lamps.next_change(later), later + 1h);

    lamps.expire(t0 + 1h);
    EXPECT_EQ(lamps.active(), kinds({LampKind::Agent}));
    lamps.expire(later + 1h - 1ms);
    EXPECT_EQ(lamps.active(), kinds({LampKind::Agent}));
    lamps.expire(later + 1h);
    EXPECT_TRUE(lamps.empty());
}

TEST(LampSet, AgentKeysExpireAfterTtlUrgentKeysDoNot) {
    LampSet lamps = styled();
    lamps.raise(LampKind::Agent, "a1", std::nullopt, t0, false);
    lamps.raise(LampKind::Urgent, "u1", std::nullopt, t0, false);

    lamps.expire(t0 + 1h - 1ms);
    EXPECT_EQ(lamps.active(), kinds({LampKind::Agent, LampKind::Urgent}));

    lamps.expire(t0 + 1h);
    EXPECT_EQ(lamps.active(), kinds({LampKind::Urgent}));
    EXPECT_EQ(lamps.layer(t0 + 1h), lit(kUrgent, {Zone::Left, Zone::Middle, Zone::Right}));
    // Nothing left that could change: the urgent key has no TTL.
    EXPECT_FALSE(lamps.next_change(t0 + 1h).has_value());

    lamps.expire(t0 + 1000h);
    EXPECT_EQ(lamps.active(), kinds({LampKind::Urgent}));
}

TEST(LampSet, NextChangeIsTheEarliestTtlExpiry) {
    LampSet lamps = styled();
    lamps.raise(LampKind::Agent, "a1", std::nullopt, t0, false);
    lamps.raise(LampKind::Agent, "a2", std::nullopt, t0 + 10min, false);
    EXPECT_EQ(lamps.next_change(t0 + 10min), t0 + 1h);
    lamps.expire(t0 + 1h);
    EXPECT_EQ(lamps.active(), kinds({LampKind::Agent}));
    EXPECT_EQ(lamps.next_change(t0 + 1h), t0 + 70min);
    lamps.expire(t0 + 70min);
    EXPECT_TRUE(lamps.empty());
    EXPECT_FALSE(lamps.next_change(t0 + 70min).has_value());
}

TEST(LampSet, NextChangeFollowsTheEarliestOfBothKinds) {
    LampSet lamps = styled();
    lamps.raise(LampKind::Agent, "a1", std::nullopt, t0, true);
    lamps.raise(LampKind::Urgent, "u1", std::nullopt, t0 + 50ms, true);
    // Agent intro changes at 150 and 300 ms, the urgent one at 200 and 350 ms.
    EXPECT_EQ(lamps.next_change(t0 + 50ms), t0 + 150ms);
    EXPECT_EQ(lamps.next_change(t0 + 150ms), t0 + 200ms);
    EXPECT_EQ(lamps.next_change(t0 + 200ms), t0 + 300ms);
    EXPECT_EQ(lamps.next_change(t0 + 300ms), t0 + 350ms);
    // Both lit steadily: the agent TTL is all that is left.
    EXPECT_EQ(lamps.next_change(t0 + 350ms), t0 + 1h);
}

TEST(LampSet, ClearWindowClearsEveryKeyLinkedToIt) {
    LampSet lamps = styled();
    lamps.raise(LampKind::Agent, "a1", "w1", t0, false);
    lamps.raise(LampKind::Agent, "a2", "w1", t0, false);
    lamps.raise(LampKind::Urgent, "u1", "w2", t0, false);
    lamps.raise(LampKind::Urgent, "u2", std::nullopt, t0, false);

    // No key is linked to w3.
    lamps.clear_window("w3");
    EXPECT_EQ(lamps.active(), kinds({LampKind::Agent, LampKind::Urgent}));

    // Every agent key of w1 goes; the urgent keys stay.
    lamps.clear_window("w1");
    EXPECT_EQ(lamps.active(), kinds({LampKind::Urgent}));
    EXPECT_EQ(lamps.layer(t0), lit(kUrgent, {Zone::Left, Zone::Middle, Zone::Right}));

    // The urgent key of w2 goes; a key linked to no window is not touched by any window.
    lamps.clear_window("w2");
    EXPECT_EQ(lamps.active(), kinds({LampKind::Urgent}));
    lamps.clear(LampKind::Urgent, "u2");
    EXPECT_TRUE(lamps.active().empty());
    EXPECT_TRUE(lamps.empty());
}

TEST(LampSet, AnAgentRaiseForAWindowThatHasAnUrgentKeyDropsIt) {
    LampSet lamps = styled();
    // The terminal's urgent signal comes first, the agent hook some seconds after it.
    lamps.raise(LampKind::Urgent, "u1", "w1", t0, true);
    EXPECT_EQ(lamps.active(), kinds({LampKind::Urgent}));
    const TimePoint later = t0 + 6s;
    lamps.raise(LampKind::Agent, "a1", "w1", later, true);

    // One light on the window: the agent one. The urgent zones show nothing, not even while the
    // agent lamp is dark in its intro.
    EXPECT_EQ(lamps.active(), kinds({LampKind::Agent}));
    EXPECT_EQ(lamps.layer(later), lit(kAgent, {Zone::Wasd}));
    EXPECT_TRUE(empty(lamps.layer(later + 150ms)));
    EXPECT_EQ(lamps.layer(later + 300ms), lit(kAgent, {Zone::Wasd}));
    EXPECT_EQ(lamps.next_change(later + 300ms), later + 1h);

    // Nothing is left lit once the agent key is cleared.
    lamps.clear(LampKind::Agent, "a1");
    EXPECT_TRUE(lamps.empty());
    EXPECT_TRUE(empty(lamps.layer(later + 1s)));
    EXPECT_FALSE(lamps.next_change(later + 1s).has_value());
}

TEST(LampSet, AnAgentRaiseLeavesUrgentKeysOfOtherWindowsAndWindowlessAgentRaisesAlone) {
    // The kinds lit after an agent key for `agent_window` meets an urgent key linked to
    // `urgent_window`, each pair in a set of its own.
    const auto lit_after = [](std::optional<std::string> urgent_window,
                              std::optional<std::string> agent_window) {
        LampSet lamps = styled();
        lamps.raise(LampKind::Urgent, "u", std::move(urgent_window), t0, false);
        lamps.raise(LampKind::Agent, "a", std::move(agent_window), t0, false);
        return lamps.active();
    };
    const std::vector<LampKind> both = kinds({LampKind::Agent, LampKind::Urgent});

    // The same window: the urgent key goes.
    EXPECT_EQ(lit_after("w1", "w1"), kinds({LampKind::Agent}));
    // Another window, or an urgent key with none: it stays.
    EXPECT_EQ(lit_after("w2", "w1"), both);
    EXPECT_EQ(lit_after(std::nullopt, "w1"), both);
    // An agent key with no window drops no urgent key, not even one with no window.
    EXPECT_EQ(lit_after("w1", std::nullopt), both);
    EXPECT_EQ(lit_after(std::nullopt, std::nullopt), both);

    // Every urgent key of the window goes, and the ones of other windows stay.
    LampSet lamps = styled();
    lamps.raise(LampKind::Urgent, "u1", "w1", t0, false);
    lamps.raise(LampKind::Urgent, "u2", "w1", t0, false);
    lamps.raise(LampKind::Urgent, "u3", "w2", t0, false);
    lamps.raise(LampKind::Agent, "a1", "w1", t0, false);
    EXPECT_EQ(lamps.active(), both);
    lamps.clear(LampKind::Urgent, "u3");
    EXPECT_EQ(lamps.active(), kinds({LampKind::Agent}));
}

TEST(LampSet, AnIgnoredAgentRaiseErasesNothing) {
    LampSet lamps = styled();
    LampStyle off = agent_style();
    off.zones.clear();
    lamps.set_style(LampKind::Agent, off);
    lamps.raise(LampKind::Urgent, "u1", "w1", t0, false);

    // The agent kind has no zones, so the raise is ignored and the urgent light stays.
    lamps.raise(LampKind::Agent, "a1", "w1", t0, true);
    EXPECT_EQ(lamps.active(), kinds({LampKind::Urgent}));
    EXPECT_EQ(lamps.layer(t0), lit(kUrgent, {Zone::Left, Zone::Middle, Zone::Right}));
}

TEST(LampSet, ReRaisingAnAgentKeyAfterAnAbsorbedUrgentRaiseChangesNothing) {
    LampSet lamps = styled();
    lamps.raise(LampKind::Agent, "a1", "w1", t0, false);
    lamps.raise(LampKind::Urgent, "u1", "w1", t0 + 1s, true);
    EXPECT_EQ(lamps.active(), kinds({LampKind::Agent}));

    lamps.raise(LampKind::Agent, "a1", "w1", t0 + 2s, true);
    EXPECT_EQ(lamps.active(), kinds({LampKind::Agent}));
    EXPECT_EQ(lamps.layer(t0 + 2s), lit(kAgent, {Zone::Wasd}));
    EXPECT_EQ(lamps.layer(t0 + 2s + 150ms), lit(kAgent, {Zone::Wasd}));
    // The re-raise refreshed the TTL and nothing else.
    EXPECT_EQ(lamps.next_change(t0 + 2s), t0 + 2s + 1h);
    lamps.clear(LampKind::Agent, "a1");
    EXPECT_TRUE(lamps.empty());
}

TEST(LampSet, UrgentRaiseForAWindowThatHasAnAgentKeyIsAbsorbed) {
    LampSet lamps = styled();
    lamps.raise(LampKind::Agent, "a1", "w1", t0, false);
    lamps.raise(LampKind::Urgent, "u1", "w1", t0, true);
    EXPECT_EQ(lamps.active(), kinds({LampKind::Agent}));
    EXPECT_EQ(lamps.layer(t0), lit(kAgent, {Zone::Wasd}));

    // Another window is not absorbed, and neither is a key without a window.
    lamps.raise(LampKind::Urgent, "u2", "w2", t0, false);
    EXPECT_EQ(lamps.active(), kinds({LampKind::Agent, LampKind::Urgent}));
    lamps.clear(LampKind::Urgent, "u2");
    lamps.raise(LampKind::Urgent, "u3", std::nullopt, t0, false);
    EXPECT_EQ(lamps.active(), kinds({LampKind::Agent, LampKind::Urgent}));
    lamps.clear(LampKind::Urgent, "u3");

    // The absorbed raise left nothing behind, and it did not use up the intro.
    lamps.clear(LampKind::Agent, "a1");
    EXPECT_TRUE(lamps.empty());
    lamps.raise(LampKind::Urgent, "u1", "w1", t0 + 1min, true);
    EXPECT_EQ(lamps.active(), kinds({LampKind::Urgent}));
    EXPECT_TRUE(empty(lamps.layer(t0 + 1min + 150ms)));
}

TEST(LampSet, AnAgentKindWithNoZonesDoesNotAbsorbUrgentRaises) {
    LampSet lamps = styled();
    lamps.raise(LampKind::Agent, "a1", "w1", t0, false);
    // Restyled to no zones: the agent key stays, but its lamp cannot show. It must not swallow the
    // attention light of its window.
    LampStyle off = agent_style();
    off.zones.clear();
    lamps.set_style(LampKind::Agent, off);
    ASSERT_EQ(lamps.key_count(LampKind::Agent), 1U);

    lamps.raise(LampKind::Urgent, "u1", "w1", t0 + 1s, false);
    EXPECT_EQ(lamps.active(), kinds({LampKind::Agent, LampKind::Urgent}));
    EXPECT_EQ(lamps.key_count(LampKind::Urgent), 1U);
    EXPECT_EQ(lamps.layer(t0 + 1s), lit(kUrgent, {Zone::Left, Zone::Middle, Zone::Right}));

    // The rule follows the style in force: with its zones back the agent lamp absorbs again.
    lamps.set_style(LampKind::Agent, agent_style());
    lamps.raise(LampKind::Urgent, "u2", "w1", t0 + 2s, false);
    EXPECT_EQ(lamps.key_count(LampKind::Urgent), 1U) << "u2 was absorbed";
}

TEST(LampSet, AgentWinsASharedZone) {
    LampSet lamps;
    LampStyle agent = agent_style();
    agent.zones = {Zone::Left, Zone::Wasd};
    LampStyle urgent = urgent_style();
    urgent.zones = {Zone::Left, Zone::Middle};
    lamps.set_style(LampKind::Agent, agent);
    lamps.set_style(LampKind::Urgent, urgent);
    lamps.raise(LampKind::Urgent, "u1", std::nullopt, t0, false);
    lamps.raise(LampKind::Agent, "a1", std::nullopt, t0, false);

    LayerOps expected;
    expected[index(Zone::Left)] = Paint{kAgent};
    expected[index(Zone::Middle)] = Paint{kUrgent};
    expected[index(Zone::Wasd)] = Paint{kAgent};
    EXPECT_EQ(lamps.layer(t0), expected);

    // The urgent light shows on the shared zone again once the agent light is gone.
    lamps.clear(LampKind::Agent, "a1");
    EXPECT_EQ(lamps.layer(t0), lit(kUrgent, {Zone::Left, Zone::Middle}));
}

TEST(LampSet, EmptyZonesDisableTheKind) {
    // A set nobody styled has no zones for either kind.
    LampSet unstyled;
    unstyled.raise(LampKind::Agent, "a1", std::nullopt, t0, true);
    unstyled.raise(LampKind::Urgent, "u1", "w1", t0, true);
    EXPECT_TRUE(unstyled.empty());
    EXPECT_TRUE(unstyled.active().empty());
    EXPECT_TRUE(empty(unstyled.layer(t0)));
    EXPECT_FALSE(unstyled.next_change(t0).has_value());

    // One kind disabled, the other working.
    LampSet lamps = styled();
    LampStyle off = agent_style();
    off.zones.clear();
    lamps.set_style(LampKind::Agent, off);
    lamps.raise(LampKind::Agent, "a1", "w1", t0, true);
    EXPECT_TRUE(lamps.empty());
    // The ignored agent raise holds no window, so it absorbs nothing.
    lamps.raise(LampKind::Urgent, "u1", "w1", t0, false);
    EXPECT_EQ(lamps.active(), kinds({LampKind::Urgent}));
    EXPECT_EQ(lamps.layer(t0), lit(kUrgent, {Zone::Left, Zone::Middle, Zone::Right}));
}

TEST(LampSet, RestyleKeepsKeys) {
    LampSet lamps = styled();
    lamps.raise(LampKind::Agent, "s1", "w1", t0, true);

    // Restyled in the middle of the intro, with new zones, color and TTL.
    LampStyle restyled{{Zone::Left, Zone::Right}, kUrgent, 2, 150ms, 10min};
    lamps.set_style(LampKind::Agent, restyled);
    const LayerOps on = lit(kUrgent, {Zone::Left, Zone::Right});
    EXPECT_EQ(lamps.active(), kinds({LampKind::Agent}));
    EXPECT_EQ(lamps.layer(t0 + 100ms), on);
    // The intro goes on where it was: it does not restart.
    EXPECT_TRUE(empty(lamps.layer(t0 + 150ms)));
    EXPECT_EQ(lamps.layer(t0 + 300ms), on);
    // The new TTL counts from the raise.
    EXPECT_EQ(lamps.next_change(t0 + 300ms), t0 + 10min);

    // A style with no zones keeps the key but shows nothing; restyle back and the lamp is back.
    lamps.set_style(LampKind::Agent, LampStyle{});
    EXPECT_EQ(lamps.active(), kinds({LampKind::Agent}));
    EXPECT_TRUE(empty(lamps.layer(t0 + 1s)));
    lamps.set_style(LampKind::Agent, restyled);
    EXPECT_EQ(lamps.layer(t0 + 1s), on);

    // The window link survived all of it.
    lamps.clear_window("w1");
    EXPECT_TRUE(lamps.empty());
}

namespace {

/// "k00", "k01", ...: sorts in raise order, so the lexicographically smallest key is the first.
std::string key_n(int n) { return std::format("k{:02}", n); }

constexpr int kCap = static_cast<int>(kMaxKeysPerKind);

} // namespace

TEST(LampSet, KeysPerKindAreBoundedAndTheOldestIsEvicted) {
    LampSet lamps = styled();
    const LayerOps on = lit(kAgent, {Zone::Wasd});
    constexpr int total = 70;
    static_assert(total > kCap);

    // The first key plays the intro; every later raise asks for one too, but the kind is lit.
    for (int i = 0; i < total; ++i) {
        const TimePoint now = t0 + std::chrono::seconds(i);
        // The first key is linked to a window: its link has to go away with the key.
        std::optional<std::string> window;
        if (i == 0) {
            window = "w0";
        }
        lamps.raise(LampKind::Agent, key_n(i), window, now, true);
        EXPECT_EQ(lamps.key_count(LampKind::Agent),
                  std::min(static_cast<std::size_t>(i) + 1, kMaxKeysPerKind))
            << i;
        if (i >= 1) {
            // Past the intro of the first key, an insert or an eviction never replays it.
            EXPECT_EQ(lamps.layer(now), on) << i;
            EXPECT_EQ(lamps.layer(now + 150ms), on) << i;
        }
    }
    EXPECT_EQ(lamps.key_count(LampKind::Agent), kMaxKeysPerKind);
    EXPECT_EQ(lamps.active(), kinds({LampKind::Agent}));

    // The first six keys (the oldest) are gone: clearing one changes nothing.
    for (int i = 0; i < total - kCap; ++i) {
        lamps.clear(LampKind::Agent, key_n(i));
        EXPECT_EQ(lamps.key_count(LampKind::Agent), kMaxKeysPerKind) << key_n(i);
    }
    // The window link of an evicted key went with it.
    lamps.clear_window("w0");
    EXPECT_EQ(lamps.key_count(LampKind::Agent), kMaxKeysPerKind);
    EXPECT_EQ(lamps.layer(t0 + 2min), on) << "still lit";

    // The newest key is present, and so is every survivor: clearing them empties the kind.
    lamps.clear(LampKind::Agent, key_n(total - 1));
    EXPECT_EQ(lamps.key_count(LampKind::Agent), kMaxKeysPerKind - 1);
    for (int i = total - kCap; i < total - 1; ++i) {
        EXPECT_EQ(lamps.layer(t0 + 2min), on) << "lit until the last key goes";
        lamps.clear(LampKind::Agent, key_n(i));
    }
    EXPECT_EQ(lamps.key_count(LampKind::Agent), 0U);
    EXPECT_TRUE(lamps.empty());
    EXPECT_TRUE(empty(lamps.layer(t0 + 2min)));
}

TEST(LampSet, TheBoundHoldsIndependentlyForUrgent) {
    LampSet lamps = styled();
    constexpr int total = 70;

    lamps.raise(LampKind::Agent, "a1", std::nullopt, t0, false);
    for (int i = 0; i < total; ++i) {
        lamps.raise(LampKind::Urgent, key_n(i), std::nullopt, t0 + std::chrono::seconds(i), false);
    }
    EXPECT_EQ(lamps.key_count(LampKind::Urgent), kMaxKeysPerKind);
    EXPECT_EQ(lamps.key_count(LampKind::Agent), 1U) << "the urgent flood evicts no agent key";

    for (int i = 0; i < total - kCap; ++i) {
        lamps.clear(LampKind::Urgent, key_n(i));
        EXPECT_EQ(lamps.key_count(LampKind::Urgent), kMaxKeysPerKind) << key_n(i);
    }
    lamps.clear(LampKind::Urgent, key_n(total - 1));
    EXPECT_EQ(lamps.key_count(LampKind::Urgent), kMaxKeysPerKind - 1);

    // A flood of agent keys is bounded on its own and evicts no urgent key.
    for (int i = 0; i < total; ++i) {
        lamps.raise(LampKind::Agent, key_n(i), std::nullopt, t0 + std::chrono::seconds(i), false);
    }
    EXPECT_EQ(lamps.key_count(LampKind::Agent), kMaxKeysPerKind);
    EXPECT_EQ(lamps.key_count(LampKind::Urgent), kMaxKeysPerKind - 1);
    lamps.clear_kind(LampKind::Agent);
    EXPECT_EQ(lamps.layer(t0 + 1min), lit(kUrgent, {Zone::Left, Zone::Middle, Zone::Right}));
}

TEST(LampSet, ReRaisingAnExistingKeyAtTheCapEvictsNothing) {
    LampSet lamps = styled();
    for (int i = 0; i < kCap; ++i) {
        lamps.raise(LampKind::Agent, key_n(i), std::nullopt, t0 + std::chrono::seconds(i), false);
    }
    ASSERT_EQ(lamps.key_count(LampKind::Agent), kMaxKeysPerKind);

    // The oldest key is raised again: it only refreshes, so every key is still there.
    const TimePoint later = t0 + 10min;
    lamps.raise(LampKind::Agent, key_n(0), std::nullopt, later, true);
    EXPECT_EQ(lamps.key_count(LampKind::Agent), kMaxKeysPerKind);
    EXPECT_EQ(lamps.next_change(later), t0 + 1s + 1h) << "k01 is the oldest now";

    // The refreshed key is the newest now, so the next new key evicts k01, not k00.
    lamps.raise(LampKind::Agent, "new", std::nullopt, later + 1s, false);
    EXPECT_EQ(lamps.key_count(LampKind::Agent), kMaxKeysPerKind);
    lamps.clear(LampKind::Agent, key_n(1));
    EXPECT_EQ(lamps.key_count(LampKind::Agent), kMaxKeysPerKind) << "k01 was evicted";
    lamps.clear(LampKind::Agent, key_n(0));
    EXPECT_EQ(lamps.key_count(LampKind::Agent), kMaxKeysPerKind - 1) << "k00 survived";
}

TEST(LampSet, EvictionBreaksTiesBySmallestKey) {
    LampSet lamps = styled();
    // Every key raised at the same time, in reverse order: only the key decides who goes.
    for (int i = kCap - 1; i >= 0; --i) {
        lamps.raise(LampKind::Agent, key_n(i), std::nullopt, t0, false);
    }
    ASSERT_EQ(lamps.key_count(LampKind::Agent), kMaxKeysPerKind);

    lamps.raise(LampKind::Agent, "zz", std::nullopt, t0, false);
    EXPECT_EQ(lamps.key_count(LampKind::Agent), kMaxKeysPerKind);
    lamps.clear(LampKind::Agent, key_n(0));
    EXPECT_EQ(lamps.key_count(LampKind::Agent), kMaxKeysPerKind) << "k00 was evicted first";

    lamps.raise(LampKind::Agent, "zy", std::nullopt, t0, false);
    lamps.clear(LampKind::Agent, key_n(1));
    EXPECT_EQ(lamps.key_count(LampKind::Agent), kMaxKeysPerKind) << "then k01";

    // k02 is the smallest left, and it is still there.
    lamps.clear(LampKind::Agent, key_n(2));
    EXPECT_EQ(lamps.key_count(LampKind::Agent), kMaxKeysPerKind - 1);
}

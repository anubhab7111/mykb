#include "core/clip_player.hpp"

#include <chrono>
#include <cstdint>
#include <gtest/gtest.h>
#include <string>

using namespace omenkbd;
using namespace std::chrono_literals;

namespace {

constexpr Rgb kC{10, 200, 90};

const TimePoint t0 = Clock::now();

/// A 150 ms flash on all four zones: one lit step.
Clip flash_clip() {
    return flash(kC, {Zone::Left, Zone::Middle, Zone::Right, Zone::Wasd}, 1, 150ms, 150ms);
}

/// A 360 ms sweep: three steps of 120 ms.
Clip sweep_clip() { return sweep(Direction::Up, kC, WasdWith::Left, 120ms); }

PlayerLimits limits(std::uint32_t per_minute, Duration cooldown, Duration gap) {
    return PlayerLimits{per_minute, cooldown, gap};
}

} // namespace

TEST(ClipPlayer, DefaultLimitsAreThoseOfTheSpec) {
    const PlayerLimits l;
    EXPECT_EQ(l.max_per_minute, 30u);
    EXPECT_EQ(l.window_cooldown, 500ms);
    EXPECT_EQ(l.gap, 150ms);
}

TEST(ClipPlayer, ChannelNames) {
    EXPECT_EQ(name(Channel::Workspace), "workspace");
    EXPECT_EQ(name(Channel::Window), "window");
    EXPECT_EQ(name(Channel::Notify), "notify");
    EXPECT_EQ(name(Channel::AgentDone), "agent-done");
}

TEST(ClipPlayer, StartsAnIdleOfferAndFollowsTheSteps) {
    ClipPlayer p;
    EXPECT_FALSE(p.playing().has_value());
    EXPECT_TRUE(empty(p.layer(t0)));
    EXPECT_FALSE(p.next_change(t0).has_value());

    EXPECT_EQ(p.offer(Channel::Workspace, sweep_clip(), t0), Offer::Started);
    EXPECT_EQ(p.layer(t0), sweep_clip().steps[0].ops);
    EXPECT_EQ(p.layer(t0 + 130ms), sweep_clip().steps[1].ops);
    EXPECT_EQ(p.layer(t0 + 359ms), sweep_clip().steps[2].ops);
    EXPECT_EQ(p.playing(), Channel::Workspace);
    EXPECT_EQ(p.next_change(t0), t0 + 120ms);
    EXPECT_EQ(p.next_change(t0 + 130ms), t0 + 240ms);
    EXPECT_EQ(p.next_change(t0 + 240ms), t0 + 360ms);

    p.advance(t0 + 360ms);
    EXPECT_FALSE(p.playing().has_value());
    EXPECT_TRUE(empty(p.layer(t0 + 360ms)));
    EXPECT_FALSE(p.next_change(t0 + 360ms).has_value());
}

TEST(ClipPlayer, AdvanceBeforeTheEndKeepsTheClipPlaying) {
    ClipPlayer p;
    EXPECT_EQ(p.offer(Channel::Workspace, sweep_clip(), t0), Offer::Started);
    p.advance(t0 + 359ms);
    EXPECT_EQ(p.playing(), Channel::Workspace);
    EXPECT_EQ(p.layer(t0 + 359ms), sweep_clip().steps[2].ops);
}

TEST(ClipPlayer, ATimeBeforeTheStartActsAsStepZero) {
    ClipPlayer p;
    EXPECT_EQ(p.offer(Channel::Workspace, sweep_clip(), t0), Offer::Started);
    // Never a negative index: a `now` earlier than the start reads the first step.
    EXPECT_EQ(p.layer(t0 - 50ms), sweep_clip().steps[0].ops);
    EXPECT_EQ(p.next_change(t0 - 50ms), t0 + 120ms);
}

TEST(ClipPlayer, WorkspaceOfferWhileBusyIsDropped) {
    ClipPlayer p;
    EXPECT_EQ(p.offer(Channel::Workspace, sweep_clip(), t0), Offer::Started);
    EXPECT_EQ(p.offer(Channel::Workspace, flash_clip(), t0 + 100ms), Offer::Dropped);
    // The dropped clip did not replace or disturb the playing one.
    EXPECT_EQ(p.playing(), Channel::Workspace);
    EXPECT_EQ(p.layer(t0 + 130ms), sweep_clip().steps[1].ops);
    EXPECT_EQ(p.next_change(t0 + 130ms), t0 + 240ms);
    // Nothing was queued either: once the sweep is over the player is idle.
    p.advance(t0 + 360ms);
    p.advance(t0 + 2s);
    EXPECT_FALSE(p.playing().has_value());
}

TEST(ClipPlayer, WorkspaceAndWindowOffersAreDroppedWhileAClipWaitsInTheQueue) {
    ClipPlayer p{limits(30, 0ms, 150ms)};
    EXPECT_EQ(p.offer(Channel::Notify, flash_clip(), t0), Offer::Started);
    EXPECT_EQ(p.offer(Channel::Notify, flash_clip(), t0 + 10ms), Offer::Queued);

    // The flash is over at t0+150ms but the queued one waits until t0+300ms: still busy.
    p.advance(t0 + 150ms);
    EXPECT_FALSE(p.playing().has_value());
    EXPECT_EQ(p.offer(Channel::Workspace, flash_clip(), t0 + 200ms), Offer::Dropped);
    EXPECT_EQ(p.offer(Channel::Window, flash_clip(), t0 + 250ms), Offer::Dropped);

    // The queued clip is untouched by those drops, and plays its turn.
    p.advance(t0 + 300ms);
    EXPECT_EQ(p.playing(), Channel::Notify);
    p.advance(t0 + 450ms);
    EXPECT_FALSE(p.playing().has_value());
    EXPECT_EQ(p.offer(Channel::Workspace, flash_clip(), t0 + 500ms), Offer::Started);
}

TEST(ClipPlayer, WindowClipsHonourTheCooldownFromTheLastWindowStart) {
    ClipPlayer p;
    EXPECT_EQ(p.offer(Channel::Window, flash_clip(), t0), Offer::Started);
    // The 150 ms clip is over by now, so only the cooldown can drop this.
    EXPECT_EQ(p.offer(Channel::Window, flash_clip(), t0 + 300ms), Offer::Dropped);
    // Measured from the last window clip's start, not from the dropped offer at t0+300ms.
    EXPECT_EQ(p.offer(Channel::Window, flash_clip(), t0 + 500ms), Offer::Started);
    EXPECT_EQ(p.offer(Channel::Window, flash_clip(), t0 + 999ms), Offer::Dropped);
    EXPECT_EQ(p.offer(Channel::Window, flash_clip(), t0 + 1000ms), Offer::Started);
}

TEST(ClipPlayer, WindowOfferWhileBusyIsDroppedEvenWithoutACooldown) {
    ClipPlayer p{limits(30, 0ms, 150ms)};
    EXPECT_EQ(p.offer(Channel::Window, sweep_clip(), t0), Offer::Started);
    EXPECT_EQ(p.offer(Channel::Window, flash_clip(), t0 + 50ms), Offer::Dropped);
    EXPECT_EQ(p.layer(t0 + 130ms), sweep_clip().steps[1].ops);
}

TEST(ClipPlayer, OnlyWindowClipsStartTheWindowCooldown) {
    ClipPlayer p;
    EXPECT_EQ(p.offer(Channel::Workspace, flash_clip(), t0), Offer::Started);
    EXPECT_EQ(p.offer(Channel::Window, flash_clip(), t0 + 200ms), Offer::Started);
}

TEST(ClipPlayer, NotifyQueuesOneAndDropsTheRest) {
    ClipPlayer p;
    EXPECT_EQ(p.offer(Channel::Notify, flash_clip(), t0), Offer::Started);
    EXPECT_EQ(p.offer(Channel::AgentDone, sweep_clip(), t0 + 20ms), Offer::Queued);
    EXPECT_EQ(p.offer(Channel::Notify, flash_clip(), t0 + 40ms), Offer::Dropped);
    EXPECT_EQ(p.offer(Channel::AgentDone, flash_clip(), t0 + 60ms), Offer::Dropped);

    // The first queued clip is the one that plays: the dropped ones did not replace it.
    p.advance(t0 + 300ms);
    EXPECT_EQ(p.playing(), Channel::AgentDone);
    EXPECT_EQ(p.layer(t0 + 300ms), sweep_clip().steps[0].ops);
    EXPECT_EQ(p.next_change(t0 + 300ms), t0 + 420ms);
    p.advance(t0 + 660ms);
    EXPECT_FALSE(p.playing().has_value());
    EXPECT_FALSE(p.next_change(t0 + 660ms).has_value());
}

TEST(ClipPlayer, NotifyOfferWhileAWorkspaceClipPlaysQueuesBehindIt) {
    ClipPlayer p;
    EXPECT_EQ(p.offer(Channel::Workspace, sweep_clip(), t0), Offer::Started);
    EXPECT_EQ(p.offer(Channel::Notify, flash_clip(), t0 + 100ms), Offer::Queued);
    EXPECT_EQ(p.playing(), Channel::Workspace);
}

TEST(ClipPlayer, QueuedClipStartsAfterTheGapSoTwoFlashesAreTwoFlashes) {
    ClipPlayer p{limits(30, 500ms, 150ms)};
    EXPECT_EQ(p.offer(Channel::Notify, flash_clip(), t0), Offer::Started);
    EXPECT_EQ(p.offer(Channel::Notify, flash_clip(), t0 + 20ms), Offer::Queued);

    p.advance(t0 + 150ms);
    EXPECT_FALSE(p.playing().has_value());
    EXPECT_TRUE(empty(p.layer(t0 + 200ms)));
    EXPECT_EQ(p.next_change(t0 + 150ms), t0 + 300ms);

    // Not early: the gap is not over at t0+299ms.
    p.advance(t0 + 299ms);
    EXPECT_FALSE(p.playing().has_value());

    p.advance(t0 + 300ms);
    EXPECT_EQ(p.playing(), Channel::Notify);
    EXPECT_EQ(p.layer(t0 + 300ms), flash_clip().steps[0].ops);
    EXPECT_EQ(p.next_change(t0 + 300ms), t0 + 450ms);
}

TEST(ClipPlayer, AnOfferAfterTheEndStartsWithoutWaitingForAnAdvance) {
    // The loop may handle a signal just before the timer that retires the clip fires.
    ClipPlayer p;
    EXPECT_EQ(p.offer(Channel::Workspace, flash_clip(), t0), Offer::Started);
    EXPECT_EQ(p.offer(Channel::Workspace, sweep_clip(), t0 + 200ms), Offer::Started);
    EXPECT_EQ(p.layer(t0 + 200ms), sweep_clip().steps[0].ops);
}

TEST(ClipPlayer, RateLimitCapsStartsPerRollingMinute) {
    ClipPlayer p{limits(30, 0ms, 0ms)};
    int started = 0;
    for (int k = 0; k < 100; ++k) {
        SCOPED_TRACE("offer " + std::to_string(k));
        const TimePoint now = t0 + k * 600ms;
        p.advance(now);
        const Offer expected = k < 30 ? Offer::Started : Offer::Dropped;
        const Offer got = p.offer(Channel::Window, flash_clip(), now);
        EXPECT_EQ(got, expected);
        started += got == Offer::Started ? 1 : 0;
    }
    EXPECT_EQ(started, 30);

    p.advance(t0 + 61s);
    EXPECT_EQ(p.offer(Channel::Window, flash_clip(), t0 + 61s), Offer::Started);
}

TEST(ClipPlayer, TheRollingMinuteSlidesOneStartAtATime) {
    ClipPlayer p{limits(2, 0ms, 0ms)};
    EXPECT_EQ(p.offer(Channel::Workspace, flash_clip(), t0), Offer::Started);
    EXPECT_EQ(p.offer(Channel::Workspace, flash_clip(), t0 + 10s), Offer::Started);
    EXPECT_EQ(p.offer(Channel::Workspace, flash_clip(), t0 + 59s + 999ms), Offer::Dropped);
    // A start leaves the window exactly one minute later.
    EXPECT_EQ(p.offer(Channel::Workspace, flash_clip(), t0 + 60s), Offer::Started);
    EXPECT_EQ(p.offer(Channel::Workspace, flash_clip(), t0 + 60s + 500ms), Offer::Dropped);
    EXPECT_EQ(p.offer(Channel::Workspace, flash_clip(), t0 + 70s), Offer::Started);
}

TEST(ClipPlayer, AClipThatCannotStartForTheRateLimitIsNotQueued) {
    ClipPlayer p{limits(1, 0ms, 0ms)};
    EXPECT_EQ(p.offer(Channel::Notify, flash_clip(), t0), Offer::Started);
    p.advance(t0 + 150ms);
    EXPECT_EQ(p.offer(Channel::Notify, flash_clip(), t0 + 200ms), Offer::Dropped);
    EXPECT_FALSE(p.playing().has_value());
    EXPECT_FALSE(p.next_change(t0 + 200ms).has_value());
}

TEST(ClipPlayer, AQueuedClipThatCannotStartForTheRateLimitIsDiscarded) {
    ClipPlayer p{limits(1, 0ms, 0ms)};
    EXPECT_EQ(p.offer(Channel::Notify, flash_clip(), t0), Offer::Started);
    EXPECT_EQ(p.offer(Channel::Notify, flash_clip(), t0 + 10ms), Offer::Queued);

    p.advance(t0 + 150ms);
    EXPECT_FALSE(p.playing().has_value());
    EXPECT_FALSE(p.next_change(t0 + 150ms).has_value());

    // It does not come back once the minute is over: it was discarded, not postponed.
    p.advance(t0 + 61s);
    EXPECT_FALSE(p.playing().has_value());
    EXPECT_TRUE(empty(p.layer(t0 + 61s)));
    // And the slot is free again for a new offer.
    EXPECT_EQ(p.offer(Channel::Notify, flash_clip(), t0 + 61s), Offer::Started);
}

TEST(ClipPlayer, ATimeJumpRetiresFinishedClipsWithoutReplayingThem) {
    ClipPlayer p;
    EXPECT_EQ(p.offer(Channel::Notify, sweep_clip(), t0), Offer::Started);
    EXPECT_EQ(p.offer(Channel::AgentDone, flash_clip(), t0 + 20ms), Offer::Queued);

    const TimePoint late = t0 + 10min;
    p.advance(late);
    // A is gone and B starts from the jump time at step 0: nothing is replayed or caught up.
    EXPECT_EQ(p.playing(), Channel::AgentDone);
    EXPECT_EQ(p.layer(late), flash_clip().steps[0].ops);
    EXPECT_NE(p.layer(late), sweep_clip().steps[0].ops);
    EXPECT_EQ(p.layer(late + 149ms), flash_clip().steps[0].ops);
    EXPECT_EQ(p.next_change(late), late + 150ms);

    p.advance(late + 150ms);
    EXPECT_FALSE(p.playing().has_value());
    EXPECT_TRUE(empty(p.layer(late + 150ms)));
    EXPECT_FALSE(p.next_change(late + 150ms).has_value());

    // A second jump finds nothing left to play.
    p.advance(late + 10min);
    EXPECT_FALSE(p.playing().has_value());
    EXPECT_FALSE(p.next_change(late + 10min).has_value());
}

TEST(ClipPlayer, ATimeJumpWithNothingQueuedJustRetiresTheClip) {
    ClipPlayer p;
    EXPECT_EQ(p.offer(Channel::Workspace, sweep_clip(), t0), Offer::Started);
    p.advance(t0 + 10min);
    EXPECT_FALSE(p.playing().has_value());
    EXPECT_TRUE(empty(p.layer(t0 + 10min)));
    EXPECT_FALSE(p.next_change(t0 + 10min).has_value());
}

TEST(ClipPlayer, ATimeJumpDuringTheGapStartsTheQueuedClipAtTheJumpTime) {
    ClipPlayer p;
    EXPECT_EQ(p.offer(Channel::Notify, flash_clip(), t0), Offer::Started);
    EXPECT_EQ(p.offer(Channel::Notify, sweep_clip(), t0 + 20ms), Offer::Queued);
    p.advance(t0 + 150ms);
    ASSERT_FALSE(p.playing().has_value());

    const TimePoint late = t0 + 1h;
    p.advance(late);
    EXPECT_EQ(p.playing(), Channel::Notify);
    EXPECT_EQ(p.layer(late), sweep_clip().steps[0].ops);
    EXPECT_EQ(p.next_change(late), late + 120ms);
}

TEST(ClipPlayer, ClearEmptiesEverything) {
    ClipPlayer p;
    EXPECT_EQ(p.offer(Channel::Notify, sweep_clip(), t0), Offer::Started);
    EXPECT_EQ(p.offer(Channel::Notify, flash_clip(), t0 + 20ms), Offer::Queued);

    p.clear();
    EXPECT_FALSE(p.playing().has_value());
    EXPECT_TRUE(empty(p.layer(t0 + 130ms)));
    EXPECT_FALSE(p.next_change(t0 + 130ms).has_value());

    // The queued clip went too: nothing starts later.
    p.advance(t0 + 1s);
    EXPECT_FALSE(p.playing().has_value());
    EXPECT_FALSE(p.next_change(t0 + 1s).has_value());

    // Both slots are free again.
    EXPECT_EQ(p.offer(Channel::Notify, flash_clip(), t0 + 2s), Offer::Started);
    EXPECT_EQ(p.offer(Channel::Notify, flash_clip(), t0 + 2s + 10ms), Offer::Queued);
}

TEST(ClipPlayer, ClearAlsoForgetsTheWindowCooldownAndTheRateCount) {
    ClipPlayer p{limits(2, 500ms, 0ms)};
    EXPECT_EQ(p.offer(Channel::Window, flash_clip(), t0), Offer::Started);
    // Without this clear, the cooldown would drop the window clip below.
    p.clear();
    EXPECT_EQ(p.offer(Channel::Window, flash_clip(), t0 + 100ms), Offer::Started);

    // One start is counted now. Without this clear, the second workspace clip below would be over
    // the limit of two.
    p.clear();
    EXPECT_EQ(p.offer(Channel::Workspace, flash_clip(), t0 + 1s), Offer::Started);
    EXPECT_EQ(p.offer(Channel::Workspace, flash_clip(), t0 + 2s), Offer::Started);
    EXPECT_EQ(p.offer(Channel::Workspace, flash_clip(), t0 + 3s), Offer::Dropped);
}

TEST(ClipPlayer, ClearKeepsTheLimits) {
    ClipPlayer p{limits(1, 0ms, 0ms)};
    p.clear();
    EXPECT_EQ(p.offer(Channel::Workspace, flash_clip(), t0), Offer::Started);
    EXPECT_EQ(p.offer(Channel::Workspace, flash_clip(), t0 + 200ms), Offer::Dropped);
}

TEST(ClipPlayer, SetLimitsAppliesToLaterOffers) {
    ClipPlayer p;
    p.set_limits(limits(1, 0ms, 0ms));
    EXPECT_EQ(p.offer(Channel::Workspace, flash_clip(), t0), Offer::Started);
    EXPECT_EQ(p.offer(Channel::Workspace, flash_clip(), t0 + 200ms), Offer::Dropped);

    // Raising the cap lets starts through again, counting those already made.
    p.set_limits(limits(2, 0ms, 0ms));
    EXPECT_EQ(p.offer(Channel::Workspace, flash_clip(), t0 + 300ms), Offer::Started);
    EXPECT_EQ(p.offer(Channel::Workspace, flash_clip(), t0 + 500ms), Offer::Dropped);
}

#include "core/clip.hpp"

#include "core/effects.hpp"

#include <chrono>
#include <cstddef>
#include <gtest/gtest.h>
#include <optional>
#include <vector>

using namespace omenkbd;
using namespace std::chrono_literals;

namespace {

constexpr Rgb kC{10, 200, 90};
// One distinct color per zone, so every op visibly changes the zone it lands on.
constexpr Frame kBelow{Rgb{255, 0, 0}, Rgb{0, 255, 0}, Rgb{0, 0, 255}, Rgb{200, 100, 50}};
const std::vector<Zone> all{Zone::Left, Zone::Middle, Zone::Right, Zone::Wasd};

/// A LayerOps that paints `color` on `zones` and has no op on the others.
LayerOps paint_on(const std::vector<Zone> &zones, Rgb color) {
    LayerOps ops;
    for (Zone z : zones) {
        ops[index(z)] = Paint{color};
    }
    return ops;
}

} // namespace

TEST(Clip, SweepUpLightsLeftMiddleRightWithWasdOnTheLeftStep) {
    const Clip c = sweep(Direction::Up, kC, WasdWith::Left, 120ms);
    ASSERT_EQ(c.steps.size(), 3u);
    EXPECT_EQ(c.steps[0].ops, paint_on({Zone::Left, Zone::Wasd}, kC));
    EXPECT_EQ(c.steps[1].ops, paint_on({Zone::Middle}, kC));
    EXPECT_EQ(c.steps[2].ops, paint_on({Zone::Right}, kC));
    for (const ClipStep &s : c.steps) {
        EXPECT_EQ(s.hold, 120ms);
    }
    EXPECT_EQ(c.total(), 360ms);
}

TEST(Clip, SweepDownReversesTheOrderKeepingWasdWithLeft) {
    const Clip c = sweep(Direction::Down, kC, WasdWith::Left, 120ms);
    ASSERT_EQ(c.steps.size(), 3u);
    EXPECT_EQ(c.steps[0].ops, paint_on({Zone::Right}, kC));
    EXPECT_EQ(c.steps[1].ops, paint_on({Zone::Middle}, kC));
    EXPECT_EQ(c.steps[2].ops, paint_on({Zone::Left, Zone::Wasd}, kC));
    for (const ClipStep &s : c.steps) {
        EXPECT_EQ(s.hold, 120ms);
    }
    EXPECT_EQ(c.total(), 360ms);
}

TEST(Clip, WasdWithMiddleAndNone) {
    // Middle: the middle step also paints Wasd, whichever way the sweep travels.
    const Clip up_middle = sweep(Direction::Up, kC, WasdWith::Middle, 120ms);
    ASSERT_EQ(up_middle.steps.size(), 3u);
    EXPECT_EQ(up_middle.steps[0].ops, paint_on({Zone::Left}, kC));
    EXPECT_EQ(up_middle.steps[1].ops, paint_on({Zone::Middle, Zone::Wasd}, kC));
    EXPECT_EQ(up_middle.steps[2].ops, paint_on({Zone::Right}, kC));

    const Clip down_middle = sweep(Direction::Down, kC, WasdWith::Middle, 120ms);
    ASSERT_EQ(down_middle.steps.size(), 3u);
    EXPECT_EQ(down_middle.steps[0].ops, paint_on({Zone::Right}, kC));
    EXPECT_EQ(down_middle.steps[1].ops, paint_on({Zone::Middle, Zone::Wasd}, kC));
    EXPECT_EQ(down_middle.steps[2].ops, paint_on({Zone::Left}, kC));

    // None: Wasd is never painted.
    const Clip up_none = sweep(Direction::Up, kC, WasdWith::None, 120ms);
    ASSERT_EQ(up_none.steps.size(), 3u);
    EXPECT_EQ(up_none.steps[0].ops, paint_on({Zone::Left}, kC));
    EXPECT_EQ(up_none.steps[1].ops, paint_on({Zone::Middle}, kC));
    EXPECT_EQ(up_none.steps[2].ops, paint_on({Zone::Right}, kC));

    const Clip down_none = sweep(Direction::Down, kC, WasdWith::None, 120ms);
    ASSERT_EQ(down_none.steps.size(), 3u);
    EXPECT_EQ(down_none.steps[0].ops, paint_on({Zone::Right}, kC));
    EXPECT_EQ(down_none.steps[1].ops, paint_on({Zone::Middle}, kC));
    EXPECT_EQ(down_none.steps[2].ops, paint_on({Zone::Left}, kC));
}

TEST(Clip, FlashWithOneBlinkIsOneStepAndTwoBlinksAreOnOffOn) {
    EXPECT_EQ(flash(kC, all, 1, 150ms, 150ms).steps.size(), 1u);
    const Clip two = flash(kC, all, 2, 150ms, 150ms);
    ASSERT_EQ(two.steps.size(), 3u);
    EXPECT_EQ(two.steps[0].ops, paint_on(all, kC));
    EXPECT_TRUE(empty(two.steps[1].ops));
    EXPECT_EQ(two.steps[2].ops, paint_on(all, kC));
    EXPECT_EQ(two.total(), 450ms);
}

TEST(Clip, FlashPaintsOnlyItsZonesAndHoldsOnAndOffForTheirOwnTimes) {
    // On and off differ, so swapping them would show. Three blinks are ON OFF ON OFF ON.
    const Clip c = flash(kC, {Zone::Right, Zone::Wasd}, 3, 100ms, 40ms);
    ASSERT_EQ(c.steps.size(), 5u);
    const LayerOps lit = paint_on({Zone::Right, Zone::Wasd}, kC);
    for (std::size_t i = 0; i < c.steps.size(); ++i) {
        const bool is_on = i % 2 == 0;
        EXPECT_EQ(c.steps[i].ops, is_on ? lit : LayerOps{}) << "step " << i;
        EXPECT_EQ(c.steps[i].hold, is_on ? 100ms : 40ms) << "step " << i;
    }
    EXPECT_EQ(c.total(), 380ms);
}

TEST(Clip, FlashWithFewerThanOneBlinkIsOneOnStep) {
    // R1: 0 means "light steadily at once", the same as 1. Never an empty clip.
    for (int blinks : {0, -3}) {
        const Clip c = flash(kC, all, blinks, 150ms, 70ms);
        ASSERT_EQ(c.steps.size(), 1u) << "blinks " << blinks;
        EXPECT_EQ(c.steps[0].ops, paint_on(all, kC));
        EXPECT_EQ(c.steps[0].hold, 150ms);
    }
}

TEST(Clip, LiftAndDipUseTheSameMathAsTheEffects) {
    // `omenkbd::apply`, qualified: both arguments are std::array, so an unqualified call also finds
    // std::apply by ADL and that wins for non-const arguments (see clip.hpp).
    for (std::size_t i = 0; i < 4; ++i) {
        EXPECT_EQ(omenkbd::apply(kBelow, lift(0.5, 120ms).steps[0].ops)[i],
                  Rgb::lerp(kBelow[i], kWhite, 0.5));
        EXPECT_EQ(omenkbd::apply(kBelow, dip(0.35, 120ms).steps[0].ops)[i],
                  multiply(kBelow[i], 0.35));
    }
}

TEST(Clip, LiftAndDipAreOneStepOnAllFourZones) {
    const Clip l = lift(0.25, 120ms);
    ASSERT_EQ(l.steps.size(), 1u);
    EXPECT_EQ(l.steps[0].hold, 120ms);
    EXPECT_EQ(l.total(), 120ms);
    for (const std::optional<ZoneOp> &op : l.steps[0].ops) {
        EXPECT_EQ(op, (ZoneOp{Mix{kWhite, 0.25}}));
    }

    const Clip d = dip(0.35, 80ms);
    ASSERT_EQ(d.steps.size(), 1u);
    EXPECT_EQ(d.steps[0].hold, 80ms);
    EXPECT_EQ(d.total(), 80ms);
    for (const std::optional<ZoneOp> &op : d.steps[0].ops) {
        EXPECT_EQ(op, (ZoneOp{Scale{0.35}}));
    }
}

TEST(Clip, OpsAtWalksTheStepsAndIsEmptyAfterTheEnd) {
    const Clip c = sweep(Direction::Up, kC, WasdWith::Left, 120ms);
    EXPECT_EQ(c.ops_at(0ms), c.steps[0].ops);
    EXPECT_EQ(c.ops_at(119ms), c.steps[0].ops);
    EXPECT_EQ(c.ops_at(120ms), c.steps[1].ops);
    EXPECT_EQ(c.ops_at(239ms), c.steps[1].ops);
    EXPECT_EQ(c.ops_at(240ms), c.steps[2].ops);
    EXPECT_EQ(c.ops_at(359ms), c.steps[2].ops);
    EXPECT_TRUE(empty(c.ops_at(360ms)));
    EXPECT_TRUE(empty(c.ops_at(361ms)));
    EXPECT_TRUE(empty(c.ops_at(1h)));
}

TEST(Clip, NextBoundaryIsStrictlyAfter) {
    const Clip c = sweep(Direction::Up, kC, WasdWith::Left, 120ms);
    EXPECT_EQ(c.next_boundary(0ms), 120ms);
    EXPECT_EQ(c.next_boundary(119ms), 120ms);
    EXPECT_EQ(c.next_boundary(120ms), 240ms);
    EXPECT_EQ(c.next_boundary(240ms), 360ms);
    EXPECT_EQ(c.next_boundary(360ms), std::nullopt);
    EXPECT_EQ(c.next_boundary(1h), std::nullopt);
}

TEST(Clip, StepsWithUnequalHoldsAreWalkedByCumulativeTime) {
    // ON 100 ms, OFF 40 ms, ON 100 ms: the steps end at 100, 140 and 240 ms.
    const Clip c = flash(kC, {Zone::Wasd}, 2, 100ms, 40ms);
    const LayerOps lit = paint_on({Zone::Wasd}, kC);
    EXPECT_EQ(c.ops_at(99ms), lit);
    EXPECT_TRUE(empty(c.ops_at(100ms)));
    EXPECT_TRUE(empty(c.ops_at(139ms)));
    EXPECT_EQ(c.ops_at(140ms), lit);
    EXPECT_EQ(c.ops_at(239ms), lit);
    EXPECT_TRUE(empty(c.ops_at(240ms)));
    EXPECT_EQ(c.next_boundary(0ms), 100ms);
    EXPECT_EQ(c.next_boundary(99ms), 100ms);
    EXPECT_EQ(c.next_boundary(100ms), 140ms);
    EXPECT_EQ(c.next_boundary(139ms), 140ms);
    EXPECT_EQ(c.next_boundary(140ms), 240ms);
    EXPECT_EQ(c.next_boundary(240ms), std::nullopt);
}

TEST(Clip, AClipWithoutStepsIsAlreadyOver) {
    const Clip c{};
    EXPECT_EQ(c.total(), 0ms);
    EXPECT_TRUE(empty(c.ops_at(0ms)));
    EXPECT_EQ(c.next_boundary(0ms), std::nullopt);
}

TEST(Clip, EmptyMeansNoZoneHasAnOp) {
    EXPECT_TRUE(empty(LayerOps{}));
    // Each zone alone, so a check that skips one of the four would show.
    for (Zone z : kAllZones) {
        EXPECT_FALSE(empty(paint_on({z}, kC))) << name(z);
    }
}

TEST(Clip, ApplyLeavesZonesWithoutAnOpUntouched) {
    // No ops at all: the frame comes back as it was.
    EXPECT_EQ(omenkbd::apply(kBelow, LayerOps{}), kBelow);
    // One op on Middle: the other three zones keep what was below.
    const Frame out = omenkbd::apply(kBelow, paint_on({Zone::Middle}, kC));
    EXPECT_EQ(out[index(Zone::Left)], kBelow[index(Zone::Left)]);
    EXPECT_EQ(out[index(Zone::Middle)], kC);
    EXPECT_EQ(out[index(Zone::Right)], kBelow[index(Zone::Right)]);
    EXPECT_EQ(out[index(Zone::Wasd)], kBelow[index(Zone::Wasd)]);
}

TEST(Clip, ApplyRunsEachOpOnItsOwnZone) {
    LayerOps ops;
    ops[index(Zone::Left)] = Paint{kC};
    // 0.25, not 0.5: lerp(a, b, 0.5) equals lerp(b, a, 0.5), so only an asymmetric amount shows a
    // Mix that swaps the zone below with its target.
    ops[index(Zone::Middle)] = Mix{kC, 0.25};
    ops[index(Zone::Right)] = Scale{0.35};
    // Wasd has no op.
    const Frame out = omenkbd::apply(kBelow, ops);
    EXPECT_EQ(out[index(Zone::Left)], kC);
    EXPECT_EQ(out[index(Zone::Middle)], Rgb::lerp(kBelow[index(Zone::Middle)], kC, 0.25));
    EXPECT_EQ(out[index(Zone::Right)], multiply(kBelow[index(Zone::Right)], 0.35));
    EXPECT_EQ(out[index(Zone::Wasd)], kBelow[index(Zone::Wasd)]);
}

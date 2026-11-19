#include "core/effects.hpp"

#include <cmath>
#include <gtest/gtest.h>

using namespace omenkbd;

namespace {
constexpr Rgb R{255, 0, 0};
constexpr Rgb G{0, 255, 0};
constexpr Rgb B{0, 0, 255};
constexpr Frame kBase{R, G, B, kWhite};
double p() { return period_secs(50); }
} // namespace

TEST(Effects, PeriodBounds) {
    EXPECT_EQ(period_secs(100), 0.5);
    EXPECT_EQ(period_secs(1), 10.0);
    EXPECT_EQ(period_secs(0), 10.0) << "speed is clamped to 1..=100";
}

TEST(Effects, StaticIsIdentity) { EXPECT_EQ(render(Effect::Static, kBase, 3.7, 50), kBase); }

TEST(Effects, BreatheFullAtZeroDimAtHalf) {
    EXPECT_EQ(render(Effect::Breathe, kBase, 0.0, 50), kBase);
    EXPECT_EQ(render(Effect::Breathe, kBase, p() / 2.0, 50)[0], (Rgb{26, 0, 0}));
}

TEST(Effects, WavePhases) {
    const Frame f = render(Effect::Wave, kBase, 0.0, 50);
    EXPECT_EQ(f[0], R);
    EXPECT_EQ(f[3], R);
    EXPECT_EQ(f[1], (Rgb{64, 191, 0}));
}

TEST(Effects, CycleStartsAtPrimaries) {
    EXPECT_EQ(render(Effect::Cycle, kBase, 0.0, 50), (Frame{R, G, B, R}));
}

TEST(Effects, GradientStartsAtBaseAndBlends) {
    EXPECT_EQ(render(Effect::Gradient, kBase, 0.0, 50), kBase);
    EXPECT_EQ(render(Effect::Gradient, kBase, p() / 8.0, 50)[0], (Rgb{128, 128, 0}));
}

TEST(Effects, PulseRange) {
    EXPECT_EQ(pulse_factor(0.0), 1.0);
    EXPECT_NEAR(pulse_factor(0.5), 0.2, 1e-9);
}

TEST(Effects, NamesAndAnimationFlag) {
    EXPECT_EQ(name(Effect::Breathe), "breathe");
    EXPECT_EQ(effect_from_name("wave"), Effect::Wave);
    EXPECT_EQ(effect_from_name("WAVE"), Effect::Wave);
    EXPECT_EQ(effect_from_name("sparkle"), std::nullopt);
    EXPECT_FALSE(is_animated(Effect::Static));
    EXPECT_TRUE(is_animated(Effect::Cycle));
}

// ---- Slow Catppuccin glows: drift, glow, tide, dusk --------------------------------------------

TEST(SlowEffects, NamesAndAnimationFlag) {
    for (const auto &[e, n] : {std::pair{Effect::Drift, "drift"}, std::pair{Effect::Glow, "glow"},
                               std::pair{Effect::Tide, "tide"}, std::pair{Effect::Dusk, "dusk"}}) {
        EXPECT_EQ(name(e), n);
        EXPECT_EQ(effect_from_name(n), e);
        EXPECT_TRUE(is_animated(e));
    }
    EXPECT_EQ(effect_from_name("DUSK"), Effect::Dusk);
}

TEST(SlowEffects, PeriodsAreTheMockupPeriodsAtSpeedFiftyAndScaleByFourEitherWay) {
    EXPECT_DOUBLE_EQ(slow_period_secs(Effect::Drift, 50), 32.0);
    EXPECT_DOUBLE_EQ(slow_period_secs(Effect::Glow, 50), 14.0);
    EXPECT_DOUBLE_EQ(slow_period_secs(Effect::Tide, 50), 20.0);
    EXPECT_DOUBLE_EQ(slow_period_secs(Effect::Dusk, 50), 36.0);
    EXPECT_DOUBLE_EQ(slow_period_secs(Effect::Drift, 100), 8.0);
    EXPECT_DOUBLE_EQ(slow_period_secs(Effect::Drift, 0), slow_period_secs(Effect::Drift, 1));
    EXPECT_GT(slow_period_secs(Effect::Drift, 1), 120.0);
}

TEST(SlowEffects, DriftFlowsLeftToRightThroughTheColorsAndWasdFollowsLeft) {
    const Frame f = render(Effect::Drift, kBase, 0.0, 50);
    EXPECT_EQ(f[0], R);
    EXPECT_EQ(f[3], R);
    EXPECT_EQ(f[1], (Rgb{255, 153, 153})); // 0.4 of the way from the last stop back to the first
    EXPECT_EQ(f[2], (Rgb{204, 204, 255}));
    EXPECT_EQ(render(Effect::Drift, kBase, 8.0, 50)[0], G) << "a quarter period moves one stop";
}

TEST(SlowEffects, GlowBreathesBetweenTheFirstTwoColorsAndDimsAtTheLowPoint) {
    EXPECT_EQ(render(Effect::Glow, kBase, 0.0, 50)[0], (Rgb{158, 0, 0})); // first color at 62%
    EXPECT_EQ(render(Effect::Glow, kBase, 7.0, 50)[0], G);                // second color, full
}

TEST(SlowEffects, TideRollsAcrossTheFirstThreeColorsAndWasdHoldsTheFourth) {
    EXPECT_EQ(render(Effect::Tide, kBase, 0.0, 50)[0], G);
    EXPECT_EQ(render(Effect::Tide, kBase, 5.0, 50)[0], B);
    EXPECT_EQ(render(Effect::Tide, kBase, 15.0, 50)[0], R);
    for (double t : {0.0, 3.0, 5.0, 11.5, 15.0}) {
        EXPECT_EQ(render(Effect::Tide, kBase, t, 50)[3], kWhite);
    }
}

TEST(SlowEffects, DuskCrossFadesThroughAllFourColorsAndBack) {
    EXPECT_EQ(render(Effect::Dusk, kBase, 0.0, 50)[0], (Rgb{0, 128, 128}));
    EXPECT_EQ(render(Effect::Dusk, kBase, 9.0, 50)[0], kWhite);
    EXPECT_EQ(render(Effect::Dusk, kBase, 27.0, 50)[0], R);
}

TEST(SlowEffects, TheFrameChangesOnlyTwiceASecond) {
    for (Effect e : {Effect::Drift, Effect::Glow, Effect::Tide, Effect::Dusk}) {
        EXPECT_EQ(render(e, kBase, 0.49, 50), render(e, kBase, 0.0, 50)) << name(e);
        EXPECT_EQ(render(e, kBase, 0.51, 50), render(e, kBase, 0.5, 50)) << name(e);
        EXPECT_NE(render(e, kBase, 0.5, 50), render(e, kBase, 0.0, 50)) << name(e);
    }
}

TEST(SlowEffects, AOneColorProfileStaysThatColor) {
    const Frame one{R, R, R, R};
    EXPECT_EQ(render(Effect::Drift, one, 12.0, 50), one);
    EXPECT_EQ(render(Effect::Tide, one, 12.0, 50), one);
    EXPECT_EQ(render(Effect::Dusk, one, 12.0, 50), one);
}

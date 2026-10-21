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

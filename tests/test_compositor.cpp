#include "core/compositor.hpp"

#include <gtest/gtest.h>

using namespace omenkbd;

namespace {
constexpr Rgb R{255, 0, 0};
constexpr Rgb G{0, 255, 0};
constexpr Rgb B{0, 0, 255};
constexpr Rgb W = kWhite;
constexpr Frame kBase{R, G, B, W};

Inputs inputs() {
    return Inputs{.base = kBase,
                  .effect = Effect::Static,
                  .speed = 50,
                  .animate = false,
                  .overlay = std::nullopt,
                  .hint = std::nullopt,
                  .brightness = 100,
                  .backlight = true};
}
} // namespace

TEST(Compositor, StaticPassthrough) {
    EXPECT_EQ(compose(inputs(), 3.0),
              (Output{.backlight = true, .zones = kBase, .animated = false}));
}

TEST(Compositor, EffectIgnoredWhenNotAnimating) {
    Inputs i = inputs();
    i.effect = Effect::Cycle;
    Output out = compose(i, 0.0);
    EXPECT_EQ(out.zones, kBase);
    EXPECT_FALSE(out.animated);
    i.animate = true;
    out = compose(i, 0.0);
    EXPECT_EQ(out.zones, (Frame{R, G, B, R})) << "cycle at t=0 when animating";
    EXPECT_TRUE(out.animated);
}

TEST(Compositor, TintHalfBlends) {
    Inputs i = inputs();
    i.overlay = ResolvedTint{kBlack, 0.5};
    const Frame z = compose(i, 0.0).zones;
    EXPECT_EQ(z[0], (Rgb{128, 0, 0}));
    EXPECT_EQ(z[3], (Rgb{128, 128, 128}));
}

TEST(Compositor, ZoneOverlayReplacesOnlyNamedZones) {
    Inputs i = inputs();
    i.overlay = ResolvedZones{{Zone::Right}, Rgb{10, 20, 30}, false};
    EXPECT_EQ(compose(i, 0.0).zones, (Frame{R, G, Rgb{10, 20, 30}, W}));
}

TEST(Compositor, HintBeatsEffectAndOverlay) {
    Inputs i = inputs();
    i.effect = Effect::Cycle;
    i.animate = true;
    i.overlay = ResolvedTint{kBlack, 0.5};
    i.hint = Rgb{0x89, 0x26, 0xff};
    const Rgb h{0x89, 0x26, 0xff};
    EXPECT_EQ(compose(i, 1.3).zones, (Frame{h, h, h, h}));
}

TEST(Compositor, BrightnessScalesAndZeroTurnsBacklightOff) {
    Inputs i = inputs();
    i.brightness = 50;
    Output out = compose(i, 0.0);
    EXPECT_EQ(out.zones[0], (Rgb{128, 0, 0}));
    EXPECT_TRUE(out.backlight);
    i.brightness = 0;
    i.effect = Effect::Wave;
    i.animate = true;
    out = compose(i, 0.0);
    EXPECT_FALSE(out.backlight);
    EXPECT_FALSE(out.animated);
}

TEST(Compositor, BacklightInputFalseMeansOffAndNotAnimated) {
    Inputs i = inputs();
    i.backlight = false;
    i.effect = Effect::Wave;
    i.animate = true;
    const Output out = compose(i, 0.0);
    EXPECT_FALSE(out.backlight);
    EXPECT_FALSE(out.animated);
}

TEST(Compositor, AnimatedFalseWithHint) {
    Inputs i = inputs();
    i.effect = Effect::Breathe;
    i.animate = true;
    i.hint = G;
    EXPECT_FALSE(compose(i, 0.0).animated);
}

TEST(Compositor, PulseOverlayAnimatesOnlyWhenAllowed) {
    Inputs i = inputs();
    i.overlay = ResolvedZones{{Zone::Right}, R, true};
    Output out = compose(i, 0.5);
    EXPECT_EQ(out.zones[2], R);
    EXPECT_FALSE(out.animated);
    i.animate = true;
    out = compose(i, 0.5);
    EXPECT_EQ(out.zones[2], (Rgb{51, 0, 0}));
    EXPECT_TRUE(out.animated) << "a pulsing overlay keeps the frame animated";
}

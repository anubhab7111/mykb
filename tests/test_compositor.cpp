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

/// A layer that does `op` on `zone` and shows what is below on the other zones.
LayerOps layer(Zone zone, ZoneOp op) {
    LayerOps ops{};
    ops[index(zone)] = op;
    return ops;
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

TEST(Compositor, LampsDrawAboveTheProfileAndBelowTheHint) {
    constexpr Rgb lamp{255, 128, 0};
    Inputs i = inputs();
    i.lamps = layer(Zone::Wasd, Paint{lamp});
    EXPECT_EQ(compose(i, 0.0).zones, (Frame{R, G, B, lamp})) << "the lamp shows over the profile";

    i.overlay = ResolvedTint{kBlack, 0.5};
    const Frame tinted = compose(i, 0.0).zones;
    EXPECT_EQ(tinted[0], (Rgb{128, 0, 0})) << "the overlay still tints the zones without a lamp";
    EXPECT_EQ(tinted[3], lamp) << "the lamp is drawn after the overlay";

    constexpr Rgb h{0x89, 0x26, 0xff};
    i.hint = h;
    EXPECT_EQ(compose(i, 0.0).zones, (Frame{h, h, h, h})) << "the hint fills every zone, lamp too";
}

TEST(Compositor, TransientDrawsAboveTheHint) {
    constexpr Rgb h{100, 50, 200};
    constexpr Rgb lamp{255, 128, 0};
    constexpr Rgb flash{0, 255, 255};
    Inputs i = inputs();
    i.hint = h;
    i.lamps = layer(Zone::Right, Paint{lamp});
    i.transient = layer(Zone::Left, Paint{flash});
    EXPECT_EQ(compose(i, 0.0).zones, (Frame{flash, h, h, h}))
        << "the clip shows over the hint; the lamp stays hidden under it";

    i.transient = layer(Zone::Middle, Scale{0.5});
    EXPECT_EQ(compose(i, 0.0).zones, (Frame{h, Rgb{50, 25, 100}, h, h}))
        << "the clip's op works on the hint-filled frame, not on the profile";

    i.hint = std::nullopt;
    i.transient = layer(Zone::Right, Paint{flash});
    EXPECT_EQ(compose(i, 0.0).zones, (Frame{R, G, flash, W}))
        << "the clip also covers a lamp when there is no hint";
}

TEST(Compositor, BrightnessScalesLampsAndTransient) {
    Inputs i = inputs();
    i.brightness = 50;
    i.lamps = layer(Zone::Left, Paint{W});
    i.transient = layer(Zone::Middle, Paint{W});
    constexpr Rgb half{128, 128, 128};
    EXPECT_EQ(compose(i, 0.0).zones, (Frame{half, half, Rgb{0, 0, 128}, half}));
}

TEST(Compositor, BrightnessZeroBlanksTheLayersAndTurnsTheBacklightOff) {
    constexpr Rgb lamp{255, 128, 0};
    constexpr Rgb flash{0, 255, 255};
    Inputs i = inputs();
    i.brightness = 0;
    i.effect = Effect::Wave;
    i.animate = true;
    i.lamps = layer(Zone::Wasd, Paint{lamp});
    i.transient = layer(Zone::Left, Paint{flash});
    const Output out = compose(i, 0.5);
    EXPECT_FALSE(out.backlight);
    EXPECT_FALSE(out.animated) << "reactions never animate, and the dark frame is not animated";
    EXPECT_EQ(out.zones, (Frame{kBlack, kBlack, kBlack, kBlack}))
        << "the layers are scaled to black";
}

TEST(Compositor, ABacklightInputOfFalseStillComposesTheLayersButGatesTheOutput) {
    constexpr Rgb lamp{255, 128, 0};
    constexpr Rgb flash{0, 255, 255};
    Inputs i = inputs();
    i.backlight = false;
    i.lamps = layer(Zone::Wasd, Paint{lamp});
    i.transient = layer(Zone::Left, Paint{flash});
    const Output out = compose(i, 0.0);
    EXPECT_FALSE(out.backlight);
    EXPECT_FALSE(out.animated);
    // Only the flag drops: the zones are still the composed frame, layers included.
    EXPECT_EQ(out.zones, (Frame{flash, G, B, lamp}));

    i.backlight = true;
    EXPECT_TRUE(compose(i, 0.0).backlight);
    EXPECT_EQ(compose(i, 0.0).zones, out.zones);
}

TEST(Compositor, MixInTheTransientLayerBlendsTheFrameBelowIt) {
    constexpr Rgb h{100, 50, 200};
    Inputs i = inputs();
    i.transient = layer(Zone::Middle, Mix{kWhite, 0.5});
    EXPECT_EQ(compose(i, 0.0).zones, (Frame{R, Rgb::lerp(G, kWhite, 0.5), B, W}))
        << "the other zones show what is below";

    // On top of the hint and of a lamp: it blends whatever the frame holds at that point.
    i.hint = h;
    EXPECT_EQ(compose(i, 0.0).zones, (Frame{h, Rgb::lerp(h, kWhite, 0.5), h, h}));
    i.hint = std::nullopt;
    i.lamps = layer(Zone::Middle, Paint{Rgb{200, 0, 0}});
    EXPECT_EQ(compose(i, 0.0).zones, (Frame{R, Rgb::lerp(Rgb{200, 0, 0}, kWhite, 0.5), B, W}));

    // Brightness scales the blended result.
    i.brightness = 50;
    EXPECT_EQ(compose(i, 0.0).zones[1], Rgb::lerp(Rgb{200, 0, 0}, kWhite, 0.5).scale(50));
}

TEST(Compositor, ScaleAndMixInTheLampsLayerWorkOnTheOverlaidProfile) {
    Inputs i = inputs();
    i.overlay = ResolvedTint{kBlack, 0.5};
    const Rgb tinted_blue = Rgb::lerp(B, kBlack, 0.5);
    const Rgb tinted_red = Rgb::lerp(R, kBlack, 0.5);

    // A lamp is not only a paint: a Scale dims the zone it covers ...
    i.lamps = layer(Zone::Right, Scale{0.5});
    Frame z = compose(i, 0.0).zones;
    EXPECT_EQ(z[2], multiply(tinted_blue, 0.5));
    EXPECT_EQ(z[0], tinted_red) << "a zone without an op keeps the overlaid profile";

    // ... and a Mix pulls it toward a color.
    i.lamps = layer(Zone::Left, Mix{kWhite, 0.25});
    z = compose(i, 0.0).zones;
    EXPECT_EQ(z[0], Rgb::lerp(tinted_red, kWhite, 0.25));
    EXPECT_EQ(z[2], tinted_blue);

    // The hint still replaces the lamp layer, whatever its op.
    constexpr Rgb h{100, 50, 200};
    i.hint = h;
    EXPECT_EQ(compose(i, 0.0).zones, (Frame{h, h, h, h}));
}

TEST(Compositor, ReactionsNeverMakeTheFrameAnimated) {
    constexpr Rgb lamp{255, 128, 0};
    constexpr Rgb flash{0, 255, 255};
    Inputs i = inputs();
    i.lamps = layer(Zone::Wasd, Paint{lamp});
    i.transient = layer(Zone::Left, Paint{flash});
    for (const bool animate : {false, true}) {
        i.animate = animate;
        const Output out = compose(i, 0.5);
        EXPECT_EQ(out.zones, (Frame{flash, G, B, lamp})) << "the layers are drawn";
        EXPECT_TRUE(out.backlight);
        EXPECT_FALSE(out.animated) << "animate=" << animate;
    }
}

TEST(Compositor, EmptyLayersChangeNothing) {
    Inputs i = inputs();
    i.effect = Effect::Cycle;
    i.speed = 50;
    i.animate = true;
    i.overlay = ResolvedTint{kBlack, 0.5};
    i.brightness = 70;
    i.lamps = LayerOps{};
    i.transient = LayerOps{};
    ASSERT_TRUE(empty(i.lamps));
    ASSERT_TRUE(empty(i.transient));

    Frame expected = render(Effect::Cycle, kBase, 1.3, 50);
    for (Rgb &z : expected) {
        z = Rgb::lerp(z, kBlack, 0.5).scale(70);
    }
    const Output out = compose(i, 1.3);
    EXPECT_EQ(out.zones, expected);
    EXPECT_TRUE(out.animated) << "an animated effect keeps the frame animated";

    constexpr Rgb h{0x89, 0x26, 0xff};
    i.hint = h;
    EXPECT_EQ(compose(i, 1.3).zones, (Frame{h.scale(70), h.scale(70), h.scale(70), h.scale(70)}));
}

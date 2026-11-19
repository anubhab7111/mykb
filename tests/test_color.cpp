#include "helpers.hpp"

#include "core/color.hpp"

#include <gtest/gtest.h>

using namespace omenkbd;
using omenkbd::test::expect_close;

TEST(Color, ParsesHexWithAndWithoutHash) {
    EXPECT_EQ(Rgb::parse("#cba6f7").value(), (Rgb{0xcb, 0xa6, 0xf7}));
    EXPECT_EQ(Rgb::parse("CBA6F7").value(), (Rgb{0xcb, 0xa6, 0xf7}));
}

TEST(Color, RejectsBadHex) {
    for (const char *s : {"", "#fff", "#gggggg", "#1234567", "12345"}) {
        EXPECT_FALSE(Rgb::parse(s).has_value()) << s;
    }
}

TEST(Color, FormatsUppercaseHex) { EXPECT_EQ((Rgb{0xcb, 0xa6, 0xf7}).to_hex(), "CBA6F7"); }

TEST(Color, ScalesWithRounding) {
    EXPECT_EQ((Rgb{255, 128, 0}).scale(50), (Rgb{128, 64, 0}));
    EXPECT_EQ(kWhite.scale(0), kBlack);
    EXPECT_EQ((Rgb{1, 2, 3}).scale(100), (Rgb{1, 2, 3}));
}

TEST(Color, LerpEndpointsAndMid) {
    EXPECT_EQ(Rgb::lerp(kBlack, kWhite, 0.0), kBlack);
    EXPECT_EQ(Rgb::lerp(kBlack, kWhite, 1.0), kWhite);
    EXPECT_EQ(Rgb::lerp(kBlack, kWhite, 0.5), (Rgb{128, 128, 128}));
}

TEST(Color, ThemeColorsAreRejectedWithAHint) {
    const auto c = Rgb::parse("theme:mauve");
    ASSERT_FALSE(c.has_value());
    EXPECT_NE(c.error().find("theme colors were removed"), std::string::npos) << c.error();
}

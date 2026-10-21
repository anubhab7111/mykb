#include "helpers.hpp"

#include "core/palette.hpp"

#include <gtest/gtest.h>

using namespace omenkbd;
using namespace omenkbd::test;

namespace {
constexpr std::string_view kCss = "/* palette */\n@define-color mauve #cba6f7;\n"
                                  "@define-color blue #89b4fa;\n@define-color bad #zzz;\n";
}

TEST(Palette, ParsesWaybarCssSkippingMalformed) {
    const Palette p = Palette::from_waybar_css(kCss);
    EXPECT_EQ(p.get("mauve"), (Rgb{0xcb, 0xa6, 0xf7}));
    EXPECT_EQ(p.size(), 2u);
}

TEST(Palette, ThemeTomlPreferredOverCss) {
    TempDir d;
    write_file(d.path() / "waybar-colors.css", kCss);
    write_file(d.path() / "omen-kbd.toml", "[palette]\nmauve = \"#ff00ff\"\n");
    const auto p = Palette::load(d.path());
    ASSERT_TRUE(p.has_value()) << p.error();
    EXPECT_EQ(p->get("mauve"), (Rgb{255, 0, 255}));
}

TEST(Palette, LoadErrorsWhenNoPaletteFile) {
    TempDir d;
    EXPECT_FALSE(Palette::load(d.path()).has_value());
}

TEST(Palette, ColorRefParse) {
    EXPECT_EQ(ColorRef::parse("theme:mauve").value(), ColorRef::theme("mauve"));
    EXPECT_EQ(ColorRef::parse("#ff0000").value(), ColorRef::hex({255, 0, 0}));
    EXPECT_FALSE(ColorRef::parse("theme:").has_value());
}

TEST(Palette, ResolveVividExactAndMissing) {
    const Palette p = Palette::from_waybar_css(kCss);
    const ColorRef r = ColorRef::theme("mauve");
    EXPECT_EQ(r.resolve(p, ThemeColors::Exact).value(), (Rgb{0xcb, 0xa6, 0xf7}));
    expect_close(r.resolve(p, ThemeColors::Vivid).value(), {0x89, 0x26, 0xff}, 1);
    EXPECT_EQ(ColorRef::hex({0xcb, 0xa6, 0xf7}).resolve(p, ThemeColors::Vivid).value(),
              (Rgb{0xcb, 0xa6, 0xf7}));
    const auto missing = ColorRef::theme("nope").resolve(p, ThemeColors::Vivid);
    ASSERT_FALSE(missing.has_value());
    EXPECT_EQ(missing.error(), "nope");
}

TEST(Palette, StringForms) {
    EXPECT_EQ(ColorRef::parse("theme:teal").value(), ColorRef::theme("teal"));
    EXPECT_EQ(ColorRef::hex({255, 0, 0}).to_string(), "#FF0000");
    EXPECT_EQ(ColorRef::theme("teal").to_string(), "theme:teal");
    EXPECT_FALSE(ColorRef::parse("red").has_value());
    EXPECT_EQ(theme_colors_from_name("exact"), ThemeColors::Exact);
    EXPECT_EQ(theme_colors_from_name("vivid"), ThemeColors::Vivid);
    EXPECT_EQ(theme_colors_from_name("loud"), std::nullopt);
}

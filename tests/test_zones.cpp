#include "core/zones.hpp"

#include <gtest/gtest.h>

using namespace omenkbd;

namespace {
constexpr Rgb R{255, 0, 0};
constexpr Rgb G{0, 255, 0};
constexpr Rgb B{0, 0, 255};
} // namespace

TEST(Zones, DefaultMapMatchesSpec) {
    EXPECT_EQ(ZoneMap().to_hw({R, G, B, kWhite}), (std::array<Rgb, 4>{B, G, R, kWhite}));
}

TEST(Zones, RejectsNonPermutation) {
    EXPECT_FALSE(ZoneMap::make(0, 0, 1, 2).has_value());
    EXPECT_FALSE(ZoneMap::make(0, 1, 2, 4).has_value());
    ASSERT_TRUE(ZoneMap::make(0, 1, 2, 3).has_value());
    EXPECT_EQ(ZoneMap::make(0, 1, 2, 3)->to_hw({R, G, B, kWhite}),
              (std::array<Rgb, 4>{R, G, B, kWhite}));
}

TEST(Zones, ZoneNames) {
    EXPECT_EQ(zone_from_name("WASD"), Zone::Wasd);
    EXPECT_EQ(zone_from_name("center"), Zone::Middle);
    EXPECT_EQ(zone_from_name("top"), std::nullopt);
}

TEST(Zones, NamesAreLowercaseWithCenterAlias) {
    EXPECT_EQ(name(Zone::Middle), "middle");
    EXPECT_EQ(name(Zone::Wasd), "wasd");
    EXPECT_EQ(index(Zone::Right), 2u);
    EXPECT_EQ(zone_from_name("Left"), Zone::Left);
}

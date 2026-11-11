// The one canonical form of a Hyprland window address (R8), shared by the socket2 parser, the
// hyprctl reply and the agent request.

#include "core/window_address.hpp"

#include <gtest/gtest.h>

using namespace omenkbd;

TEST(WindowAddress, IsLowercaseHexWithoutThe0xPrefix) {
    EXPECT_EQ(normalize_window_address("0xABCdef"), "abcdef");
    EXPECT_EQ(normalize_window_address("0X55666F363B20"), "55666f363b20");
    EXPECT_EQ(normalize_window_address("55666F363B20"), "55666f363b20");
    EXPECT_EQ(normalize_window_address("abc"), "abc");
    EXPECT_EQ(normalize_window_address("0x"), "") << "a bare prefix names no window";
    EXPECT_EQ(normalize_window_address(""), "");
    EXPECT_EQ(normalize_window_address("0x0xab"), "0xab") << "only one prefix is stripped";
}

// The one canonical form of a Hyprland window address (R8), shared by the socket2 parser, the
// hyprctl reply and the agent request.

#include "core/window_address.hpp"

#include <gtest/gtest.h>

#include <string>

using namespace omenkbd;

TEST(WindowAddress, IsLowercaseHexWithoutThe0xPrefix) {
    EXPECT_EQ(normalize_window_address("0xABCdef"), "abcdef");
    EXPECT_EQ(normalize_window_address("0X55666F363B20"), "55666f363b20");
    EXPECT_EQ(normalize_window_address("55666F363B20"), "55666f363b20");
    EXPECT_EQ(normalize_window_address("abc"), "abc");
    EXPECT_EQ(normalize_window_address("0x"), "") << "a bare prefix names no window";
    EXPECT_EQ(normalize_window_address(""), "");
    EXPECT_EQ(normalize_window_address("0x0xab"), "") << "only one prefix is stripped";
}

// T14-5: an address is 1 to 64 hex digits after an optional `0x`; anything else is invalid, and
// invalid comes back empty, like an empty address (callers that care tell the two apart first).
TEST(WindowAddress, AnythingButOneTo64HexDigitsIsInvalid) {
    const std::string longest(kMaxWindowAddressDigits, 'F');
    EXPECT_EQ(normalize_window_address(longest), std::string(kMaxWindowAddressDigits, 'f'));
    EXPECT_EQ(normalize_window_address("0x" + longest), std::string(kMaxWindowAddressDigits, 'f'))
        << "the prefix does not count toward the length";
    EXPECT_EQ(normalize_window_address("0"), "0");
    EXPECT_EQ(normalize_window_address("0x0"), "0");
    EXPECT_EQ(normalize_window_address("0123456789abcdefABCDEF"), "0123456789abcdefabcdef");

    for (const std::string &bad : {
             std::string(kMaxWindowAddressDigits + 1, 'a'),
             "0x" + std::string(kMaxWindowAddressDigits + 1, 'a'),
             std::string("zz"),
             std::string("a1-b"),
             std::string("w1"),
             std::string(" ab"),
             std::string("ab "),
             std::string("ab\n"),
             std::string("a b"),
             std::string("-1"),
             std::string("+ab"),
             std::string("x"),
             std::string("0xg"),
             std::string("ab", 3), // an embedded NUL
             std::string("\xC3\xA9"),
         }) {
        EXPECT_EQ(normalize_window_address(bad), "") << '"' << bad << '"';
    }
}

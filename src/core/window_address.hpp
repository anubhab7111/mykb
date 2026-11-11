#pragma once
// The one canonical form of a Hyprland window address, shared by every place an address enters
// the daemon: the socket2 parser, the hyprctl reply and the agent request (`to_signal`).

#include "util/strings.hpp"

#include <algorithm>
#include <cstddef>
#include <string>
#include <string_view>

namespace omenkbd {

/// The most hex digits an address may have (after the optional `0x`).
inline constexpr std::size_t kMaxWindowAddressDigits = 64;

/// R8: lowercase hex without the `0x` that `hyprctl clients -j` prints (socket2 omits it). A valid
/// address is an optional `0x` or `0X`, then 1 to `kMaxWindowAddressDigits` hex digits. Anything
/// else (an empty address, a bare `0x`, a second prefix, any other character, too many digits)
/// gives an empty result. An empty input and an invalid one both come back empty: a caller for
/// which an empty address means something checks the input first.
inline std::string normalize_window_address(std::string_view address) {
    if (address.starts_with("0x") || address.starts_with("0X")) {
        address.remove_prefix(2);
    }
    const auto is_hex = [](char c) {
        return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
    };
    if (address.empty() || address.size() > kMaxWindowAddressDigits ||
        !std::ranges::all_of(address, is_hex)) {
        return {};
    }
    return to_lower(address);
}

} // namespace omenkbd

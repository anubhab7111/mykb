#pragma once
// The one canonical form of a Hyprland window address, shared by the two places an address enters
// the daemon: the socket2 parser and the agent request (`to_signal`).

#include "util/strings.hpp"

#include <string>
#include <string_view>

namespace omenkbd {

/// R8: lowercase hex without the `0x` that `hyprctl clients -j` prints (socket2 omits it). The
/// result is empty for an empty address or a bare `0x`; callers decide what that means.
inline std::string normalize_window_address(std::string_view address) {
    std::string out = to_lower(address);
    if (out.starts_with("0x")) {
        out.erase(0, 2);
    }
    return out;
}

} // namespace omenkbd

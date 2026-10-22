#pragma once
// `omen-kbd gen-hyprland`: the Lua module that turns held modifier sets into mode colors.

#include "config.hpp"

#include <filesystem>
#include <string>
#include <string_view>

namespace omenkbd {

/// The kbdlight.lua module for Hyprland's Lua config, with the modifier sets from
/// `[hints.modifiers]` and the hold delay from `hints.delay_ms`. The algorithm is the one the
/// spike validated (spikes/RESULTS.md): a self-disabling repeat ticker, a non-modifier press
/// cancels a hint that has not shown yet, and a generation guard against duplicate copies.
/// `command` is how the hook runs omen-kbd (through `sh -c`); see hook_command().
std::string gen_hyprland(const Config &cfg, std::string_view command = "omen-kbd");

/// `exe` when it is an absolute path made only of letters, digits and `/._+-` (safe inside a Lua
/// string and `sh -c` without quoting), else plain "omen-kbd" (which must then be on Hyprland's
/// PATH).
std::string hook_command(const std::filesystem::path &exe);

} // namespace omenkbd

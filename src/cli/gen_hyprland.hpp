#pragma once
// `omen-kbd gen-hyprland`: the Lua module that turns held modifier sets into mode colors.

#include "config.hpp"

#include <string>

namespace omenkbd {

/// The kbdlight.lua module for Hyprland's Lua config, with the modifier sets from
/// `[hints.modifiers]` and the hold delay from `hints.delay_ms`. The algorithm is the one the
/// spike validated (spikes/RESULTS.md): a self-disabling repeat ticker, a non-modifier press
/// cancels a hint that has not shown yet, and a generation guard against duplicate copies.
std::string gen_hyprland(const Config &cfg);

} // namespace omenkbd

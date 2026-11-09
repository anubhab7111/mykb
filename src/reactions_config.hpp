#pragma once
// The `[reactions]` table of the user's config: the spec's defaults and a strict parser.

#include "core/reactions_cfg.hpp"

#include <expected>
#include <string>
#include <toml++/toml.hpp>

namespace omenkbd {

/// The block of spec 5.11 as built-in defaults, with `enabled = false` (a missing table means off).
ReactionsCfg default_reactions();

/// Parses a `[reactions]` table over the defaults; `enabled` is true unless the table says
/// `enabled = false`, because the table is present.
///
/// Strict: unknown keys, wrong types and out-of-range values are errors that name the field, as
/// `reactions: ...` or `reactions.<table>: ...`. The ranges are the spec's, and each is enforced
/// here because nothing downstream bounds them (a clip loops `blinks - 1` times). A sub-table that
/// is missing keeps its defaults, and `notify.apps`, when present, replaces the default apps.
std::expected<ReactionsCfg, std::string> parse_reactions(const toml::table &t);

} // namespace omenkbd

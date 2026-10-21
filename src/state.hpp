#pragma once
// Runtime state the daemon persists between restarts (`~/.local/state/omen-kbd/state.json`).
// Writes are atomic (tmp file + fsync + rename) so a crash never leaves a half-written file.
// JSON shape is identical to the Rust prototype's.

#include "core/effects.hpp"

#include <cstdint>
#include <expected>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace omenkbd {

/// A runtime override of the active profile's effect (`omen-kbd effect …`).
struct EffectOverride {
    Effect effect;
    std::uint8_t speed;
    friend bool operator==(const EffectOverride &, const EffectOverride &) = default;
};

/// Unknown JSON fields are ignored and missing ones get defaults.
struct PersistedState {
    std::string active_profile;
    /// Colors set with `omen-kbd set` (1 or 4 color strings), shown as profile `custom`.
    std::optional<std::vector<std::string>> custom;
    std::optional<EffectOverride> effect_override;
    std::uint8_t brightness = 100;
    bool on = true;
    std::uint64_t lifetime_writes = 0;

    static PersistedState default_for(std::string_view first_profile);

    /// A missing file gives defaults; a corrupt one is renamed to `<name>.bad` and reported in
    /// the returned warning.
    static std::pair<PersistedState, std::optional<std::string>>
    load(const std::filesystem::path &path, std::string_view first_profile);

    /// Atomically replaces the file: write `<name>.tmp`, fsync, rename. Creates parent dirs.
    [[nodiscard]] std::expected<void, std::string> save(const std::filesystem::path &path) const;

    friend bool operator==(const PersistedState &, const PersistedState &) = default;
};

/// `$XDG_STATE_HOME/omen-kbd/state.json` (default `~/.local/state/omen-kbd/state.json`).
std::filesystem::path state_path();

} // namespace omenkbd

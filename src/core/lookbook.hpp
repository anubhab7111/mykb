#pragma once
// How each signal looks on the keyboard, decided once from the `[reactions]` settings: the clips
// (built with the clip builders), the lamp styles, the limits for the clip player, and the
// notification policy (which app gets which color, and which notifications are skipped). Colors are
// resolved when the look book is built, so nothing downstream touches the palette. Pure: no clock.

#include "core/clip.hpp"
#include "core/clip_player.hpp"
#include "core/color.hpp"
#include "core/lamps.hpp"
#include "core/palette.hpp"
#include "core/reactions_cfg.hpp"
#include "core/signals.hpp"

#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <string_view>

namespace omenkbd {

/// Turns a color reference into a color; the daemon passes the current theme's palette.
using Resolver = std::function<Rgb(const ColorRef &)>;

class LookBook {
  public:
    /// Resolves every color of `cfg` now, once each, and keeps no reference to either argument.
    /// `cfg` is in range, as `parse_reactions` guarantees. Its app keys may have any case: the look
    /// book lowercases them itself. Of two keys that are equal once lowercased it keeps the one
    /// that sorts first and ignores the other silently (the parser rejects such a table).
    LookBook(const ReactionsCfg &cfg, const Resolver &resolve);

    /// The sweep for a switch from workspace `from` to `to`: `Up` when `to > from`, else `Down`.
    /// `nullopt` when they are equal.
    [[nodiscard]] std::optional<Clip> workspace(int from, int to) const;

    /// `lift(window.open)`; `nullopt` when `open <= 0`.
    [[nodiscard]] std::optional<Clip> window_opened() const;

    /// `dip(window.close)`; `nullopt` when `close >= 1`.
    [[nodiscard]] std::optional<Clip> window_closed() const;

    /// One blink on all four zones, or `nullopt` when the notification is skipped.
    ///
    /// Three identities stand for the app, lowercased and in this order: its name, its desktop
    /// entry, and the entry's last dot-separated segment. An empty one is not an identity (a bare
    /// `omen-kbd notify` has no app, and an entry without a dot has no last segment), so it matches
    /// nothing in the app table, and an empty `focused_class` (no focus event yet) matches no
    /// identity.
    ///   - Skipped when `urgency < min_urgency`.
    ///   - Skipped when `skip_if_focused` is set and `focused_class`, lowercased, equals an
    ///     identity.
    ///   - Color: `critical` for a critical notification; else the app table's color for the first
    ///     identity it holds; else `color`.
    [[nodiscard]] std::optional<Clip> notified(const signal::Notified &n,
                                               std::string_view focused_class) const;

    /// One blink of the `done` color on the agent zones; `nullopt` when those are empty.
    [[nodiscard]] std::optional<Clip> agent_done() const;

    /// The attention light: no TTL.
    [[nodiscard]] LampStyle urgent_style() const;

    /// The Claude lamp, in the `waiting` color; its keys expire after `ttl_s`.
    [[nodiscard]] LampStyle agent_style() const;

    /// `max_per_minute`, a window cooldown of `window.cooldown_ms`, and a gap of `blink_ms`.
    [[nodiscard]] PlayerLimits limits() const;

  private:
    struct NotifyLook {
        Rgb color;
        Rgb critical;
        Urgency min_urgency;
        bool skip_if_focused;
        std::map<std::string, Rgb, std::less<>> apps;
    };

    Duration step_;
    Duration blink_;
    std::uint32_t max_per_minute_;
    Duration window_cooldown_;
    Rgb workspace_color_;
    WasdWith wasd_with_;
    double window_open_;
    double window_close_;
    NotifyLook notify_;
    LampStyle urgent_;
    LampStyle agent_;
    Rgb agent_done_;
};

} // namespace omenkbd

#pragma once
// The keyboard reactions as one object: signals go in, two layers come out. `lamps` holds the
// lights that stay (the attention light and the Claude lamp), `transient` the short reaction
// playing now (a sweep, a lift, a flash). It joins the look book (how a signal looks), the clip
// player and the lamp set, and adds the rules that need to know what is focused and whether anyone
// would see the keyboard. It keeps no clock: callers pass `now`.

#include "core/clip.hpp"
#include "core/clip_player.hpp"
#include "core/clock.hpp"
#include "core/lamps.hpp"
#include "core/lookbook.hpp"
#include "core/reactions_cfg.hpp"
#include "core/reactions_status.hpp"
#include "core/signals.hpp"

#include <optional>
#include <string>

namespace omenkbd {

/// What the model knows about who could see a reaction.
struct Visibility {
    /// The backlight is on and the composed frame is not dark.
    bool keyboard_visible;
    /// Not locked and not idle.
    bool user_present;
};

struct Layers {
    LayerOps lamps;
    LayerOps transient;
};

class Reactions {
  public:
    /// Disabled, until `reconfigure` says otherwise.
    Reactions();

    /// Takes new settings. Lamp keys stay, with the new styles at once; a clip already playing
    /// finishes with the look it started with, and the next one has the new look. `enabled = false`
    /// drops every lamp and clip, and forgets the focused window. `resolve` is only used during the
    /// call.
    void reconfigure(const ReactionsCfg &cfg, const Resolver &resolve);

    /// The palette changed: re-resolves the colors of the settings kept by the last `reconfigure`.
    /// Keys, intros in flight and playing clips keep their state. `resolve` is only used during the
    /// call.
    void restyle(const Resolver &resolve);

    /// Takes the signal `sig` at `now`. Ignored while disabled. Short reactions and lamp intros
    /// need `short_ok`, which is a visible keyboard and a present user; without it a lamp lights
    /// steadily at once and a clip is not played. An empty window address counts as no window: an
    /// `UrgentRaised` without one is dropped, and a `FocusChanged` to none clears no light.
    void on_signal(const Signal &sig, TimePoint now, Visibility visibility);

    /// Moves time on and applies the visibility: expires lamps, advances the player and drops the
    /// clips while a short reaction would not be seen. Call it once per frame, before `layers`.
    void update(TimePoint now, Visibility visibility);

    /// Drops the playing and the queued clip. Lamps stay.
    void suppress();

    /// What to draw at `now`. The lamps layer is empty unless the last visibility had a visible
    /// keyboard: the composed frame still says "backlight on" after the Fn key, so a lamp drawn
    /// then would only be written to zones nobody sees. When the keyboard shows again, the lamps
    /// show steady, at the cost of one write.
    [[nodiscard]] Layers layers(TimePoint now) const;

    /// When the layers next change on their own (a clip step, an intro phase, a TTL expiry), as of
    /// the last `on_signal` or `update`. A new `reconfigure` or `suppress` shows in it at the next
    /// call of either. A time that is not after `now` means `update(now)` is overdue; `nullopt`
    /// when nothing will change.
    [[nodiscard]] std::optional<TimePoint> next_change() const;

    /// True when `visibility` is not the one the last `on_signal` or `update` took, so the layers
    /// may be stale (a lamp hidden or shown): the caller then treats `update` as overdue, and
    /// `Model::reaction_deadline` returns a time long past (`TimePoint{}`). Before any call nothing
    /// is held, and only a visible keyboard counts as a change.
    [[nodiscard]] bool visibility_differs(Visibility visibility) const;

    [[nodiscard]] ReactionsStatus status() const;

  private:
    /// The part of `on_signal` and `update` before anything else: remembers the visibility, expires
    /// lamps, retires finished clips and drops the rest while short reactions are off.
    void tick(TimePoint now, Visibility visibility);

    /// Caches the nearest change of the lamps and the player.
    void refresh_next_change(TimePoint now);

    void handle(const Signal &sig, TimePoint now);

    /// Offers `clip` on `channel`, unless there is none or short reactions are off.
    void offer(Channel channel, std::optional<Clip> clip, TimePoint now);

    [[nodiscard]] bool short_ok() const;

    /// True when `window` is the focused window. No window is never focused, and before the first
    /// focus event nothing is.
    [[nodiscard]] bool is_focused(const std::optional<std::string> &window) const;

    ReactionsCfg cfg_;
    LookBook look_;
    ClipPlayer player_;
    LampSet lamps_;
    std::string focused_window_;
    std::string focused_class_;
    Visibility visibility_{false, false};
    /// `visibility_` was taken by an `on_signal` or `update`.
    bool visibility_taken_ = false;
    std::optional<TimePoint> next_change_;
};

} // namespace omenkbd

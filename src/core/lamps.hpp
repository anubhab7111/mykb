#pragma once
// Keyed lights on the keyboard. A lamp kind blinks once when it first lights, then holds steady
// until its keys are cleared or expire. Keys can be linked to a window, so one call clears every
// key of that window. Pure: a lamp set keeps no clock, callers pass `now`.

#include "core/clip.hpp"
#include "core/clock.hpp"
#include "core/color.hpp"
#include "core/zones.hpp"

#include <array>
#include <chrono>
#include <cstddef>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace omenkbd {

/// Agent outranks Urgent where their zones overlap.
enum class LampKind { Agent, Urgent };

/// The most keys one kind holds. A key lives for the kind's TTL (an hour for the agent lamp) and
/// `LampSet::next_change` visits every key on every frame, so an unbounded producer would grow
/// memory and per-frame work. Past the bound a new key evicts the kind's oldest one.
inline constexpr std::size_t kMaxKeysPerKind = 64;

/// "agent" or "urgent".
std::string_view name(LampKind kind);

struct LampStyle {
    /// Where the lamp shows. Empty disables the kind: raises are ignored.
    std::vector<Zone> zones;
    Rgb color{};
    /// Lit phases of the intro: ON, OFF, ON, ..., the last one steady. 0 and 1 both light steadily
    /// at once, with no intro.
    int blinks = 2;
    /// How long each lit and each dark phase of the intro lasts.
    Duration blink = std::chrono::milliseconds(150);
    /// A key goes away this long after it was last raised; none means keys stay until cleared.
    std::optional<Duration> ttl;
};

class LampSet {
  public:
    /// Takes effect at once for the keys already raised. Keys, window links, raise times and the
    /// progress of an intro in flight are kept.
    void set_style(LampKind kind, LampStyle style);

    /// Lights `kind` for `key`, linked to `window` when given. A kind that had no keys starts an
    /// intro at `now` when `play_intro` is set; a kind already lit never does. Raising a key the
    /// kind already holds only refreshes its TTL. Ignored when the kind has no zones: it changes
    /// nothing.
    ///
    /// A kind holds at most `kMaxKeysPerKind` keys: a new key raised past that evicts the kind's
    /// oldest one first (the smallest `raised_at`; on a tie the smallest key), with its window
    /// link. The kind stays lit through it, so the intro and the paint do not change.
    ///
    /// A window shows one light, the `Agent` one:
    ///   - an `Urgent` raise is ignored when an `Agent` key already carries its `window`;
    ///   - an `Agent` raise drops every `Urgent` key carrying its `window`, whether the key is new
    ///     or already held.
    /// So a window never holds keys of both kinds. A raise without a window never meets a rule.
    void raise(LampKind kind, std::string key, std::optional<std::string> window, TimePoint now,
               bool play_intro);

    /// Drops one key; the kind goes dark with its last key.
    void clear(LampKind kind, std::string_view key);

    /// Drops every key linked to `window`.
    void clear_window(std::string_view window);

    /// Drops every key of `kind`.
    void clear_kind(LampKind kind);

    /// Drops the keys whose TTL has run out by `now`. `layer` and `next_change` do not: call this
    /// first, as the loop does when `next_change` says a time that is not after `now`.
    void expire(TimePoint now);

    /// What the lamps do at `now`: `Urgent` paints first, `Agent` over it. A kind in its intro
    /// shows the phase at `now`; once the intro is over, or when it never played, it paints its
    /// zones steadily. A late sample therefore shows the steady paint and never replays a blink.
    [[nodiscard]] LayerOps layer(TimePoint now) const;

    /// When `layer` or `expire` next have something to do: the next intro boundary where the paint
    /// changes, or the earliest TTL expiry. `nullopt` when nothing will change. A time that is not
    /// after `now` means `expire(now)` is overdue.
    [[nodiscard]] std::optional<TimePoint> next_change(TimePoint now) const;

    /// The kinds that hold a key, `Agent` first.
    [[nodiscard]] std::vector<LampKind> active() const;

    /// True when no kind holds a key.
    [[nodiscard]] bool empty() const;

    /// How many keys `kind` holds; never more than `kMaxKeysPerKind`.
    [[nodiscard]] std::size_t key_count(LampKind kind) const;

  private:
    struct Key {
        std::optional<std::string> window;
        /// The last raise.
        TimePoint raised_at;
    };
    struct Slot {
        LampStyle style;
        /// By key; `std::less<>` lets `clear` look a key up by string_view.
        std::map<std::string, Key, std::less<>> keys;
        /// When the intro began; none when the kind lit without one. Meaningful while `keys` is
        /// not empty.
        std::optional<TimePoint> intro_start;
    };

    [[nodiscard]] Slot &slot(LampKind kind);
    [[nodiscard]] const Slot &slot(LampKind kind) const;

    std::array<Slot, 2> slots_;
};

} // namespace omenkbd

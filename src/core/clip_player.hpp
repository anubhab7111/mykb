#pragma once
// Plays short keyboard reactions one at a time: one playing slot, one queued clip, a policy per
// channel and a rate limit over a rolling minute. Pure: it keeps no clock, callers pass `now`.

#include "core/clip.hpp"
#include "core/clock.hpp"

#include <chrono>
#include <cstdint>
#include <deque>
#include <optional>
#include <string_view>

namespace omenkbd {

/// Where a short reaction comes from. Each channel has its own policy in `ClipPlayer::offer`.
enum class Channel { Workspace, Window, Notify, AgentDone };

/// "workspace", "window", "notify" or "agent-done".
std::string_view name(Channel c);

/// What `ClipPlayer::offer` did with a clip.
enum class Offer { Started, Queued, Dropped };

struct PlayerLimits {
    /// At most this many clips start in any rolling minute, all channels together.
    std::uint32_t max_per_minute = 30;
    /// A window clip is dropped until this long after the last window clip started.
    Duration window_cooldown = std::chrono::milliseconds(500);
    /// A queued clip starts this long after the playing one ends, so two identical clips in a row
    /// stay two clips.
    Duration gap = std::chrono::milliseconds(150);
};

class ClipPlayer {
  public:
    explicit ClipPlayer(PlayerLimits limits = {});

    /// Takes effect at the next offer or start; a clip already queued keeps the gap it was queued
    /// with.
    void set_limits(PlayerLimits limits);

    /// Starts `clip` now, queues it behind the playing one, or drops it. It first retires whatever
    /// has finished by `now`, as `advance` does. Per channel:
    ///   - `Workspace`: dropped while a clip plays or waits in the queue.
    ///   - `Window`: as `Workspace`, and dropped until `window_cooldown` after the last window clip
    ///     started.
    ///   - `Notify`, `AgentDone`: start when idle; otherwise queued if the queue is empty, else
    ///     dropped.
    /// An offer that would start now is dropped when `max_per_minute` starts already fall in the
    /// minute before `now`.
    Offer offer(Channel channel, Clip clip, TimePoint now);

    /// Retires the playing clip once it is over, and starts the queued clip once `gap` has passed
    /// since it ended. A queued clip that is already late starts at `now`, never earlier: a time
    /// jump (suspend, a stalled loop) does not replay or catch up anything. A queued clip the rate
    /// limit does not let start is discarded.
    void advance(TimePoint now);

    /// The ops of the playing clip's step showing at `now`; empty when idle or once the clip is
    /// over. A `now` before the clip's start shows its first step.
    [[nodiscard]] LayerOps layer(TimePoint now) const;

    /// When `layer` or `playing` next change on their own: the end of the showing step (the clip's
    /// end for the last one), or when a queued clip is due. `nullopt` when idle. A time that is not
    /// after `now` means `advance(now)` is overdue.
    [[nodiscard]] std::optional<TimePoint> next_change(TimePoint now) const;

    /// Back to idle: stops the playing clip, forgets the queued one, and forgets the window
    /// cooldown and the starts counted for the rate limit. The limits stay.
    void clear();

    /// The channel of the clip in the playing slot, until `advance` retires it.
    [[nodiscard]] std::optional<Channel> playing() const;

  private:
    struct Playing {
        Channel channel;
        Clip clip;
        TimePoint start;
        /// When the last step ends.
        [[nodiscard]] TimePoint end() const;
    };
    struct Queued {
        Channel channel;
        Clip clip;
        /// When it may start: the playing clip's end plus the gap in force when it was queued.
        TimePoint due;
    };

    /// Puts `clip` in the playing slot at `now` unless `max_per_minute` starts already fall in the
    /// minute before `now`; then it returns false and the clip is lost. The caller has checked
    /// that the slot is free.
    bool try_start(Channel channel, Clip clip, TimePoint now);

    PlayerLimits limits_;
    std::optional<Playing> playing_;
    std::optional<Queued> queued_;
    /// Start times within the last minute, oldest first.
    std::deque<TimePoint> starts_;
    std::optional<TimePoint> last_window_start_;
};

} // namespace omenkbd

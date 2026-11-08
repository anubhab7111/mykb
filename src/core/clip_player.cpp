#include "core/clip_player.hpp"

#include <algorithm>
#include <utility>

namespace omenkbd {

namespace {

constexpr Duration kMinute = std::chrono::minutes(1);

} // namespace

std::string_view name(Channel c) {
    switch (c) {
    case Channel::Workspace:
        return "workspace";
    case Channel::Window:
        return "window";
    case Channel::Notify:
        return "notify";
    case Channel::AgentDone:
        return "agent-done";
    }
    return "?";
}

ClipPlayer::ClipPlayer(PlayerLimits limits) : limits_(limits) {}

void ClipPlayer::set_limits(PlayerLimits limits) { limits_ = limits; }

Offer ClipPlayer::offer(Channel channel, Clip clip, TimePoint now) {
    // The loop may handle a signal just before the timer that would retire a finished clip.
    advance(now);
    const bool idle = !playing_ && !queued_;
    switch (channel) {
    case Channel::Window:
        if (last_window_start_ && now < *last_window_start_ + limits_.window_cooldown) {
            return Offer::Dropped;
        }
        [[fallthrough]];
    case Channel::Workspace:
        return idle && try_start(channel, std::move(clip), now) ? Offer::Started : Offer::Dropped;
    case Channel::Notify:
    case Channel::AgentDone:
        if (idle) {
            return try_start(channel, std::move(clip), now) ? Offer::Started : Offer::Dropped;
        }
        if (queued_) {
            return Offer::Dropped;
        }
        // Not idle and nothing queued, so a clip is playing.
        queued_ = Queued{channel, std::move(clip), playing_->end() + limits_.gap};
        return Offer::Queued;
    }
    return Offer::Dropped;
}

void ClipPlayer::advance(TimePoint now) {
    if (playing_ && now >= playing_->end()) {
        playing_.reset();
    }
    if (!playing_ && queued_ && now >= queued_->due) {
        Queued next = std::move(*queued_);
        queued_.reset();
        // A late clip starts now, from its first step. One the rate limit refuses is discarded.
        try_start(next.channel, std::move(next.clip), now);
    }
}

LayerOps ClipPlayer::layer(TimePoint now) const {
    if (!playing_) {
        return {};
    }
    return playing_->clip.ops_at(std::max(now - playing_->start, Duration::zero()));
}

std::optional<TimePoint> ClipPlayer::next_change(TimePoint now) const {
    if (playing_) {
        const Duration elapsed = std::max(now - playing_->start, Duration::zero());
        // Past the last step the clip is over but not yet retired: it is due at its end.
        return playing_->start +
               playing_->clip.next_boundary(elapsed).value_or(playing_->clip.total());
    }
    if (queued_) {
        return queued_->due;
    }
    return std::nullopt;
}

void ClipPlayer::clear() {
    playing_.reset();
    queued_.reset();
    starts_.clear();
    last_window_start_.reset();
}

std::optional<Channel> ClipPlayer::playing() const {
    return playing_.transform([](const Playing &p) { return p.channel; });
}

TimePoint ClipPlayer::Playing::end() const { return start + clip.total(); }

bool ClipPlayer::try_start(Channel channel, Clip clip, TimePoint now) {
    // Forget starts that left the minute, then count what is left.
    while (!starts_.empty() && now - starts_.front() >= kMinute) {
        starts_.pop_front();
    }
    if (starts_.size() >= limits_.max_per_minute) {
        return false;
    }
    starts_.push_back(now);
    if (channel == Channel::Window) {
        last_window_start_ = now;
    }
    playing_ = Playing{channel, std::move(clip), now};
    return true;
}

} // namespace omenkbd

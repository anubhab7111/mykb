#pragma once
// A stand-in for the daemon's frame loop, for the write-count tests (spec section 6). It drives a
// `Model` with a fake clock the way the daemon does: a fact, a request or a backlight reading is
// applied and the frame composed at once; a signal too, except while an effect ticks, when the next
// tick samples it (Ruling P16). Between events it wakes only where `Daemon::next_wake` would: at
// the effect's next tick while the output is animated, otherwise at the model's reaction deadline.
// It counts what the writer would write: one write per change of (backlight, hardware zones),
// because the daemon skips an identical frame.

#include "config.hpp"
#include "core/compositor.hpp"
#include "daemon/model.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace omenkbd::test {

/// A static profile `plain` (the first, so the rig starts on it), an animated profile `wavy`,
/// reactions with hex colors and the spec's timings, and a fullscreen rule. No `[gates]` table, so
/// a test can append one.
constexpr std::string_view kRigConfig = R"(
[effects]
enabled = true
fps = 10

[reactions]
enabled = true

[reactions.workspace]
color = "#a0a0ff"

[reactions.notify]
color = "#2040ff"
critical = "#ff2020"

[reactions.notify.apps]
zapzap = "#20ff40"

[reactions.urgent]
color = "#ffe020"

[reactions.agent]
waiting = "#ff9040"
done = "#40ff90"
ttl_s = 3600

[[profiles]]
name = "plain"
colors = ["#101010", "#202020", "#303030", "#404040"]

[[profiles]]
name = "wavy"
colors = ["#101010", "#202020", "#303030", "#404040"]
effect = "cycle"

[[rules]]
name = "fullscreen video dims"
when = { fullscreen_app = ["mpv"] }
brightness_cap = 15
)";

class ReactionRig {
  public:
    /// Starts on the config's first profile and composes the first frame, which is not a write.
    explicit ReactionRig(std::string_view config_toml = kRigConfig)
        : model_(make_model(config_toml)) {
        sample();
    }

    Model &model() { return model_; }

    /// The fake clock.
    [[nodiscard]] TimePoint now() const { return now_; }

    /// A signal arrives: the daemon applies it and recomputes, except while an effect ticks
    /// (Ruling P16): then the next tick samples it, so a signal adds no write on an animated base.
    void signal(const Signal &s) {
        model_.apply_signal(s, now_);
        if (!next_tick_) {
            sample();
        }
    }

    /// A fact changes: the daemon applies it and recomputes.
    void fact(const FactUpdate &f) {
        model_.apply_fact(f);
        sample();
    }

    /// A client request: the daemon recomputes unless nothing changed. A power request always
    /// writes the backlight (`force_backlight_next_` in `Daemon::on_request`), because the Fn key
    /// may have changed it behind the daemon's back.
    void request(const Request &r) {
        const auto change = model_.handle(r, now_);
        if (!change) {
            ADD_FAILURE() << "request failed: " << change.error();
            return;
        }
        if (*change != Change::None) {
            force_backlight_ = force_backlight_ || std::holds_alternative<req::Power>(r);
            sample();
        }
    }

    /// A backlight reading arrives (a probe result or the Fn-key event): applied, then recomputed.
    void set_hw_backlight(bool on) {
        model_.set_hw_backlight(on, now_);
        sample();
    }

    /// `omen-kbd reload`: the daemon replaces the config, then recomputes.
    void reload(std::string_view config_toml) {
        model_.replace_config(parse_config(config_toml).value(), Palette{});
        sample();
    }

    /// Lets `d` pass, waking only where the daemon would (`Daemon::next_wake`): at the effect's
    /// next tick while the last output was animated (the ticks sample the reactions), otherwise at
    /// the reaction deadline. The end of `d` is no wake, so a deadline that comes late shows as a
    /// missing or late write.
    void run_for(Duration d) {
        const TimePoint end = now_ + d;
        int stalls = 0;
        for (std::optional<TimePoint> wake = next_wake(); wake && *wake <= end;
             wake = next_wake()) {
            // A deadline that is not after now is overdue: the daemon wakes at once.
            const TimePoint next = std::max(*wake, now_);
            if (next > now_) {
                stalls = 0;
            } else if (++stalls > 1) {
                ADD_FAILURE() << "the reaction deadline stays at or before now after a sample";
                break;
            }
            now_ = next;
            sample();
        }
        now_ = end;
    }

    /// The frames written so far: each change after the first frame.
    [[nodiscard]] std::size_t writes() const { return frames_.size(); }

    /// What each write showed, in logical zone order (left, middle, right, wasd).
    [[nodiscard]] const std::vector<Frame> &frames() const { return frames_; }

    /// When each frame of `frames()` was written, as a time since the rig started.
    [[nodiscard]] const std::vector<Duration> &frame_times() const { return frame_times_; }

  private:
    static Model make_model(std::string_view config_toml) {
        Config config = parse_config(config_toml).value();
        const std::string first = config.profiles.front().name;
        return Model(std::move(config), Palette{}, PersistedState::default_for(first), "mock");
    }

    [[nodiscard]] Duration tick() const {
        return std::chrono::milliseconds(1000 / std::max(1, int{model_.config().effects.fps}));
    }

    /// Where `Daemon::next_wake` would wake for the frame: the next tick while animated, otherwise
    /// the reaction deadline (which the daemon considers only while the output is not animated).
    [[nodiscard]] std::optional<TimePoint> next_wake() const {
        return next_tick_ ? next_tick_ : model_.reaction_deadline();
    }

    /// `Daemon::recompute()` (daemon.cpp lines 140-172 when this was written: the job and the
    /// forget at 147-158, the tick at 161-163), in its order: compose the frame at `now_` with what
    /// the model knows; when the job is sent, forget the backlight reading if the job is forced or
    /// changes the backlight (the reading no longer holds once the daemon writes the backlight
    /// itself); then schedule the next tick. A write is counted when the (backlight, hardware
    /// zones) key changes: a forced job with the same key rewrites only the backlight. While
    /// suspended there is no frame and nothing ticks.
    void sample() {
        const std::optional<Inputs> in = model_.inputs(now_);
        if (!in) {
            next_tick_.reset();
            return;
        }
        const Output out = compose(*in, std::chrono::duration<double>(now_ - start_).count());
        const std::pair key{out.backlight, model_.config().device.zone_map.to_hw(out.zones)};
        const bool force = std::exchange(force_backlight_, false);
        if (force || last_ != key) {
            if (force || !last_ || last_->first != out.backlight) {
                model_.forget_hw_backlight();
            }
            if (last_ && *last_ != key) {
                frames_.push_back(out.zones);
                frame_times_.push_back(now_ - start_);
            }
            last_ = key;
        }
        next_tick_ = out.animated ? std::optional(now_ + tick()) : std::nullopt;
    }

    Model model_;
    TimePoint start_ = TimePoint{} + std::chrono::hours(1);
    TimePoint now_ = start_;
    std::optional<std::pair<bool, std::array<Rgb, 4>>> last_;
    /// Set while the last output was animated: when the effect's next tick is due.
    std::optional<TimePoint> next_tick_;
    /// The next job must write the backlight.
    bool force_backlight_ = false;
    std::vector<Frame> frames_;
    std::vector<Duration> frame_times_;
};

} // namespace omenkbd::test

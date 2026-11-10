#pragma once
// A stand-in for the daemon's frame loop, for the write-count tests (spec section 6). It drives a
// `Model` with a fake clock the way the daemon does: it applies a signal, fact or request and
// composes the frame at once, and between events it wakes at the effect's ticks while the output is
// animated, otherwise at the model's reaction deadline. It counts what the writer would write: one
// write per change of (backlight, hardware zones), because the daemon skips an identical frame.

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

    /// A signal arrives: the daemon applies it and recomputes.
    void signal(const Signal &s) {
        model_.apply_signal(s, now_);
        sample();
    }

    /// A fact changes: the daemon applies it and recomputes.
    void fact(const FactUpdate &f) {
        model_.apply_fact(f);
        sample();
    }

    /// A client request: the daemon recomputes unless nothing changed.
    void request(const Request &r) {
        const auto change = model_.handle(r, now_);
        if (!change) {
            ADD_FAILURE() << "request failed: " << change.error();
            return;
        }
        if (*change != Change::None) {
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

    /// Lets `d` pass, waking where the daemon would: at the effect's ticks while the last output
    /// was animated (they sample the reactions), otherwise at the reaction deadline. The last
    /// sample is at the end of `d`.
    void run_for(Duration d) {
        const TimePoint end = now_ + d;
        int stalls = 0;
        while (now_ < end) {
            TimePoint next = end;
            if (animated_) {
                next = std::min(next, now_ + tick());
            } else if (const std::optional<TimePoint> deadline = model_.reaction_deadline()) {
                // A deadline that is not after now is overdue: the daemon wakes at once.
                next = std::clamp(*deadline, now_, next);
            }
            if (next > now_) {
                stalls = 0;
            } else if (++stalls > 1) {
                ADD_FAILURE() << "the reaction deadline stays at or before now after a sample";
                return;
            }
            now_ = next;
            sample();
        }
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

    /// The daemon's `recompute()`: compose the frame at `now_` and count it when the writer would
    /// write it. While suspended there is no frame and nothing ticks.
    void sample() {
        const std::optional<Inputs> in = model_.inputs(now_);
        if (!in) {
            animated_ = false;
            return;
        }
        const Output out = compose(*in, std::chrono::duration<double>(now_ - start_).count());
        const std::pair key{out.backlight, model_.config().device.zone_map.to_hw(out.zones)};
        if (last_ != key) {
            if (last_) {
                frames_.push_back(out.zones);
                frame_times_.push_back(now_ - start_);
            }
            last_ = key;
        }
        animated_ = out.animated;
    }

    Model model_;
    TimePoint start_ = TimePoint{} + std::chrono::hours(1);
    TimePoint now_ = start_;
    std::optional<std::pair<bool, std::array<Rgb, 4>>> last_;
    bool animated_ = false;
    std::vector<Frame> frames_;
    std::vector<Duration> frame_times_;
};

} // namespace omenkbd::test

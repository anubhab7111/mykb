#pragma once
// Pure daemon state: config, palette, persisted state, system facts, gates, hints and reactions →
// compositor inputs and status. No I/O, no clock of its own: callers pass `now`.

#include "backend/backend.hpp"
#include "config.hpp"
#include "core/compositor.hpp"
#include "core/signals.hpp"
#include "daemon/clock.hpp"
#include "daemon/reactions.hpp"
#include "ipc/protocol.hpp"
#include "state.hpp"

#include <expected>
#include <optional>
#include <string>
#include <tuple>
#include <utility>
#include <variant>
#include <vector>

namespace omenkbd {

/// A change in what the daemon knows about the system.
namespace fact {
struct PowerProfile {
    std::string v;
    friend bool operator==(const PowerProfile &, const PowerProfile &) = default;
};
struct OnBattery {
    bool v;
    friend bool operator==(const OnBattery &, const OnBattery &) = default;
};
struct BatteryPct {
    double v;
    friend bool operator==(const BatteryPct &, const BatteryPct &) = default;
};
struct CpuTemp {
    double v;
    friend bool operator==(const CpuTemp &, const CpuTemp &) = default;
};
struct Locked {
    bool v;
    friend bool operator==(const Locked &, const Locked &) = default;
};
struct LidClosed {
    bool v;
    friend bool operator==(const LidClosed &, const LidClosed &) = default;
};
/// The lowercased class of the focused window while it is in true fullscreen; nullopt otherwise.
struct FullscreenApp {
    std::optional<std::string> v;
    friend bool operator==(const FullscreenApp &, const FullscreenApp &) = default;
};
} // namespace fact

using FactUpdate = std::variant<fact::PowerProfile, fact::OnBattery, fact::BatteryPct,
                                fact::CpuTemp, fact::Locked, fact::LidClosed, fact::FullscreenApp>;

/// What a request changed: nothing, the frame, or the frame and the persisted state.
enum class Change { None, Output, OutputAndPersist };

class Model {
  public:
    Model(Config config, Palette palette, PersistedState persisted, std::string backend);

    /// Applies a request. Status/Subscribe/Reload are handled by the daemon (Change::None).
    std::expected<Change, std::string> handle(const Request &r, TimePoint now);

    /// Also reconfigures the reactions, and refreshes their deadline (a shorter TTL or blink can
    /// bring it forward).
    void replace_config(Config config, Palette palette);
    /// Also restyles the reactions: lamps take the new colors at once.
    void set_palette(Palette palette);
    void apply_fact(const FactUpdate &f);
    /// Entering also drops the playing and queued reaction clips; lamps stay.
    void set_suspended(bool s);
    void set_submap(std::optional<std::string> name);
    /// Hands `s` to the reactions at `now`. A no-op while `[reactions]` is off.
    void apply_signal(const Signal &s, TimePoint now);
    /// When the reaction layers next change on their own, so the daemon wakes to show it; `nullopt`
    /// when nothing will, while suspended, and while `[reactions]` is off. A time that is not after
    /// now means a frame is due.
    ///
    /// `TimePoint{}`, long past, when the visibility changed since the reactions last took it and
    /// no frame followed: the daemon forgets its backlight reading only after composing a forced
    /// write, and `record_write` clears a spent budget without a frame. The next `inputs` takes
    /// the new visibility (a held lamp shows or hides at once), and the deadline moves on.
    [[nodiscard]] std::optional<TimePoint> reaction_deadline() const;
    /// What the firmware reports (e.g. after the Fn backlight key), read at `now`.
    void set_hw_backlight(bool on, TimePoint now) {
        hw_backlight_ = on;
        hw_backlight_at_ = now;
    }
    /// The daemon is writing the backlight itself: what was read before, and when, no longer holds.
    void forget_hw_backlight() {
        hw_backlight_.reset();
        hw_backlight_at_.reset();
    }
    /// True when the backlight was read no more than `max_age` before `now`.
    [[nodiscard]] bool backlight_reading_fresh(TimePoint now, Duration max_age) const;
    void add_warning(std::string w);
    /// Records a writer outcome; `true` = zones were written.
    void record_write(const std::expected<bool, BackendError> &r);

    /// Compositor inputs for `now`, or nullopt while suspended. Drops an expired hint, and moves
    /// the reactions on to `now` for their two layers.
    std::optional<Inputs> inputs(TimePoint now);
    [[nodiscard]] Status status(const Output *out) const;

    [[nodiscard]] const PersistedState &persisted() const { return persisted_; }
    [[nodiscard]] const Config &config() const { return config_; }
    [[nodiscard]] const Palette &palette() const { return palette_; }
    /// When the current modifier hint expires, if one is shown.
    [[nodiscard]] std::optional<TimePoint> hint_deadline() const;
    /// Animation allowed by config and gates (ignoring the Fn-key state) and something animates:
    /// while true the daemon keeps probing the backlight so the Fn key can pause/resume effects.
    [[nodiscard]] bool wants_animation() const;

  private:
    [[nodiscard]] RuleOutcome rules() const;
    [[nodiscard]] std::string shown_profile(const RuleOutcome &rules) const;
    [[nodiscard]] Effect shown_effect(const std::string &profile) const;
    [[nodiscard]] bool animation_allowed() const;
    [[nodiscard]] std::uint8_t effective_brightness(const RuleOutcome &rules) const;
    [[nodiscard]] bool backlight_wanted() const;
    /// The user's on/off: the persisted one, unless the firmware last reported the opposite of
    /// what the daemon wants, which only the Fn key does.
    [[nodiscard]] bool user_on() const;
    /// Who could see a reaction now, with the rules `r` in force.
    [[nodiscard]] Visibility visibility(const RuleOutcome &r) const;
    /// Passes the reactions' config (`reconfigure`) or only the palette (`restyle`) on, with a
    /// resolver over `resolve` that lives for the call. Keeps its warnings while reactions are on.
    void reconfigure_reactions();
    void restyle_reactions();
    std::optional<Rgb> hint_color(std::vector<std::string> &warnings) const;
    Rgb resolve(const ColorRef &c, std::vector<std::string> &warnings) const;
    std::tuple<Frame, Effect, std::uint8_t> profile_look(const std::string &name,
                                                         std::vector<std::string> &warnings) const;
    Frame frame_from(const std::vector<ColorRef> &refs, std::vector<std::string> &warnings) const;
    void fix_active_profile();

    Config config_;
    Palette palette_;
    PersistedState persisted_;
    Facts facts_;
    bool suspended_ = false;
    bool lid_closed_ = false;
    std::optional<bool> hw_backlight_;
    /// When `hw_backlight_` was read.
    std::optional<TimePoint> hw_backlight_at_;
    Reactions reactions_;
    /// The `now` of the reactions' last move (a signal or a frame), so a reload can refresh their
    /// deadline without a clock.
    std::optional<TimePoint> reactions_at_;
    /// What resolving the reactions' colors warned about, while they are on.
    std::vector<std::string> reaction_warnings_;
    /// Modifier hint (normalized mods) and when it was set.
    std::optional<std::pair<std::string, TimePoint>> hint_;
    std::optional<std::string> submap_;
    bool budget_exhausted_ = false;
    std::optional<std::string> last_error_;
    std::vector<std::string> warnings_;
    std::vector<std::string> frame_warnings_;
    std::string backend_;
};

} // namespace omenkbd

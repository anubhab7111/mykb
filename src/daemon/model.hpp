#pragma once
// Pure daemon state: config, palette, persisted state, system facts, gates and hints → compositor
// inputs and status. No I/O, no clock of its own: callers pass `now`.

#include "backend/backend.hpp"
#include "config.hpp"
#include "core/compositor.hpp"
#include "ipc/protocol.hpp"
#include "state.hpp"

#include <chrono>
#include <expected>
#include <optional>
#include <string>
#include <tuple>
#include <utility>
#include <variant>
#include <vector>

namespace omenkbd {

using Clock = std::chrono::steady_clock;
using TimePoint = Clock::time_point;

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
} // namespace fact

using FactUpdate = std::variant<fact::PowerProfile, fact::OnBattery, fact::BatteryPct,
                                fact::CpuTemp, fact::Locked, fact::LidClosed>;

/// What a request changed: nothing, the frame, or the frame and the persisted state.
enum class Change { None, Output, OutputAndPersist };

class Model {
  public:
    Model(Config config, Palette palette, PersistedState persisted, std::string backend);

    /// Applies a request. Status/Subscribe/Reload are handled by the daemon (Change::None).
    std::expected<Change, std::string> handle(const Request &r, TimePoint now);

    void replace_config(Config config, Palette palette);
    void set_palette(Palette palette) { palette_ = std::move(palette); }
    void apply_fact(const FactUpdate &f);
    void set_suspended(bool s);
    void set_submap(std::optional<std::string> name);
    /// What the firmware reports (e.g. after the Fn backlight key).
    void set_hw_backlight(bool on) { hw_backlight_ = on; }
    void add_warning(std::string w);
    /// Records a writer outcome; `true` = zones were written.
    void record_write(const std::expected<bool, BackendError> &r);

    /// Compositor inputs for `now`, or nullopt while suspended. Drops an expired hint.
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

#pragma once
// CLI ↔ daemon protocol: one JSON object per line. Wire format identical to the Rust prototype
// (`"cmd"` tag, kebab-case values, field order).

#include "core/reactions_status.hpp"
#include "core/rules.hpp"
#include "core/signals.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace omenkbd {

/// The longest session id, in bytes, that an `agent` request may carry (the shortest is 1). The
/// daemon keys a lamp on it and keeps the key for an hour, so it is bounded; the Claude hook's own
/// limit of 128 characters fits. The CLI checks the same bound.
inline constexpr std::size_t kMaxSessionBytes = 128;

namespace req {
struct Status {
    friend bool operator==(const Status &, const Status &) = default;
};
struct Set {
    std::vector<std::string> colors;
    friend bool operator==(const Set &, const Set &) = default;
};
struct Profile {
    std::string select;
    friend bool operator==(const Profile &, const Profile &) = default;
};
struct Brightness {
    std::string value;
    friend bool operator==(const Brightness &, const Brightness &) = default;
};
enum class PowerAction { On, Off, Toggle };
struct Power {
    PowerAction action;
    friend bool operator==(const Power &, const Power &) = default;
};
struct Effect {
    std::string name;
    std::optional<std::uint8_t> speed;
    friend bool operator==(const Effect &, const Effect &) = default;
};
struct Hint {
    std::string mods;
    friend bool operator==(const Hint &, const Hint &) = default;
};
struct Idle {
    bool on;
    friend bool operator==(const Idle &, const Idle &) = default;
};
struct Flag {
    std::string name;
    bool on;
    friend bool operator==(const Flag &, const Flag &) = default;
};
struct Reload {
    friend bool operator==(const Reload &, const Reload &) = default;
};
struct Subscribe {
    friend bool operator==(const Subscribe &, const Subscribe &) = default;
};
/// A desktop notification arrived (from the swaync script). `urgency` is as the sender spelled it:
/// `to_signal` accepts any case.
struct Notify {
    std::string app;
    std::string desktop_entry;
    std::string urgency;
    friend bool operator==(const Notify &, const Notify &) = default;
};
/// A Claude Code session changed state (from the hook): `state` is `waiting`, `working`, `done` or
/// `end`. `window` is the Hyprland address of the session's terminal as the sender spelled it
/// (`hyprctl` prints a `0x` prefix); `to_signal` normalizes it. `source` names the sender.
struct Agent {
    std::string state;
    std::string session;
    std::optional<std::string> window;
    std::string source;
    friend bool operator==(const Agent &, const Agent &) = default;
};
} // namespace req

using Request = std::variant<req::Status, req::Set, req::Profile, req::Brightness, req::Power,
                             req::Effect, req::Hint, req::Idle, req::Flag, req::Reload,
                             req::Subscribe, req::Notify, req::Agent>;

/// What `status`, Waybar and `doctor` see. `zones` are logical-order `RRGGBB` after compositing.
struct Status {
    std::string profile;
    std::string effect;
    bool effects_enabled = false;
    bool animated = false;
    std::uint8_t brightness = 0;
    bool on = false;
    bool backlight = false;
    std::array<std::string, 4> zones;
    std::optional<std::string> hint;
    std::vector<std::string> active_rules;
    Facts facts;
    std::string backend;
    std::optional<std::string> last_error;
    bool budget_exhausted = false;
    std::uint64_t lifetime_writes = 0;
    std::vector<std::string> warnings;
    /// The default (off, nothing lit) when a daemon that predates reactions sent the status.
    ReactionsStatus reactions;

    friend bool operator==(const Status &, const Status &) = default;
};

struct Response {
    bool ok = true;
    std::optional<std::string> error;
    std::optional<Status> status;

    static Response success(std::optional<Status> s) { return {true, std::nullopt, std::move(s)}; }
    static Response failure(std::string message) {
        return {false, std::move(message), std::nullopt};
    }
    friend bool operator==(const Response &, const Response &) = default;
};

/// One JSON line, without the trailing newline.
std::string encode(const Request &r);
std::expected<Request, std::string> decode_request(std::string_view line);

/// The signal a `notify` or `agent` request reports, nullopt for every other request, and an error
/// for an unknown urgency or state, for an `agent` session id that is empty or longer than
/// `kMaxSessionBytes`, and for an `agent` window that is not empty and not an address (see
/// `normalize_window_address`; a bare `0x` is not one), whatever the state. The urgency is
/// case-insensitive. The window is normalized (R8: lowercase hex, no `0x`), and an empty one
/// becomes nullopt so that no key is ever linked to an empty address. `working` and `end` carry no
/// window.
std::optional<std::expected<Signal, std::string>> to_signal(const Request &r);

std::string encode(const Response &r);
/// The status object alone (as in a response's `status`), e.g. for `status --json`; `indent` as
/// in nlohmann::json::dump (-1 = one line).
std::string encode_status(const Status &s, int indent = -1);
std::expected<Response, std::string> decode_response(std::string_view line);

} // namespace omenkbd

#pragma once
// CLI ↔ daemon protocol: one JSON object per line. Wire format identical to the Rust prototype
// (`"cmd"` tag, kebab-case values, field order).

#include "core/rules.hpp"

#include <array>
#include <cstdint>
#include <expected>
#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace omenkbd {

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
} // namespace req

using Request =
    std::variant<req::Status, req::Set, req::Profile, req::Brightness, req::Power, req::Effect,
                 req::Hint, req::Idle, req::Flag, req::Reload, req::Subscribe>;

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
std::string encode(const Response &r);
std::expected<Response, std::string> decode_response(std::string_view line);

} // namespace omenkbd

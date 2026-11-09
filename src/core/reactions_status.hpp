#pragma once
// What the reactions are doing right now, for `omen-kbd status`. Plain data: it names the lamp
// kinds and the playing channel, so it carries no clip, color or window.

#include <optional>
#include <string>
#include <vector>

namespace omenkbd {

struct ReactionsStatus {
    /// False while `[reactions]` is off.
    bool enabled = false;
    /// The lamp kinds that hold a key, `agent` first.
    std::vector<std::string> lamps;
    /// The channel of the short reaction in the playing slot (`workspace`, `window`, `notify` or
    /// `agent-done`), if any.
    std::optional<std::string> playing;

    friend bool operator==(const ReactionsStatus &, const ReactionsStatus &) = default;
};

} // namespace omenkbd

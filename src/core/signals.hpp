#pragma once
// What happened: a closed set of signals from Hyprland, notifications and Claude Code sessions. A
// signal carries no colors, window titles or notification text; how it looks on the keyboard is
// decided elsewhere.

#include <optional>
#include <string>
#include <string_view>
#include <variant>

namespace omenkbd {

/// How urgent a notification is. Ordered: Low < Normal < Critical.
enum class Urgency { Low, Normal, Critical };

/// "low", "normal" or "critical".
std::string_view name(Urgency u);

/// Case-insensitive, because swaync sends `Low`, `Normal` and `Critical`.
std::optional<Urgency> urgency_from_name(std::string_view s);

namespace signal {

/// Both workspace ids are > 0 and differ.
struct WorkspaceSwitched {
    int from;
    int to;
    friend bool operator==(const WorkspaceSwitched &, const WorkspaceSwitched &) = default;
};
struct WindowOpened {
    friend bool operator==(const WindowOpened &, const WindowOpened &) = default;
};
struct WindowClosed {
    friend bool operator==(const WindowClosed &, const WindowClosed &) = default;
};
/// `window` is a Hyprland window address: lowercase hex, no `0x`.
struct UrgentRaised {
    std::string window;
    friend bool operator==(const UrgentRaised &, const UrgentRaised &) = default;
};
struct UrgentCleared {
    std::string window;
    friend bool operator==(const UrgentCleared &, const UrgentCleared &) = default;
};
/// Hyprland went away, so no window is urgent any more.
struct UrgentReset {
    friend bool operator==(const UrgentReset &, const UrgentReset &) = default;
};
/// The focused window changed. `window` is its address, `app_class` its class.
struct FocusChanged {
    std::string window;
    std::string app_class;
    friend bool operator==(const FocusChanged &, const FocusChanged &) = default;
};
/// A desktop notification. `desktop_entry` is the app's desktop file id.
struct Notified {
    std::string app;
    std::string desktop_entry;
    Urgency urgency;
    friend bool operator==(const Notified &, const Notified &) = default;
};
/// The Claude Code session `session` needs the user. `window` is the Hyprland address of its
/// terminal, when it has one.
struct AgentWaiting {
    std::string session;
    std::optional<std::string> window;
    friend bool operator==(const AgentWaiting &, const AgentWaiting &) = default;
};
struct AgentWorking {
    std::string session;
    friend bool operator==(const AgentWorking &, const AgentWorking &) = default;
};
/// The session finished a turn. `window` is as for `AgentWaiting`.
struct AgentDone {
    std::string session;
    std::optional<std::string> window;
    friend bool operator==(const AgentDone &, const AgentDone &) = default;
};
struct AgentEnded {
    std::string session;
    friend bool operator==(const AgentEnded &, const AgentEnded &) = default;
};
} // namespace signal

using Signal = std::variant<signal::WorkspaceSwitched, signal::WindowOpened, signal::WindowClosed,
                            signal::UrgentRaised, signal::UrgentCleared, signal::UrgentReset,
                            signal::FocusChanged, signal::Notified, signal::AgentWaiting,
                            signal::AgentWorking, signal::AgentDone, signal::AgentEnded>;

/// True for the signals that could start a short clip or a lamp intro: a workspace switch, a window
/// opening or closing, a window turning urgent, a notification, and a session that is waiting or
/// done. The daemon reads the backlight before delivering these, because the Fn key changes it
/// without any event. The other signals only update or clear what is already showing.
bool may_start_reaction(const Signal &s);

} // namespace omenkbd

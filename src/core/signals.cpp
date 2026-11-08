#include "core/signals.hpp"

#include "util/strings.hpp"

namespace omenkbd {

std::string_view name(Urgency u) {
    switch (u) {
    case Urgency::Low:
        return "low";
    case Urgency::Normal:
        return "normal";
    case Urgency::Critical:
        return "critical";
    }
    return "?";
}

std::optional<Urgency> urgency_from_name(std::string_view s) {
    const std::string lower = to_lower(s);
    for (Urgency u : {Urgency::Low, Urgency::Normal, Urgency::Critical}) {
        if (name(u) == lower) {
            return u;
        }
    }
    return std::nullopt;
}

bool may_start_reaction(const Signal &s) {
    // One overload per alternative and no catch-all, so adding a signal does not compile until it
    // is classified here.
    struct Visitor {
        bool operator()(const signal::WorkspaceSwitched &) const { return true; }
        bool operator()(const signal::WindowOpened &) const { return true; }
        bool operator()(const signal::WindowClosed &) const { return true; }
        bool operator()(const signal::UrgentRaised &) const { return true; }
        bool operator()(const signal::Notified &) const { return true; }
        bool operator()(const signal::AgentWaiting &) const { return true; }
        bool operator()(const signal::AgentDone &) const { return true; }
        bool operator()(const signal::UrgentCleared &) const { return false; }
        bool operator()(const signal::UrgentReset &) const { return false; }
        bool operator()(const signal::FocusChanged &) const { return false; }
        bool operator()(const signal::AgentWorking &) const { return false; }
        bool operator()(const signal::AgentEnded &) const { return false; }
    };
    return std::visit(Visitor{}, s);
}

} // namespace omenkbd

#include "daemon/reactions.hpp"

#include <utility>
#include <variant>

namespace omenkbd {

namespace {

/// A visitor made of lambdas, one per signal type.
template <class... Fs> struct Overloaded : Fs... {
    using Fs::operator()...;
};

/// An empty address is no window: Hyprland reports one when nothing has focus, and no key may be
/// linked to it.
std::optional<std::string> window_or_none(const std::optional<std::string> &window) {
    return window && !window->empty() ? window : std::nullopt;
}

} // namespace

Reactions::Reactions() : look_(cfg_, [](const ColorRef &) { return Rgb{}; }) {}

void Reactions::reconfigure(const ReactionsCfg &cfg, const Resolver &resolve) {
    cfg_ = cfg;
    restyle(resolve);
    if (!cfg_.enabled) {
        lamps_.clear_kind(LampKind::Agent);
        lamps_.clear_kind(LampKind::Urgent);
        player_.clear();
        next_change_.reset();
        // No focus event arrives while disabled, so what was focused then is stale.
        focused_window_.clear();
        focused_class_.clear();
    }
}

void Reactions::restyle(const Resolver &resolve) {
    // Clips already built, keys and intros in flight keep their state: only what is built from here
    // on, and the lamps' paint, take the new look.
    look_ = LookBook(cfg_, resolve);
    player_.set_limits(look_.limits());
    lamps_.set_style(LampKind::Agent, look_.agent_style());
    lamps_.set_style(LampKind::Urgent, look_.urgent_style());
}

void Reactions::on_signal(const Signal &sig, TimePoint now, Visibility visibility) {
    if (!cfg_.enabled) {
        return;
    }
    tick(now, visibility);
    handle(sig, now);
    refresh_next_change(now);
}

void Reactions::update(TimePoint now, Visibility visibility) {
    if (!cfg_.enabled) {
        return;
    }
    tick(now, visibility);
    refresh_next_change(now);
}

void Reactions::suppress() { player_.clear(); }

Layers Reactions::layers(TimePoint now) const {
    Layers out;
    if (visibility_.keyboard_visible) {
        out.lamps = lamps_.layer(now);
    }
    out.transient = player_.layer(now);
    return out;
}

std::optional<TimePoint> Reactions::next_change() const { return next_change_; }

bool Reactions::visibility_differs(Visibility visibility) const {
    if (!visibility_taken_) {
        return visibility.keyboard_visible;
    }
    return visibility.keyboard_visible != visibility_.keyboard_visible ||
           visibility.user_present != visibility_.user_present;
}

ReactionsStatus Reactions::status() const {
    ReactionsStatus out;
    out.enabled = cfg_.enabled;
    for (LampKind kind : lamps_.active()) {
        out.lamps.emplace_back(name(kind));
    }
    if (const std::optional<Channel> channel = player_.playing()) {
        out.playing = std::string(name(*channel));
    }
    return out;
}

void Reactions::tick(TimePoint now, Visibility visibility) {
    visibility_ = visibility;
    visibility_taken_ = true;
    lamps_.expire(now);
    player_.advance(now);
    if (!short_ok()) {
        player_.clear();
    }
}

void Reactions::refresh_next_change(TimePoint now) {
    next_change_ = lamps_.next_change(now);
    if (const std::optional<TimePoint> clips = player_.next_change(now);
        clips && (!next_change_ || *clips < *next_change_)) {
        next_change_ = clips;
    }
}

void Reactions::handle(const Signal &sig, TimePoint now) {
    // An intro is a short reaction too: a lamp raised while nobody would see it lights steadily.
    const bool intro = short_ok();
    std::visit(
        Overloaded{
            [&](const signal::WorkspaceSwitched &s) {
                offer(Channel::Workspace, look_.workspace(s.from, s.to), now);
            },
            [&](const signal::WindowOpened &) {
                offer(Channel::Window, look_.window_opened(), now);
            },
            [&](const signal::WindowClosed &) {
                offer(Channel::Window, look_.window_closed(), now);
            },
            [&](const signal::Notified &n) {
                offer(Channel::Notify, look_.notified(n, focused_class_), now);
            },
            [&](const signal::UrgentRaised &s) {
                // The window with focus is being looked at already. A window without an address
                // could never be focused or closed, and the light has no TTL: it is dropped.
                if (!s.window.empty() && !is_focused(s.window)) {
                    lamps_.raise(LampKind::Urgent, s.window, s.window, now, intro);
                }
            },
            [&](const signal::UrgentCleared &s) { lamps_.clear(LampKind::Urgent, s.window); },
            [&](const signal::UrgentReset &) { lamps_.clear_kind(LampKind::Urgent); },
            [&](const signal::FocusChanged &s) {
                focused_window_ = s.window;
                focused_class_ = s.app_class;
                if (!s.window.empty()) {
                    lamps_.clear_window(s.window);
                }
            },
            [&](const signal::AgentWaiting &s) {
                const std::optional<std::string> window = window_or_none(s.window);
                if (!is_focused(window)) {
                    lamps_.raise(LampKind::Agent, s.session, window, now, intro);
                }
            },
            [&](const signal::AgentWorking &s) { lamps_.clear(LampKind::Agent, s.session); },
            [&](const signal::AgentDone &s) {
                lamps_.clear(LampKind::Agent, s.session);
                if (!is_focused(window_or_none(s.window))) {
                    offer(Channel::AgentDone, look_.agent_done(), now);
                }
            },
            [&](const signal::AgentEnded &s) { lamps_.clear(LampKind::Agent, s.session); },
        },
        sig);
}

void Reactions::offer(Channel channel, std::optional<Clip> clip, TimePoint now) {
    if (clip && short_ok()) {
        player_.offer(channel, std::move(*clip), now);
    }
}

bool Reactions::short_ok() const {
    return visibility_.keyboard_visible && visibility_.user_present;
}

bool Reactions::is_focused(const std::optional<std::string> &window) const {
    return window && !focused_window_.empty() && *window == focused_window_;
}

} // namespace omenkbd

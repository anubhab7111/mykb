#include "core/lamps.hpp"

#include <algorithm>
#include <cstddef>
#include <utility>

namespace omenkbd {

namespace {

/// From the lowest layer to the highest: `Agent` is painted over `Urgent`.
constexpr std::array<LampKind, 2> kPaintOrder{LampKind::Urgent, LampKind::Agent};

/// What the style paints once its intro is over: the color on every zone.
LayerOps steady(const LampStyle &style) {
    LayerOps ops;
    for (Zone z : style.zones) {
        ops[index(z)] = Paint{style.color};
    }
    return ops;
}

/// True when the key of a `keys` map entry is linked to `window`.
bool linked_to(const auto &entry, std::string_view window) {
    return entry.second.window && *entry.second.window == window;
}

/// An intro that is still playing: its clip, when it began and how far in it is.
struct Intro {
    Clip clip;
    TimePoint start;
    Duration elapsed;
};

/// The intro showing at `now`. None when the kind lit without one and once the intro is over,
/// however late `now` is: the state is read from the times, so no sample replays a blink. A style
/// with 0 or 1 blinks has a single lit phase, which looks like the steady paint after it.
std::optional<Intro> intro_at(const LampStyle &style, std::optional<TimePoint> start,
                              TimePoint now) {
    if (!start) {
        return std::nullopt;
    }
    Clip clip =
        flash(style.color, style.zones, std::max(1, style.blinks), style.blink, style.blink);
    // A `now` before the start reads the first step: the clip never sees a negative time.
    const Duration elapsed = std::max(now - *start, Duration::zero());
    if (elapsed >= clip.total()) {
        return std::nullopt;
    }
    return Intro{std::move(clip), *start, elapsed};
}

/// `below` with every zone `above` has an op for replaced by that op.
void paint_over(LayerOps &below, const LayerOps &above) {
    for (std::size_t i = 0; i < below.size(); ++i) {
        if (above[i]) {
            below[i] = above[i];
        }
    }
}

} // namespace

std::string_view name(LampKind kind) {
    switch (kind) {
    case LampKind::Agent:
        return "agent";
    case LampKind::Urgent:
        return "urgent";
    }
    return "?";
}

void LampSet::set_style(LampKind kind, LampStyle style) { slot(kind).style = std::move(style); }

void LampSet::raise(LampKind kind, std::string key, std::optional<std::string> window,
                    TimePoint now, bool play_intro) {
    Slot &s = slot(kind);
    if (s.style.zones.empty()) {
        return;
    }
    if (window) {
        if (kind == LampKind::Urgent) {
            const auto &agent_keys = slot(LampKind::Agent).keys;
            const bool absorbed = std::ranges::any_of(
                agent_keys, [&window](const auto &entry) { return linked_to(entry, *window); });
            if (absorbed) {
                return;
            }
        } else {
            // The agent light replaces the urgent one of its window, so the window shows one light.
            std::erase_if(slot(LampKind::Urgent).keys,
                          [&window](const auto &entry) { return linked_to(entry, *window); });
        }
    }
    if (const auto it = s.keys.find(key); it != s.keys.end()) {
        it->second.raised_at = now;
        return;
    }
    if (s.keys.empty()) {
        s.intro_start = play_intro ? std::optional<TimePoint>{now} : std::nullopt;
    } else if (s.keys.size() >= kMaxKeysPerKind) {
        // The oldest raise goes. `keys` is ordered by key and `min_element` keeps the first of
        // equals, so on a tie the smallest key goes. The kind stays lit, so its intro is untouched.
        s.keys.erase(std::ranges::min_element(
            s.keys, {}, [](const auto &entry) { return entry.second.raised_at; }));
    }
    s.keys.emplace(std::move(key), Key{std::move(window), now});
}

void LampSet::clear(LampKind kind, std::string_view key) {
    auto &keys = slot(kind).keys;
    if (const auto it = keys.find(key); it != keys.end()) {
        keys.erase(it);
    }
}

void LampSet::clear_window(std::string_view window) {
    for (Slot &s : slots_) {
        std::erase_if(s.keys, [window](const auto &entry) { return linked_to(entry, window); });
    }
}

void LampSet::clear_kind(LampKind kind) { slot(kind).keys.clear(); }

void LampSet::expire(TimePoint now) {
    for (Slot &s : slots_) {
        if (!s.style.ttl) {
            continue;
        }
        const Duration ttl = *s.style.ttl;
        std::erase_if(
            s.keys, [now, ttl](const auto &entry) { return now - entry.second.raised_at >= ttl; });
    }
}

LayerOps LampSet::layer(TimePoint now) const {
    LayerOps out;
    for (LampKind kind : kPaintOrder) {
        const Slot &s = slot(kind);
        if (s.keys.empty()) {
            continue;
        }
        const std::optional<Intro> intro = intro_at(s.style, s.intro_start, now);
        paint_over(out, intro ? intro->clip.ops_at(intro->elapsed) : steady(s.style));
    }
    return out;
}

std::optional<TimePoint> LampSet::next_change(TimePoint now) const {
    std::optional<TimePoint> soonest;
    const auto consider = [&soonest](TimePoint t) {
        if (!soonest || t < *soonest) {
            soonest = t;
        }
    };
    for (const Slot &s : slots_) {
        if (s.keys.empty()) {
            continue;
        }
        if (const std::optional<Intro> intro = intro_at(s.style, s.intro_start, now)) {
            // The end of the last lit phase is no change: it is lit before and after.
            const std::optional<Duration> boundary = intro->clip.next_boundary(intro->elapsed);
            if (boundary && *boundary < intro->clip.total()) {
                consider(intro->start + *boundary);
            }
        }
        if (s.style.ttl) {
            const Duration ttl = *s.style.ttl;
            for (const auto &entry : s.keys) {
                consider(entry.second.raised_at + ttl);
            }
        }
    }
    return soonest;
}

std::vector<LampKind> LampSet::active() const {
    std::vector<LampKind> kinds;
    for (LampKind kind : {LampKind::Agent, LampKind::Urgent}) {
        if (!slot(kind).keys.empty()) {
            kinds.push_back(kind);
        }
    }
    return kinds;
}

bool LampSet::empty() const {
    return std::ranges::all_of(slots_, [](const Slot &s) { return s.keys.empty(); });
}

std::size_t LampSet::key_count(LampKind kind) const { return slot(kind).keys.size(); }

LampSet::Slot &LampSet::slot(LampKind kind) { return slots_[static_cast<std::size_t>(kind)]; }

const LampSet::Slot &LampSet::slot(LampKind kind) const {
    return slots_[static_cast<std::size_t>(kind)];
}

} // namespace omenkbd

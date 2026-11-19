#include "daemon/model.hpp"

#include "util/strings.hpp"

#include <algorithm>
#include <charconv>
#include <format>

namespace omenkbd {

namespace {

constexpr std::string_view kEffectsOff =
    "animations are off: set [effects] enabled = true in ~/.config/omen-kbd/config.toml, then "
    "omen-kbd reload";

void push_unique(std::vector<std::string> &v, std::string s) {
    if (std::ranges::find(v, s) == v.end()) {
        v.push_back(std::move(s));
    }
}

std::optional<int> parse_int(std::string_view s) {
    if (s.starts_with('+')) {
        s.remove_prefix(1);
    }
    int v = 0;
    const auto [ptr, ec] = std::from_chars(s.data(), s.data() + s.size(), v);
    if (ec != std::errc{} || ptr != s.data() + s.size() || s.empty()) {
        return std::nullopt;
    }
    return v;
}

} // namespace

Model::Model(Config config, PersistedState persisted, std::string backend)
    : config_(std::move(config)), persisted_(std::move(persisted)), backend_(std::move(backend)) {
    fix_active_profile();
    reconfigure_reactions();
}

std::expected<Change, std::string> Model::handle(const Request &r) {
    using R = std::expected<Change, std::string>;
    if (std::holds_alternative<req::Status>(r) || std::holds_alternative<req::Reload>(r)) {
        return Change::None;
    }
    // The daemon turns these into signals (`to_signal`) and delivers them itself, reading the
    // backlight first when a reaction could start (`Daemon::deliver_signal`).
    if (std::holds_alternative<req::Notify>(r) || std::holds_alternative<req::Agent>(r)) {
        return Change::None;
    }
    if (const auto *s = std::get_if<req::Set>(&r)) {
        return set_look(*s);
    }
    if (const auto *b = std::get_if<req::Brightness>(&r)) {
        const std::string_view v = trim(b->value);
        const auto bad = [&] {
            return R(std::unexpect,
                     std::format("brightness must be 0-100, +N or -N, got \"{}\"", v));
        };
        const auto n = parse_int(v);
        if (!n) {
            return bad();
        }
        int next = 0;
        if (v.starts_with('+') || v.starts_with('-')) {
            next = std::clamp(int{persisted_.brightness} + *n, 0, 100);
        } else {
            if (*n < 0 || *n > 100) {
                return bad();
            }
            next = *n;
        }
        persisted_.brightness = static_cast<std::uint8_t>(next);
        return Change::OutputAndPersist;
    }
    if (const auto *pw = std::get_if<req::Power>(&r)) {
        switch (pw->action) {
        case req::PowerAction::On:
            persisted_.on = true;
            break;
        case req::PowerAction::Off:
            persisted_.on = false;
            break;
        case req::PowerAction::Toggle:
            persisted_.on = !user_on();
            break;
        }
        return Change::OutputAndPersist;
    }
    if (const auto *i = std::get_if<req::Idle>(&r)) {
        facts_.idle = i->on;
        return Change::Output;
    }
    if (const auto *f = std::get_if<req::Flag>(&r)) {
        if (f->name == "gamemode") {
            facts_.gamemode = f->on;
            return Change::Output;
        }
        if (f->name == "locked") { // from hypridle's on_lock_cmd / on_unlock_cmd
            apply_fact(fact::Locked{f->on});
            return Change::Output;
        }
        return R(std::unexpect, std::format("unknown flag: {}", f->name));
    }
    return Change::None;
}

std::expected<Change, std::string> Model::set_look(const req::Set &s) {
    using R = std::expected<Change, std::string>;
    if (s.look.empty() && !s.effect && !s.speed) {
        return R(std::unexpect,
                 std::string("set needs a profile, 1 or 4 colors, --effect or --speed"));
    }
    std::optional<Effect> effect;
    if (s.effect) {
        effect = effect_from_name(*s.effect);
        if (!effect) {
            return R(std::unexpect, std::format("unknown effect \"{}\" (expected static, drift, "
                                                "glow, tide or dusk)",
                                                *s.effect));
        }
    }
    if (s.speed && (*s.speed < 1 || *s.speed > 100)) {
        return R(std::unexpect, std::format("speed must be 1-100, got {}", *s.speed));
    }
    // Work on a copy: a bad request changes nothing.
    PersistedState next = persisted_;
    if (!s.look.empty()) {
        const bool profile_name =
            s.look.size() == 1 && !Rgb::parse(s.look[0]) &&
            (config_.profile(s.look[0]) != nullptr || (s.look[0] == kCustomProfile && next.custom));
        if (profile_name) {
            next.active_profile = s.look[0];
        } else {
            if (s.look.size() != 1 && s.look.size() != 4) {
                return R(std::unexpect,
                         std::format("set takes a profile name, or 1 or 4 colors (left middle "
                                     "right wasd), got {}",
                                     s.look.size()));
            }
            for (const auto &c : s.look) {
                if (auto parsed = Rgb::parse(c); !parsed) {
                    return R(std::unexpect,
                             s.look.size() == 1
                                 ? std::format("unknown profile or color: {}", parsed.error())
                                 : parsed.error());
                }
            }
            next.custom = s.look;
            next.active_profile = std::string(kCustomProfile);
            next.on = true;
        }
        next.effect_override.reset();
    }
    if (effect || s.speed) {
        std::swap(persisted_, next); // look at the look as it will be, then swap back
        std::vector<std::string> ignored;
        const auto [colors, look_effect, look_speed] =
            profile_look(shown_profile(rules()), ignored);
        const Effect shown =
            persisted_.effect_override ? persisted_.effect_override->effect : look_effect;
        std::swap(persisted_, next);
        const Effect target = effect.value_or(shown);
        if (target != Effect::Static && !config_.effects.enabled) {
            return R(std::unexpect, std::string(kEffectsOff));
        }
        const std::uint8_t speed = s.speed.value_or(look_speed);
        if (target == Effect::Static && look_effect == Effect::Static) {
            next.effect_override.reset();
        } else {
            next.effect_override = EffectOverride{target, speed};
        }
    }
    persisted_ = std::move(next);
    return Change::OutputAndPersist;
}

void Model::replace_config(Config config) {
    config_ = std::move(config);
    fix_active_profile();
    reconfigure_reactions();
    // The cached deadline was worked out under the old timings: a shorter TTL or blink would leave
    // it too late, and the daemon would sleep through the change.
    if (reactions_at_) {
        reactions_.update(*reactions_at_, visibility(rules()));
    }
}

void Model::apply_fact(const FactUpdate &f) {
    std::visit(
        [this](const auto &v) {
            using T = std::decay_t<decltype(v)>;
            if constexpr (std::is_same_v<T, fact::PowerProfile>) {
                facts_.power_profile = v.v;
            } else if constexpr (std::is_same_v<T, fact::OnBattery>) {
                facts_.on_battery = v.v;
            } else if constexpr (std::is_same_v<T, fact::BatteryPct>) {
                facts_.battery_pct = v.v;
            } else if constexpr (std::is_same_v<T, fact::CpuTemp>) {
                facts_.cpu_temp_c = v.v;
            } else if constexpr (std::is_same_v<T, fact::Locked>) {
                facts_.locked = v.v;
            } else if constexpr (std::is_same_v<T, fact::LidClosed>) {
                lid_closed_ = v.v;
            } else if constexpr (std::is_same_v<T, fact::FullscreenApp>) {
                // Lowercased here, where the fact enters: `Facts::fullscreen_app` is documented
                // lowercase and the rules compare it with their own lowercased classes.
                facts_.fullscreen_app =
                    v.v.transform([](const std::string &app) { return to_lower(app); });
            }
        },
        f);
}

void Model::set_suspended(bool s) {
    suspended_ = s;
    if (s) {
        reactions_.suppress();
    }
}

void Model::apply_signal(const Signal &s, TimePoint now) {
    reactions_.on_signal(s, now, visibility(rules()));
    reactions_at_ = now;
}

std::optional<TimePoint> Model::reaction_deadline() const {
    if (suspended_ || !config_.reactions.enabled) {
        return std::nullopt;
    }
    if (reactions_.visibility_differs(visibility(rules()))) {
        return TimePoint{}; // a frame is due now: see the header
    }
    return reactions_.next_change();
}

bool Model::backlight_reading_fresh(TimePoint now, Duration max_age) const {
    return hw_backlight_at_ && now - *hw_backlight_at_ <= max_age;
}

void Model::add_warning(std::string w) { push_unique(warnings_, std::move(w)); }

void Model::record_write(const std::expected<bool, BackendError> &r) {
    if (r) {
        if (*r) {
            ++persisted_.lifetime_writes;
            budget_exhausted_ = false;
        }
        last_error_.reset();
        return;
    }
    if (r.error().kind == BackendError::Kind::Budget) {
        budget_exhausted_ = true;
    }
    last_error_ = r.error().message();
}

bool Model::wants_animation() const {
    if (!animation_allowed()) {
        return false;
    }
    const RuleOutcome r = rules();
    const bool pulsing = r.overlay && std::holds_alternative<ZonesOverlay>(*r.overlay) &&
                         std::get<ZonesOverlay>(*r.overlay).pulse;
    return is_animated(shown_effect(shown_profile(r))) || pulsing;
}

// GCC 16 at -O3 reports the padding of the overlay's ZonesOverlay as uninitialized when `Inputs` is
// moved into the returned optional. Every member is initialized above; this is a false positive.
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmaybe-uninitialized"
std::optional<Inputs> Model::inputs(TimePoint now) {
    if (suspended_) {
        return std::nullopt;
    }
    const RuleOutcome r = rules();
    reactions_.update(now, visibility(r));
    reactions_at_ = now;
    const Layers layers = reactions_.layers(now);
    std::vector<std::string> warnings;
    auto [base, effect, speed] = profile_look(shown_profile(r), warnings);
    if (persisted_.effect_override) {
        effect = persisted_.effect_override->effect;
        speed = persisted_.effect_override->speed;
    }
    std::optional<Overlay> overlay;
    if (r.overlay) {
        if (const auto *z = std::get_if<ZonesOverlay>(&*r.overlay)) {
            overlay = ZonesOverlay{z->zones, z->color, z->pulse};
        } else {
            const auto &t = std::get<TintOverlay>(*r.overlay);
            overlay = TintOverlay{t.color, t.strength};
        }
    }
    Inputs in{.base = base,
              .effect = effect,
              .speed = speed,
              .animate = animation_allowed() && hw_backlight_ != false,
              .overlay = std::move(overlay),
              .brightness = effective_brightness(r),
              .backlight = backlight_wanted(),
              .lamps = layers.lamps,
              .transient = layers.transient};
    frame_warnings_ = std::move(warnings);
    return in;
}
#pragma GCC diagnostic pop

Status Model::status(const Output *out) const {
    const RuleOutcome r = rules();
    Status s;
    s.profile = shown_profile(r);
    s.effect = std::string(name(shown_effect(s.profile)));
    s.effects_enabled = config_.effects.enabled;
    s.animated = out != nullptr && out->animated;
    s.brightness = persisted_.brightness;
    s.on = user_on();
    s.backlight = out != nullptr ? out->backlight : backlight_wanted();
    if (out != nullptr) {
        for (std::size_t i = 0; i < 4; ++i) {
            s.zones[i] = out->zones[i].to_hex();
        }
    }
    s.active_rules = r.active;
    s.facts = facts_;
    s.backend = backend_;
    s.last_error = last_error_;
    s.budget_exhausted = budget_exhausted_;
    s.lifetime_writes = persisted_.lifetime_writes;
    s.warnings = warnings_;
    for (const auto &w : frame_warnings_) {
        push_unique(s.warnings, w);
    }
    s.reactions = reactions_.status();
    return s;
}

// ── internals ───────────────────────────────────────────────────────────────

RuleOutcome Model::rules() const { return evaluate(config_.rules, facts_); }

std::string Model::shown_profile(const RuleOutcome &r) const {
    return r.profile.value_or(persisted_.active_profile);
}

Effect Model::shown_effect(const std::string &profile) const {
    if (persisted_.effect_override) {
        return persisted_.effect_override->effect;
    }
    std::vector<std::string> ignored;
    return std::get<1>(profile_look(profile, ignored));
}

bool Model::animation_allowed() const {
    // Idle and lock stop the ticking whatever their gate does to the backlight (dim, keep).
    return config_.effects.enabled && !budget_exhausted_ && !facts_.idle && !facts_.locked &&
           !(facts_.on_battery == true && config_.gates.pause_effects_on_battery);
}

std::uint8_t Model::effective_brightness(const RuleOutcome &r) const {
    std::uint8_t b = persisted_.brightness;
    if (r.brightness_cap) {
        b = std::min(b, *r.brightness_cap);
    }
    const auto &g = config_.gates;
    const bool dim = (facts_.locked && g.on_lock == GateAction::Dim) ||
                     (facts_.idle && g.on_idle == GateAction::Dim);
    if (dim) {
        b = std::min(b, g.dim_brightness);
    }
    return b;
}

bool Model::backlight_wanted() const {
    const auto &g = config_.gates;
    return persisted_.on && !lid_closed_ && !(facts_.locked && g.on_lock == GateAction::Off) &&
           !(facts_.idle && g.on_idle == GateAction::Off);
}

Visibility Model::visibility(const RuleOutcome &r) const {
    // The firmware's off (the Fn key) and a spent budget keep the zones from showing although the
    // composed frame says "on".
    const bool keyboard_visible = !suspended_ && backlight_wanted() &&
                                  effective_brightness(r) > 0 && hw_backlight_ != false &&
                                  !budget_exhausted_;
    return Visibility{keyboard_visible, !facts_.locked && !facts_.idle};
}

void Model::reconfigure_reactions() { reactions_.reconfigure(config_.reactions); }

bool Model::user_on() const {
    if (hw_backlight_ && *hw_backlight_ != backlight_wanted()) {
        return *hw_backlight_;
    }
    return persisted_.on;
}

std::tuple<Frame, Effect, std::uint8_t>
Model::profile_look(const std::string &profile_name, std::vector<std::string> &warnings) const {
    if (profile_name == kCustomProfile) {
        std::vector<Rgb> colors;
        if (persisted_.custom) {
            for (const auto &s : *persisted_.custom) {
                if (auto c = Rgb::parse(s)) {
                    colors.push_back(*c);
                }
            }
        }
        return {frame_from(colors, warnings), Effect::Static, std::uint8_t{50}};
    }
    if (const ProfileCfg *p = config_.profile(profile_name)) {
        return {frame_from(p->colors, warnings), p->effect, p->speed};
    }
    push_unique(warnings, std::format("unknown profile \"{}\"", profile_name));
    return {Frame{kWhite, kWhite, kWhite, kWhite}, Effect::Static, std::uint8_t{50}};
}

Frame Model::frame_from(const std::vector<Rgb> &colors, std::vector<std::string> &warnings) {
    if (colors.size() == 1) {
        return {colors[0], colors[0], colors[0], colors[0]};
    }
    if (colors.size() == 4) {
        return {colors[0], colors[1], colors[2], colors[3]};
    }
    push_unique(warnings, "stored custom colors are invalid; showing white");
    return {kWhite, kWhite, kWhite, kWhite};
}

void Model::fix_active_profile() {
    const auto &p = persisted_.active_profile;
    const bool valid =
        (p == kCustomProfile && persisted_.custom.has_value()) || config_.profile(p) != nullptr;
    if (!valid) {
        const std::string first = config_.profiles.front().name;
        add_warning(std::format("profile \"{}\" is not in the config; using \"{}\"", p, first));
        persisted_.active_profile = first;
    }
}

} // namespace omenkbd

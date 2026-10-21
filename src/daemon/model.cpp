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

Model::Model(Config config, Palette palette, PersistedState persisted, std::string backend)
    : config_(std::move(config)), palette_(std::move(palette)), persisted_(std::move(persisted)),
      backend_(std::move(backend)) {
    fix_active_profile();
}

std::expected<Change, std::string> Model::handle(const Request &r, TimePoint now) {
    using R = std::expected<Change, std::string>;
    if (std::holds_alternative<req::Status>(r) || std::holds_alternative<req::Subscribe>(r) ||
        std::holds_alternative<req::Reload>(r)) {
        return Change::None;
    }
    if (const auto *s = std::get_if<req::Set>(&r)) {
        if (s->colors.size() != 1 && s->colors.size() != 4) {
            return R(std::unexpect,
                     std::format("set takes 1 or 4 colors (left middle right wasd), got {}",
                                 s->colors.size()));
        }
        for (const auto &c : s->colors) {
            if (auto parsed = ColorRef::parse(c); !parsed) {
                return R(std::unexpect, parsed.error());
            }
        }
        persisted_.custom = s->colors;
        persisted_.active_profile = std::string(kCustomProfile);
        persisted_.on = true;
        persisted_.effect_override.reset();
        return Change::OutputAndPersist;
    }
    if (const auto *p = std::get_if<req::Profile>(&r)) {
        const auto &profiles = config_.profiles;
        const auto cur = std::ranges::find(profiles, persisted_.active_profile, &ProfileCfg::name);
        const auto n = profiles.size();
        const auto idx = static_cast<std::size_t>(cur - profiles.begin()); // == n when not found
        std::string target;
        if (p->select == "next") {
            target = profiles[cur == profiles.end() ? 0 : (idx + 1) % n].name;
        } else if (p->select == "prev") {
            target = profiles[cur == profiles.end() ? n - 1 : (idx + n - 1) % n].name;
        } else if (p->select == kCustomProfile && persisted_.custom) {
            target = std::string(kCustomProfile);
        } else if (config_.profile(p->select) != nullptr) {
            target = p->select;
        } else {
            return R(std::unexpect, std::format("unknown profile: {}", p->select));
        }
        persisted_.active_profile = target;
        persisted_.effect_override.reset();
        return Change::OutputAndPersist;
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
            persisted_.on = !persisted_.on;
            break;
        }
        return Change::OutputAndPersist;
    }
    if (const auto *e = std::get_if<req::Effect>(&r)) {
        if (!config_.effects.enabled) {
            return R(std::unexpect, std::string(kEffectsOff));
        }
        const auto effect = effect_from_name(e->name);
        if (!effect) {
            return R(std::unexpect,
                     std::format("unknown effect \"{}\" (expected static, breathe, wave, cycle or "
                                 "gradient)",
                                 e->name));
        }
        if (*effect == Effect::Static) {
            persisted_.effect_override.reset();
        } else {
            std::vector<std::string> ignored;
            const std::uint8_t speed =
                e->speed.value_or(std::get<2>(profile_look(shown_profile(rules()), ignored)));
            if (speed < 1 || speed > 100) {
                return R(std::unexpect, std::format("speed must be 1-100, got {}", speed));
            }
            persisted_.effect_override = EffectOverride{*effect, speed};
        }
        return Change::OutputAndPersist;
    }
    if (const auto *h = std::get_if<req::Hint>(&r)) {
        if (to_lower(trim(h->mods)) == "clear") {
            hint_.reset();
            return Change::Output;
        }
        auto key = normalize_mods(h->mods);
        if (!key) {
            return R(std::unexpect, key.error());
        }
        if (!config_.hints.modifiers.contains(*key)) {
            return R(std::unexpect, std::format("no color configured for {}", *key));
        }
        hint_ = std::pair{*key, now};
        return Change::Output;
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
        return R(std::unexpect, std::format("unknown flag: {}", f->name));
    }
    return Change::None;
}

void Model::replace_config(Config config, Palette palette) {
    config_ = std::move(config);
    palette_ = std::move(palette);
    fix_active_profile();
    if (hint_ && !config_.hints.modifiers.contains(hint_->first)) {
        hint_.reset();
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
                if (v.v) {
                    hint_.reset();
                }
            } else if constexpr (std::is_same_v<T, fact::LidClosed>) {
                lid_closed_ = v.v;
            }
        },
        f);
}

void Model::set_suspended(bool s) {
    suspended_ = s;
    if (s) {
        hint_.reset();
    }
}

void Model::set_submap(std::optional<std::string> name) {
    if (name && name->empty()) {
        name.reset();
    }
    submap_ = std::move(name);
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

std::optional<TimePoint> Model::hint_deadline() const {
    if (!hint_) {
        return std::nullopt;
    }
    return hint_->second + std::chrono::seconds(config_.hints.timeout_s);
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

std::optional<Inputs> Model::inputs(TimePoint now) {
    if (suspended_) {
        return std::nullopt;
    }
    if (const auto d = hint_deadline(); d && now >= *d) {
        hint_.reset();
    }
    const RuleOutcome r = rules();
    std::vector<std::string> warnings;
    auto [base, effect, speed] = profile_look(shown_profile(r), warnings);
    if (persisted_.effect_override) {
        effect = persisted_.effect_override->effect;
        speed = persisted_.effect_override->speed;
    }
    std::optional<ResolvedOverlay> overlay;
    if (r.overlay) {
        if (const auto *z = std::get_if<ZonesOverlay>(&*r.overlay)) {
            overlay = ResolvedZones{z->zones, resolve(z->color, warnings), z->pulse};
        } else {
            const auto &t = std::get<TintOverlay>(*r.overlay);
            overlay = ResolvedTint{resolve(t.color, warnings), t.strength};
        }
    }
    Inputs in{.base = base,
              .effect = effect,
              .speed = speed,
              .animate = animation_allowed() && hw_backlight_ != false,
              .overlay = std::move(overlay),
              .hint = hint_color(warnings),
              .brightness = effective_brightness(r),
              .backlight = backlight_wanted()};
    frame_warnings_ = std::move(warnings);
    return in;
}

Status Model::status(const Output *out) const {
    const RuleOutcome r = rules();
    Status s;
    s.profile = shown_profile(r);
    s.effect = std::string(name(shown_effect(s.profile)));
    s.effects_enabled = config_.effects.enabled;
    s.animated = out != nullptr && out->animated;
    s.brightness = persisted_.brightness;
    s.on = persisted_.on;
    s.backlight = out != nullptr ? out->backlight : backlight_wanted();
    if (out != nullptr) {
        for (std::size_t i = 0; i < 4; ++i) {
            s.zones[i] = out->zones[i].to_hex();
        }
    }
    if (hint_) {
        s.hint = hint_->first;
    } else if (submap_ && config_.hints.submaps.contains(*submap_)) {
        s.hint = "submap:" + *submap_;
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
    return config_.effects.enabled && !budget_exhausted_ &&
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

std::optional<Rgb> Model::hint_color(std::vector<std::string> &warnings) const {
    if (hint_) {
        if (const auto it = config_.hints.modifiers.find(hint_->first);
            it != config_.hints.modifiers.end()) {
            return resolve(it->second, warnings);
        }
    }
    if (submap_) {
        if (const auto it = config_.hints.submaps.find(*submap_);
            it != config_.hints.submaps.end()) {
            return resolve(it->second, warnings);
        }
    }
    return std::nullopt;
}

Rgb Model::resolve(const ColorRef &c, std::vector<std::string> &warnings) const {
    auto rgb = c.resolve(palette_, config_.theme.colors);
    if (!rgb) {
        push_unique(warnings, std::format("unknown theme color: {}", rgb.error()));
        return kWhite;
    }
    return *rgb;
}

std::tuple<Frame, Effect, std::uint8_t>
Model::profile_look(const std::string &profile_name, std::vector<std::string> &warnings) const {
    if (profile_name == kCustomProfile) {
        std::vector<ColorRef> refs;
        if (persisted_.custom) {
            for (const auto &s : *persisted_.custom) {
                if (auto c = ColorRef::parse(s)) {
                    refs.push_back(*c);
                }
            }
        }
        return {frame_from(refs, warnings), Effect::Static, std::uint8_t{50}};
    }
    if (const ProfileCfg *p = config_.profile(profile_name)) {
        return {frame_from(p->colors, warnings), p->effect, p->speed};
    }
    push_unique(warnings, std::format("unknown profile \"{}\"", profile_name));
    return {Frame{kWhite, kWhite, kWhite, kWhite}, Effect::Static, std::uint8_t{50}};
}

Frame Model::frame_from(const std::vector<ColorRef> &refs,
                        std::vector<std::string> &warnings) const {
    if (refs.size() == 1) {
        const Rgb c = resolve(refs[0], warnings);
        return {c, c, c, c};
    }
    if (refs.size() == 4) {
        return {resolve(refs[0], warnings), resolve(refs[1], warnings), resolve(refs[2], warnings),
                resolve(refs[3], warnings)};
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

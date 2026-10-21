#include "core/compositor.hpp"

namespace omenkbd {

Output compose(const Inputs &i, double t) {
    Frame zones =
        (i.animate && is_animated(i.effect)) ? render(i.effect, i.base, t, i.speed) : i.base;

    bool pulsing = false;
    if (i.overlay) {
        if (const auto *tint = std::get_if<ResolvedTint>(&*i.overlay)) {
            for (Rgb &z : zones) {
                z = Rgb::lerp(z, tint->color, tint->strength);
            }
        } else {
            const auto &ov = std::get<ResolvedZones>(*i.overlay);
            Rgb c = ov.color;
            if (ov.pulse && i.animate) {
                pulsing = true;
                c = multiply(c, pulse_factor(t));
            }
            for (Zone z : ov.zones) {
                zones[index(z)] = c;
            }
        }
    }

    if (i.hint) {
        zones.fill(*i.hint);
    }
    for (Rgb &z : zones) {
        z = z.scale(i.brightness);
    }
    const bool backlight = i.backlight && i.brightness > 0;
    const bool animated = backlight && !i.hint && i.animate && (is_animated(i.effect) || pulsing);
    return Output{backlight, zones, animated};
}

} // namespace omenkbd

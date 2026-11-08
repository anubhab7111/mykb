#pragma once
// A short keyboard reaction as data. A clip is a list of steps; each step holds for a while and
// says, per zone, what to do to the frame below it. Pure: a clip never reads a clock, callers pass
// the time since it started.

#include "core/clock.hpp"
#include "core/color.hpp"
#include "core/zones.hpp"

#include <array>
#include <optional>
#include <variant>
#include <vector>

namespace omenkbd {

/// Replaces the zone with `color`.
struct Paint {
    Rgb color;
    friend bool operator==(const Paint &, const Paint &) = default;
};

/// Blends the zone below toward `toward`: `Rgb::lerp(below, toward, amount)`.
struct Mix {
    Rgb toward;
    double amount;
    friend bool operator==(const Mix &, const Mix &) = default;
};

/// Multiplies the zone below by `factor`: `multiply(below, factor)`.
struct Scale {
    double factor;
    friend bool operator==(const Scale &, const Scale &) = default;
};

using ZoneOp = std::variant<Paint, Mix, Scale>;

/// What one layer does to each zone, in logical zone order. `nullopt` shows what is below.
using LayerOps = std::array<std::optional<ZoneOp>, 4>;

/// One step of a clip: the ops it applies and how long it holds.
struct ClipStep {
    LayerOps ops;
    Duration hold;
};

/// A short reaction: its steps run one after another, each for its own `hold`.
struct Clip {
    std::vector<ClipStep> steps;

    /// The sum of the holds.
    [[nodiscard]] Duration total() const;

    /// The ops of the step showing `elapsed` after the clip started; empty at and after `total()`.
    /// `elapsed` is a time since the start, so it is never negative.
    [[nodiscard]] LayerOps ops_at(Duration elapsed) const;

    /// When the ops next change: the end of the step showing at `elapsed`, which is strictly after
    /// it. `nullopt` at and after `total()`.
    [[nodiscard]] std::optional<Duration> next_boundary(Duration elapsed) const;
};

/// True when no zone has an op, so the layer shows exactly what is below.
bool empty(const LayerOps &ops);

/// `below` with each zone's op applied; a zone without an op keeps its color.
///
/// Always call it as `omenkbd::apply(...)`, inside namespace omenkbd too. Both arguments are
/// std::array, so an unqualified call also finds std::apply through ADL. That template beats these
/// const-reference parameters unless both arguments are const lvalues, and then fails to compile
/// inside <tuple>.
Frame apply(const Frame &below, const LayerOps &ops);

/// The way a sweep runs across the zones: `Up` from left to right, `Down` from right to left.
enum class Direction { Up, Down };

/// Which sweep step also lights WASD: the one for the Left zone, the one for the Middle zone, or
/// none. WASD stays with that zone when the sweep runs `Down`.
enum class WasdWith { Left, Middle, None };

/// Three steps of `step` each, painting `color`. `Up` lights left, middle, right; `Down` lights
/// right, middle, left. WASD joins the step named by `wasd`.
Clip sweep(Direction dir, Rgb color, WasdWith wasd, Duration step);

/// Paints `color` on `zones` in `blinks` lit phases of `on`, with a dark phase of `off` between
/// each pair: ON (OFF ON)*, so the last phase is lit. A `blinks` below 1 counts as 1, and an empty
/// `zones` gives a clip that shows nothing.
Clip flash(Rgb color, const std::vector<Zone> &zones, int blinks, Duration on, Duration off);

/// One step of `hold` that mixes all four zones `toward_white` (0 to 1) of the way to white.
Clip lift(double toward_white, Duration hold);

/// One step of `hold` that scales all four zones by `factor` (0 to 1).
Clip dip(double factor, Duration hold);

} // namespace omenkbd

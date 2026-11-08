#include "core/clip.hpp"

#include "core/effects.hpp"

#include <algorithm>
#include <cstddef>

namespace omenkbd {

namespace {

/// The first step that ends strictly after `elapsed`, and when it ends. It is the step showing at
/// `elapsed`, so both `ops_at` and `next_boundary` read it. None once the clip is over.
struct Showing {
    const ClipStep *step;
    Duration end;
};

std::optional<Showing> showing(const std::vector<ClipStep> &steps, Duration elapsed) {
    Duration end{};
    for (const ClipStep &step : steps) {
        end += step.hold;
        if (end > elapsed) {
            return Showing{&step, end};
        }
    }
    return std::nullopt;
}

/// The new color of one zone. One overload per ZoneOp alternative and no catch-all, so adding an
/// alternative does not compile until it is handled here.
struct Recolor {
    Rgb below;
    Rgb operator()(const Paint &p) const { return p.color; }
    Rgb operator()(const Mix &m) const { return Rgb::lerp(below, m.toward, m.amount); }
    Rgb operator()(const Scale &s) const { return multiply(below, s.factor); }
};

/// A layer with `op` on every zone in `zones` and none on the others.
LayerOps ops_on(const std::vector<Zone> &zones, const ZoneOp &op) {
    LayerOps ops;
    for (Zone z : zones) {
        ops[index(z)] = op;
    }
    return ops;
}

/// A clip of one step that holds `op` on all four zones.
Clip one_step(const ZoneOp &op, Duration hold) {
    LayerOps ops;
    ops.fill(op);
    return Clip{{ClipStep{ops, hold}}};
}

} // namespace

Duration Clip::total() const {
    Duration sum{};
    for (const ClipStep &step : steps) {
        sum += step.hold;
    }
    return sum;
}

LayerOps Clip::ops_at(Duration elapsed) const {
    const std::optional<Showing> now = showing(steps, elapsed);
    return now ? now->step->ops : LayerOps{};
}

std::optional<Duration> Clip::next_boundary(Duration elapsed) const {
    if (const std::optional<Showing> now = showing(steps, elapsed)) {
        return now->end;
    }
    return std::nullopt;
}

bool empty(const LayerOps &ops) {
    return std::ranges::none_of(ops,
                                [](const std::optional<ZoneOp> &op) { return op.has_value(); });
}

Frame apply(const Frame &below, const LayerOps &ops) {
    Frame out = below;
    for (std::size_t i = 0; i < out.size(); ++i) {
        if (const std::optional<ZoneOp> &op = ops[i]) {
            out[i] = std::visit(Recolor{below[i]}, *op);
        }
    }
    return out;
}

Clip sweep(Direction dir, Rgb color, WasdWith wasd, Duration step) {
    const ZoneOp paint = Paint{color};
    // The steps in the order of an upward sweep. WASD joins the step of its partner zone, so it
    // stays with that zone when the order is reversed.
    std::array<LayerOps, 3> columns{};
    columns[0][index(Zone::Left)] = paint;
    columns[1][index(Zone::Middle)] = paint;
    columns[2][index(Zone::Right)] = paint;
    switch (wasd) {
    case WasdWith::Left:
        columns[0][index(Zone::Wasd)] = paint;
        break;
    case WasdWith::Middle:
        columns[1][index(Zone::Wasd)] = paint;
        break;
    case WasdWith::None:
        break;
    }
    if (dir == Direction::Down) {
        std::ranges::reverse(columns);
    }
    Clip clip;
    for (const LayerOps &ops : columns) {
        clip.steps.push_back(ClipStep{ops, step});
    }
    return clip;
}

Clip flash(Rgb color, const std::vector<Zone> &zones, int blinks, Duration on, Duration off) {
    const ClipStep lit{ops_on(zones, Paint{color}), on};
    const ClipStep dark{LayerOps{}, off};
    Clip clip{{lit}};
    for (int i = 1; i < blinks; ++i) {
        clip.steps.push_back(dark);
        clip.steps.push_back(lit);
    }
    return clip;
}

Clip lift(double toward_white, Duration hold) { return one_step(Mix{kWhite, toward_white}, hold); }

Clip dip(double factor, Duration hold) { return one_step(Scale{factor}, hold); }

} // namespace omenkbd

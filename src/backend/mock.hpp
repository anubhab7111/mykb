#pragma once
// In-memory backend for tests and `--backend mock` demos. Never touches hardware.

#include "backend/backend.hpp"

#include <chrono>
#include <memory>
#include <utility>
#include <vector>

namespace omenkbd {

/// One recorded backend call.
struct MockOp {
    enum class Kind { Zones, Backlight };
    Kind kind;
    std::array<Rgb, 4> zones{};
    bool on = false;

    static MockOp zones_op(const std::array<Rgb, 4> &z) { return {Kind::Zones, z, false}; }
    static MockOp backlight_op(bool on) { return {Kind::Backlight, {}, on}; }
    friend bool operator==(const MockOp &, const MockOp &) = default;
};

struct MockState;

/// Test/demo handle onto a mock backend's shared state (thread-safe).
class MockHandle {
  public:
    explicit MockHandle(std::shared_ptr<MockState> s) : s_(std::move(s)) {}

    /// Every successful call so far, in order.
    [[nodiscard]] std::vector<MockOp> ops() const;
    /// Makes every call fail with `e` until called again with nullopt.
    void fail_with(std::optional<BackendError> e);
    /// Simulates the Fn backlight key: what read_backlight returns from now on.
    void set_hw_backlight(bool on);
    /// Each write sleeps `d` (exercises the writer's coalescing).
    void set_delay(std::chrono::milliseconds d);

  private:
    std::shared_ptr<MockState> s_;
};

/// A mock backend and its handle. With `render`, each zone write prints a truecolor line.
std::pair<std::unique_ptr<Backend>, MockHandle> make_mock(bool render);

} // namespace omenkbd

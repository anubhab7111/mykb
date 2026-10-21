#include "backend/mock.hpp"

#include <format>
#include <iostream>
#include <mutex>
#include <thread>

namespace omenkbd {

struct MockState {
    mutable std::mutex m;
    std::vector<MockOp> ops;
    std::optional<BackendError> fail;
    bool hw_backlight = true;
    std::chrono::milliseconds delay{0};
};

std::vector<MockOp> MockHandle::ops() const {
    std::lock_guard lock(s_->m);
    return s_->ops;
}

void MockHandle::fail_with(std::optional<BackendError> e) {
    std::lock_guard lock(s_->m);
    s_->fail = std::move(e);
}

void MockHandle::set_hw_backlight(bool on) {
    std::lock_guard lock(s_->m);
    s_->hw_backlight = on;
}

void MockHandle::set_delay(std::chrono::milliseconds d) {
    std::lock_guard lock(s_->m);
    s_->delay = d;
}

namespace {

class MockBackend final : public Backend {
  public:
    MockBackend(std::shared_ptr<MockState> s, bool render) : s_(std::move(s)), render_(render) {}

    [[nodiscard]] std::string_view name() const override { return "mock"; }

    std::expected<void, BackendError> write_zones(const std::array<Rgb, 4> &hw) override {
        if (auto r = before_call(); !r) {
            return r;
        }
        {
            std::lock_guard lock(s_->m);
            s_->ops.push_back(MockOp::zones_op(hw));
        }
        if (render_) {
            std::string line = "mock:";
            for (std::size_t i = 0; i < 4; ++i) {
                line += std::format("  hw{} \x1b[48;2;{};{};{}m   \x1b[0m #{}", i, hw[i].r, hw[i].g,
                                    hw[i].b, hw[i].to_hex());
            }
            std::cout << line << std::endl;
        }
        return {};
    }

    std::expected<void, BackendError> set_backlight(bool on) override {
        if (auto r = before_call(); !r) {
            return r;
        }
        {
            std::lock_guard lock(s_->m);
            s_->ops.push_back(MockOp::backlight_op(on));
            s_->hw_backlight = on;
        }
        if (render_) {
            std::cout << "mock: backlight " << (on ? "on" : "off") << std::endl;
        }
        return {};
    }

    std::expected<bool, BackendError> read_backlight() override {
        std::lock_guard lock(s_->m);
        if (s_->fail) {
            return std::unexpected(*s_->fail);
        }
        return s_->hw_backlight;
    }

  private:
    /// Sleeps the configured delay (outside the lock), then returns the configured failure.
    std::expected<void, BackendError> before_call() {
        std::chrono::milliseconds delay{0};
        std::optional<BackendError> fail;
        {
            std::lock_guard lock(s_->m);
            delay = s_->delay;
            fail = s_->fail;
        }
        if (delay.count() > 0) {
            std::this_thread::sleep_for(delay);
        }
        if (fail) {
            return std::unexpected(*fail);
        }
        return {};
    }

    std::shared_ptr<MockState> s_;
    bool render_;
};

} // namespace

std::pair<std::unique_ptr<Backend>, MockHandle> make_mock(bool render) {
    auto state = std::make_shared<MockState>();
    return {std::make_unique<MockBackend>(state, render), MockHandle(state)};
}

} // namespace omenkbd

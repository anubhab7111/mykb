#pragma once
// CPU package temperature from hwmon (coretemp), polled every 2 s. The daemon starts this only
// when a rule uses `cpu_temp_above`.

#include "daemon/event_loop.hpp"
#include "daemon/events.hpp"

#include <filesystem>
#include <functional>
#include <optional>

namespace omenkbd {

/// The `temp*_input` of `class/hwmon/*` whose `name` is `coretemp` and whose `temp*_label` is
/// `Package id 0`.
std::optional<std::filesystem::path> find_package_temp(const std::filesystem::path &sys_root);

/// A hwmon `temp*_input` (millidegrees) in °C.
std::optional<double> read_temp_c(const std::filesystem::path &p);

/// Emits Fact CpuTemp at once and then every 2 s whenever the reading changes. Looks the sensor
/// up again while it is missing (e.g. coretemp loaded late).
class HwmonPoller {
  public:
    using Emit = std::function<void(const Event &)>;

    HwmonPoller(EventLoop &loop, std::filesystem::path sys_root, Emit emit);
    ~HwmonPoller();
    HwmonPoller(const HwmonPoller &) = delete;
    HwmonPoller &operator=(const HwmonPoller &) = delete;

  private:
    void tick();

    EventLoop &loop_;
    std::filesystem::path sys_root_;
    Emit emit_;
    std::optional<std::filesystem::path> input_;
    std::optional<double> last_;
    std::optional<EventLoop::TimerId> timer_;
};

} // namespace omenkbd
